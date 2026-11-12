#include "RTXPTEmbeddedRenderer.hpp"
#include "RTXPTScene.hpp"
#include "RTXPTMaterials.hpp"
#include "RTXPTLights.hpp"
#include "RTXPTLightsBaker.hpp"
#include "RTXPTEnvMapBaker.hpp"
#include "RTXPTSceneFrame.hpp"
#include "RTXPTSceneImporter.hpp"
#include "RTXPTSceneRuntime.hpp"
#include "RTXPTRayTracingPass.hpp"
#include "RTXPTAccumulationPass.hpp"
#include "RTXPTViewPostProcess.hpp"
#include "RTXPTRealtimePipeline.hpp"
#include "RTXPTNrdConfig.hpp"
#include "RTXPTShaderSource.hpp"
#include "RTXPTSuperResolutionInputs.hpp"
#if RTXPT_HAS_RAY_RECONSTRUCTION
#    include "RTXPTRayReconstructionInputs.hpp"
#endif
#include "RenderStateCache.h"
#include "PipelineStateCache.h"
#include "ArchiverFactoryLoader.h"
#include "MapHelper.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace Diligent
{
namespace
{
//Next event estimation of the reference tracer: light candidates sampled per vertex, and how many of them are traced.
constexpr Uint32 kNEECandidateSamples = 5;
constexpr Uint32 kNEEFullSamples      = 1;

#if RTXPT_HAS_RAY_RECONSTRUCTION
RAY_RECONSTRUCTION_QUALITY ToReconstructionQuality(RTXPTRayReconstructionQuality Quality)
{
    switch (Quality)
    {
        case RTXPTRayReconstructionQuality::Native: return RAY_RECONSTRUCTION_QUALITY_NATIVE;
        case RTXPTRayReconstructionQuality::Quality: return RAY_RECONSTRUCTION_QUALITY_QUALITY;
        case RTXPTRayReconstructionQuality::Balanced: return RAY_RECONSTRUCTION_QUALITY_BALANCED;
        case RTXPTRayReconstructionQuality::Performance: return RAY_RECONSTRUCTION_QUALITY_PERFORMANCE;
    }
    return RAY_RECONSTRUCTION_QUALITY_COUNT;
}

//Finds the DLSS RR variant and queries exactly Quality for the display size. The query's [Min, Max] range is enforced
//here because DLSS RR accepts sizes outside it.
RTXPTErrorCode SelectReconstruction(IRayReconstructionFactory* Factory, RTXPTRayReconstructionQuality Quality, RTXPTViewSize DisplaySize, RayReconstructionInfo& Variant, RayReconstructionDesc& Desc)
{
    if (Factory == nullptr || !RTXPTRayReconstructionInputs::FindDLSSVariant(*Factory, Variant))
        return RTXPTErrorCode::RayReconstructionUnavailable;
    RayReconstructionSourceSettingsAttribs Query;
    Query.VariantId    = Variant.VariantId;
    Query.OutputWidth  = DisplaySize.Width;
    Query.OutputHeight = DisplaySize.Height;
    Query.Quality      = ToReconstructionQuality(Quality);
    RayReconstructionSourceSettings Source;
    if (Query.Quality == RAY_RECONSTRUCTION_QUALITY_COUNT || !Factory->GetSourceSettings(Query, Source) ||
        !RTXPTRayReconstructionInputs::MakeDesc(Query, Source, Desc, RTXPTRayReconstructionExposure::Explicit))
        return RTXPTErrorCode::RayReconstructionQueryFailed;
    return RTXPTErrorCode::Ok;
}
#endif
} // namespace

struct RTXPTEmbeddedScene::Impl
{
    //Declared first, so they are released last: the shaders and pipelines created from the caches use them until
    //they are released.
    RefCntAutoPtr<IRenderStateCache>               Cache;
    RefCntAutoPtr<IPipelineStateCache>             PSOCache;
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Shaders;
    RefCntAutoPtr<IRenderDevice>                   Device;
    //The runtime scene keeps the model buffers and material textures its assets reference, so frames can be
    //prepared after the source scene and materials are gone.
    RTXPTSceneRuntime      Runtime;
    RTXPTImportedScene     Imported;
    RTXPTSceneFrameBuilder Frames;
    RTXPTEnvMapBaker       Environment;
    RTXPTEnvMapSettings    EnvironmentSettings;
    std::string            EnvironmentAssetsRoot;
    bool                   Ready{};
    bool                   EnableProfiling{};
    float                  RadianceScale{1.0f};
    RTXPTGpuProfiler       Profiler;

    RTXPTErrorCode UpdateEnvironment(IDeviceContext* Context, bool ForceRebuild = false)
    {
        RTXPTEnvMapBakerUpdateAttribs Update;
        Update.pEngineFactory   = Device->GetEngineFactory();
        Update.AssetsRoot       = EnvironmentAssetsRoot;
        Update.Settings         = EnvironmentSettings;
        Update.pLights          = Frames.GetFrame().pLights;
        Update.ForceRebuild     = ForceRebuild;
        Update.ComputeSupported = true;
        return Environment.Update(Device, Context, Update) ? RTXPTErrorCode::Ok : RTXPTErrorCode::EnvironmentFailed;
    }

    RTXPTErrorCode Prepare(IDeviceContext* Context, const RTXPTSceneSnapshotPtr& Snapshot)
    {
        if (!Context || !Snapshot)
            return Frames.Prepare(Context, Snapshot);
        RTXPTShaderSourceScope  ShaderScope{Shaders};
        auto                    Timing = Profiler.BeginFrame(Device, Context, EnableProfiling, {}, Frames.GetStats().PrepareCount + 1);
        RTXPTGpuProfiler::Scope PrepareTiming{&Profiler, Context, RTXPTGpuProfiler::Stage::ScenePrepare};
        const auto              Code = Frames.Prepare(Context, Snapshot, &Profiler);
        return Code == RTXPTErrorCode::Ok ? UpdateEnvironment(Context) : Code;
    }
};

RTXPTEmbeddedScene::RTXPTEmbeddedScene() :
    m_Impl{std::make_unique<Impl>()} {}
RTXPTEmbeddedScene::~RTXPTEmbeddedScene() = default;

bool RTXPTEmbeddedScene::IsDeviceSupported(IRenderDevice* Device)
{
    if (!Device)
        return false;
    const auto& Features = Device->GetDeviceInfo().Features;
    return Features.RayTracing == DEVICE_FEATURE_STATE_ENABLED && Features.BindlessResources == DEVICE_FEATURE_STATE_ENABLED &&
        Features.ComputeShaders == DEVICE_FEATURE_STATE_ENABLED &&
        (Device->GetAdapterInfo().RayTracing.CapFlags & RAY_TRACING_CAP_FLAG_STANDALONE_SHADERS) != 0;
}

RTXPTErrorCode RTXPTEmbeddedScene::Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedSceneCreateInfo& CreateInfo)
{
    auto& S           = *m_Impl;
    S.EnableProfiling = CreateInfo.EnableProfiling;
    if (!Device || !Context || !CreateInfo.Scene || !CreateInfo.Materials || !CreateInfo.Shaders || !CreateInfo.Cache ||
        !std::isfinite(CreateInfo.RadianceScale) || CreateInfo.RadianceScale <= 0.0f)
        return RTXPTErrorCode::InvalidArgument;
    S.RadianceScale = CreateInfo.RadianceScale;
    auto& Scene     = *CreateInfo.Scene;
    auto& Materials = *CreateInfo.Materials;
    if (!IsDeviceSupported(Device))
        return RTXPTErrorCode::DeviceFeaturesMissing;
    if (Scene.HasSkinnedGeometry())
        return RTXPTErrorCode::SkinnedGeometryUnsupported;
    S.Cache    = CreateInfo.Cache;
    S.PSOCache = CreateInfo.PSOCache;
    S.Shaders  = CreateInfo.Shaders;
    S.Device   = Device;
    RTXPTShaderSourceScope ShaderScope{CreateInfo.Shaders};
    auto*                  Factory = Device->GetEngineFactory();

    if (S.Runtime.BeginUpdate() != RTXPTSceneResult::Ok)
        return RTXPTErrorCode::UpdateFailed;
    RTXPTSceneSnapshotPtr Snapshot;
    const RTXPTErrorCode  ImportCode = ImportRTXPTScene(Device, Context, Scene, Materials, S.Runtime, S.Imported);
    if (ImportCode != RTXPTErrorCode::Ok)
    {
        S.Runtime.CancelUpdate();
        return ImportCode;
    }
    if (S.Runtime.CommitUpdate(Snapshot) != RTXPTSceneResult::Ok)
        return RTXPTErrorCode::UpdateFailed;

    RTXPTSceneFrameBuilderCreateInfo FramesCI;
    FramesCI.pEngineFactory         = Factory;
    FramesCI.pStateCache            = S.Cache;
    FramesCI.pPSOCache              = S.PSOCache;
    const RTXPTErrorCode FramesCode = S.Frames.Initialize(Device, FramesCI);
    if (FramesCode != RTXPTErrorCode::Ok)
        return FramesCode;

    const auto& Graph = Scene.GetSceneGraphData();
    //Only the scene can opt into a background sky; its directional lights are baked over that source or black.
    S.EnvironmentSettings         = RTXPTEnvMapBaker::MakeSceneDefaultSettings(Graph);
    S.EnvironmentSettings.Enabled = std::any_of(Graph.Lights.begin(), Graph.Lights.end(),
                                                [](const auto& Light) { return Light.Type == "EnvironmentLight"; });
    S.EnvironmentAssetsRoot       = Scene.GetAssetsRoot();
    S.Environment.SetFileSource(Scene.GetFileSource());
    if (!S.Environment.CreateResources(Device, Context, Factory, S.Cache, S.PSOCache, true))
        return RTXPTErrorCode::EnvironmentFailed;

    const RTXPTErrorCode PrepareCode = S.Frames.Prepare(Context, Snapshot);
    if (PrepareCode != RTXPTErrorCode::Ok)
        return PrepareCode;
    const auto EnvironmentCode = S.UpdateEnvironment(Context, true);
    if (EnvironmentCode != RTXPTErrorCode::Ok)
        return EnvironmentCode;
    Context->Flush();
    Context->WaitForIdle();
    S.Ready = true;
    return RTXPTErrorCode::Ok;
}

bool RTXPTEmbeddedScene::IsReady() const { return m_Impl->Ready; }

RTXPTErrorCode RTXPTEmbeddedScene::Prepare(IDeviceContext* Context)
{
    if (!m_Impl->Ready)
        return RTXPTErrorCode::NotInitialized;
    return m_Impl->Prepare(Context, m_Impl->Runtime.GetSnapshot());
}

RTXPTErrorCode RTXPTEmbeddedScene::Prepare(IDeviceContext* Context, const RTXPTSceneSnapshotPtr& Snapshot)
{
    if (!m_Impl->Ready)
        return RTXPTErrorCode::NotInitialized;
    return m_Impl->Prepare(Context, Snapshot);
}

RTXPTSceneRuntime& RTXPTEmbeddedScene::GetRuntime() { return m_Impl->Runtime; }

const RTXPTImportedScene& RTXPTEmbeddedScene::GetImportedScene() const { return m_Impl->Imported; }

const RTXPTSceneFrameStats& RTXPTEmbeddedScene::GetFrameStats() const { return m_Impl->Frames.GetStats(); }
ITextureView*               RTXPTEmbeddedScene::GetEnvironmentMapSRV() const { return m_Impl->Environment.GetEnvironmentMapSRV(); }
RTXPTPreparedSceneToken     RTXPTEmbeddedScene::GetPreparedToken() const { return m_Impl->Frames.GetFrame().Prepared; }
RTXPTGpuProfiler&           RTXPTEmbeddedScene::GetProfiler() { return m_Impl->Profiler; }

struct RTXPTEmbeddedView::Impl
{
    //Declared first, so they are released last (see RTXPTEmbeddedScene::Impl).
    RefCntAutoPtr<IRenderStateCache>   Cache;
    RefCntAutoPtr<IPipelineStateCache> PSOCache;
    const RTXPTEmbeddedScene::Impl*    Scene{};
    RefCntAutoPtr<IRenderDevice>       Device;
    RefCntAutoPtr<IDeviceContext>      Context;
    RefCntAutoPtr<IBuffer>             Constants;
    RefCntAutoPtr<ITexture>            Radiance, Depth, Motion, Accumulated, Processed, Output;
    RefCntAutoPtr<ITextureView>        DisplayRTV;
    RTXPTLightsBaker                   LightsBaker;
    RTXPTRayTracingPass                Rays;
    RTXPTAccumulationPass              Accumulation;
    //Reference only: the realtime pipeline keeps its own display post processing.
    RTXPTViewPostProcess  PostProcess;
    RTXPTRealtimePipeline Realtime;
    RTXPTViewUpscaler     Upscaler;
#if RTXPT_HAS_RAY_RECONSTRUCTION
    //RealtimeDLSSRR: likewise for the reconstruction.
    RefCntAutoPtr<IRayReconstructionFactory> ReconstructionFactory;
    RefCntAutoPtr<IRayReconstruction>        Reconstruction;
#endif
    RTXPTViewRayReconstructionInfo RayReconstruction;
    //The used exposure and next state of the latest successful Render with auto exposure; null otherwise.
    RefCntAutoPtr<ITexture>  AutoExposureUsed, AutoExposureState;
    RTXPTPreparedSceneToken  LastScene;
    RTXPTViewHistoryInfo     History;
    RTXPTViewMode            Mode = RTXPTViewMode::Reference;
    RTXPTLightsBakerSettings Lighting;
    PathTracerCameraData     PreviousCamera{};
    PathTracerViewData       PreviousView{};
    double                   PreviousTime{};
    Uint32                   Width{}, Height{}, DisplayWidth{}, DisplayHeight{}, FrameIndex{}, Samples{};
    //The scene frame revisions this view's light sampling and history last followed.
    Uint64           LightingRevision{}, EmissiveRevision{}, ContentRevision{}, EnvironmentRevision{};
    bool             LightsBakerReady{};
    bool             Ready{};
    bool             OutputValid{};
    bool             EnableProfiling{};
    bool             DeferredFrameCommit{};
    bool             PendingFrame{};
    RTXPTGpuProfiler Profiler;

    bool IsRealtime() const { return Mode != RTXPTViewMode::Reference; }
    bool IsDenoised() const { return Mode == RTXPTViewMode::RealtimeNative || Mode == RTXPTViewMode::RealtimeDLSSSR; }
    bool IsReconstructed() const { return Mode == RTXPTViewMode::RealtimeDLSSRR; }

    //The DLSS RR feature of a DLSSRR view; null otherwise.
    IRayReconstruction* GetReconstruction() const
    {
#if RTXPT_HAS_RAY_RECONSTRUCTION
        return Reconstruction;
#else
        return nullptr;
#endif
    }

    //The jitter the DLSS feature of the view requests for its successful frame Index; false without one.
    bool GetFeatureJitter(Uint32 Index, float2& Jitter) const
    {
        Jitter = float2{0, 0};
        if (Upscaler.IsReady())
        {
            Jitter = Upscaler.GetJitter();
            return true;
        }
#if RTXPT_HAS_RAY_RECONSTRUCTION
        if (Reconstruction)
        {
            Reconstruction->GetJitterOffset(Index, Jitter.x, Jitter.y);
            return true;
        }
#endif
        return false;
    }

    void AcceptFrame(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, Uint32 SampleCount, Uint64 Revision, const RTXPTPreparedSceneToken& Prepared)
    {
        History.Scene                         = Prepared;
        History.PreviousViewScene             = LastScene;
        LastScene                             = Prepared;
        const auto& Stats                     = Scene->Frames.GetStats();
        History.ValidPreviousRigidInstances   = Stats.ValidPreviousRigidInstances;
        History.ValidPreviousSkinnedInstances = Stats.ValidPreviousSkinnedInstances;
        History.InvalidHistoryInstances       = Stats.InvalidHistoryInstances;
        History.PreviousTransformStorageBytes = Stats.PreviousTransformStorageBytes;
        History.PreviousSkinnedRangeBytes     = Stats.PreviousSkinnedRangeBytes;
        History.SkinnedVertexStorageBytes     = Stats.SkinnedVertexStorageBytes;
        History.LightFeedbackHistoryFrames    = LightsBaker.GetFeedbackHistoryFrameCount();
        PreviousCamera                        = Camera;
        PreviousView                          = View;
        PreviousTime                          = Time;
        Samples                               = SampleCount;
        ContentRevision                       = Revision;
        if (DeferredFrameCommit)
            PendingFrame = true;
        else
            ++FrameIndex;
        OutputValid = true;
    }

    //Publishes the used exposure and next state of a successful frame with auto exposure.
    void PublishAutoExposure(RTXPTAutoExposure* AutoExposure)
    {
        if (!AutoExposure)
            return;
        AutoExposureUsed  = AutoExposure->GetUsedExposure();
        AutoExposureState = AutoExposure->GetPendingState();
    }

    //The path tracer is compiled for the size of the scene's bindless table, which grows with the scene.
    bool InitializeRays(Uint32 MaterialTextureCount)
    {
        RTXPTShaderSourceScope        ShaderScope{Scene->Shaders};
        RTXPTRayTracingPassCreateInfo RaysCI;
        RaysCI.pEngineFactory               = Device->GetEngineFactory();
        RaysCI.pStateCache                  = Cache;
        RaysCI.pPSOCache                    = PSOCache;
        RaysCI.pFrameConstants              = Constants;
        RaysCI.IndexValueType               = VT_UINT32;
        RaysCI.MaterialTextureCount         = MaterialTextureCount;
        RaysCI.EnableAnyHit                 = true;
        RaysCI.RayTracingSupported          = true;
        RaysCI.StandaloneRTShadersSupported = true;
        RaysCI.NEECandidateSamples          = kNEECandidateSamples;
        RaysCI.NEEFullSamples               = kNEEFullSamples;
        return Rays.Initialize(Device, Context, RaysCI);
    }

    //Points light sampling at the scene's current environment cube and its importance maps.
    void FollowEnvironment()
    {
        const auto& Environment                = Scene->Environment;
        Lighting.EnvMapParams                  = Environment.GetLightsBakerParams();
        Lighting.pEnvironmentRadianceMap       = Environment.GetRadianceMapSRV();
        Lighting.EnvMapImportanceMapResolution = Environment.GetStats().ImportanceResolution;
        Lighting.EnvMapImportanceMapMipCount   = Environment.GetStats().ImportanceMipLevels;
    }

    //Follows the lighting of the scene frame and binds its resources.
    bool BindFrame(const RTXPTSceneFrame& Frame)
    {
        const auto& S                  = *Scene;
        const auto& EnvironmentStats   = S.Environment.GetStats();
        const bool  EnvironmentChanged = EnvironmentRevision != EnvironmentStats.Version;
        if (!LightsBakerReady || Frame.LightingRevision != LightingRevision || EnvironmentChanged)
        {
            Lighting.ResetFeedback = !LightsBakerReady || EnvironmentChanged;
            FollowEnvironment();
            if (!LightsBaker.UpdateBegin(Device, *Frame.pLights, Lighting))
                return false;
            LightsBakerReady    = true;
            LightingRevision    = Frame.LightingRevision;
            EmissiveRevision    = Frame.EmissiveRevision;
            EnvironmentRevision = EnvironmentStats.Version;
        }
        else if (Frame.EmissiveRevision != EmissiveRevision)
        {
            //The emissive triangles were rebuilt in place; refresh the light buffers and sampling weights.
            LightsBaker.RequestProxyRebuild();
            EmissiveRevision = Frame.EmissiveRevision;
        }
        if (Rays.GetStats().MaterialTextureCount != Frame.MaterialTextureCount && !InitializeRays(Frame.MaterialTextureCount))
            return false;

        RTXPTRayTracingBindings Bindings;
        Bindings.pTLAS                         = Frame.pTLAS;
        Bindings.TLASBuildIndex                = Frame.BuildIndex;
        Bindings.pMaterialBuffer               = Frame.pMaterialBuffer;
        Bindings.pSubInstanceBuffer            = Frame.pSubInstanceBuffer;
        Bindings.pVertexBuffer                 = Frame.pVertexBuffer;
        Bindings.pSkinnedVertexBuffer          = Frame.pSkinnedVertexBuffer;
        Bindings.pIndexBuffer                  = Frame.pIndexBuffer;
        Bindings.pMaterialTextures             = Frame.ppMaterialTextures;
        Bindings.MaterialTextureCount          = Frame.MaterialTextureCount;
        Bindings.pLightBuffer                  = LightsBaker.GetLightBuffer();
        Bindings.pLightExBuffer                = LightsBaker.GetLightExBuffer();
        Bindings.pLightingControlBuffer        = LightsBaker.GetControlBuffer();
        Bindings.pLightProxyCounters           = LightsBaker.GetLightProxyCounters();
        Bindings.pLightSamplingProxies         = LightsBaker.GetLightSamplingProxies();
        Bindings.pLocalSamplingBuffer          = LightsBaker.GetLocalSamplingBuffer();
        Bindings.pEnvironmentLightLookup       = LightsBaker.GetEnvironmentLightLookup();
        Bindings.pFeedbackTotalWeightUAV       = LightsBaker.GetFeedbackTotalWeightUAV();
        Bindings.pFeedbackCandidatesUAV        = LightsBaker.GetFeedbackCandidatesUAV();
        Bindings.pEnvironmentMapSRV            = S.Environment.GetEnvironmentMapSRV();
        Bindings.pEnvironmentImportanceMapSRV  = S.Environment.GetImportanceMapSRV();
        Bindings.pEnvironmentRadianceMapSRV    = S.Environment.GetRadianceMapSRV();
        Bindings.pEnvironmentSampler           = S.Environment.GetEnvironmentSampler();
        Bindings.pEnvironmentImportanceSampler = S.Environment.GetImportanceSampler();
        return Rays.Bind(Context, Bindings);
    }

    //Selects the DLSS variant, queries exactly the requested quality for the display size and creates the feature for
    //the queried input size, which becomes the render size. Nothing is created for an unsupported query.
    RTXPTErrorCode CreateUpscaler(ISuperResolutionFactory* Factory, RTXPTUpscaleQuality Quality, RTXPTSuperResolutionProvider Provider)
    {
        RTXPTViewUpscalerCreateInfo CreateInfo;
        CreateInfo.Factory     = Factory;
        CreateInfo.Quality     = Quality;
        CreateInfo.Provider    = Provider;
        CreateInfo.DisplaySize = {DisplayWidth, DisplayHeight};
        const auto Code        = Upscaler.Initialize(CreateInfo);
        if (Code != RTXPTErrorCode::Ok)
            return Code;
        Width  = Upscaler.GetInfo().RenderSize.Width;
        Height = Upscaler.GetInfo().RenderSize.Height;
        return RTXPTErrorCode::Ok;
    }

    //Like CreateUpscaler for the DLSS RR variant (SelectReconstruction), with explicit exposure (D4) and the Core
    //default preset (D5).
    RTXPTErrorCode CreateReconstruction(IRayReconstructionFactory* Factory, RTXPTRayReconstructionQuality Quality)
    {
#if RTXPT_HAS_RAY_RECONSTRUCTION
        RayReconstructionInfo Variant;
        RayReconstructionDesc Desc;
        const auto            Code = SelectReconstruction(Factory, Quality, {DisplayWidth, DisplayHeight}, Variant, Desc);
        if (Code != RTXPTErrorCode::Ok)
            return Code;
        Factory->CreateRayReconstruction(Desc, &Reconstruction);
        if (!Reconstruction)
            return RTXPTErrorCode::RayReconstructionCreateFailed;
        ReconstructionFactory         = Factory;
        Width                         = Desc.InputWidth;
        Height                        = Desc.InputHeight;
        RayReconstruction.Variant     = Variant.Name;
        RayReconstruction.Quality     = Quality;
        RayReconstruction.Preset      = RTXPTRayReconstructionInputs::GetDLSSPresetName(Desc.DLSSPreset);
        RayReconstruction.RenderSize  = {Width, Height};
        RayReconstruction.DisplaySize = {DisplayWidth, DisplayHeight};
        return RTXPTErrorCode::Ok;
#else
        (void)Factory, (void)Quality;
        return RTXPTErrorCode::RayReconstructionUnavailable;
#endif
    }

    bool CreateTarget(const char* Name, TEXTURE_FORMAT Format, BIND_FLAGS Flags, RefCntAutoPtr<ITexture>& Target)
    {
        return CreateTarget(Name, Format, Flags, Width, Height, Target);
    }

    bool CreateTarget(const char* Name, TEXTURE_FORMAT Format, BIND_FLAGS Flags, Uint32 TargetWidth, Uint32 TargetHeight, RefCntAutoPtr<ITexture>& Target)
    {
        TextureDesc Desc;
        Desc.Name      = Name;
        Desc.Type      = RESOURCE_DIM_TEX_2D;
        Desc.Width     = TargetWidth;
        Desc.Height    = TargetHeight;
        Desc.MipLevels = 1;
        Desc.Format    = Format;
        Desc.BindFlags = Flags | BIND_SHADER_RESOURCE;
        Device->CreateTexture(Desc, nullptr, &Target);
        return Target != nullptr;
    }
};

RTXPTEmbeddedView::RTXPTEmbeddedView() :
    m_Impl{std::make_unique<Impl>()} {}
RTXPTEmbeddedView::~RTXPTEmbeddedView() = default;

RTXPTErrorCode RTXPTEmbeddedView::Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedViewCreateInfo& CreateInfo)
{
    return Initialize(Device, Context, CreateInfo, nullptr);
}

RTXPTErrorCode RTXPTEmbeddedView::QueryRayReconstruction(IRayReconstructionFactory* Factory, RTXPTRayReconstructionQuality Quality, RTXPTViewSize DisplaySize)
{
#if RTXPT_HAS_RAY_RECONSTRUCTION
    RayReconstructionInfo Variant;
    RayReconstructionDesc Desc;
    return SelectReconstruction(Factory, Quality, DisplaySize, Variant, Desc);
#else
    (void)Factory, (void)Quality, (void)DisplaySize;
    return RTXPTErrorCode::RayReconstructionUnavailable;
#endif
}

RTXPTErrorCode RTXPTEmbeddedView::RetryInitialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedViewCreateInfo& CreateInfo)
{
    auto&       Upscaler = m_Impl->Upscaler;
    const auto& Info     = Upscaler.GetInfo();
    if (CreateInfo.Mode != RTXPTViewMode::RealtimeDLSSSR || !Upscaler.HasSelection() || Info.Quality != CreateInfo.UpscaleQuality ||
        Info.Provider != CreateInfo.SuperResolutionProvider || Info.DisplaySize.Width != CreateInfo.DisplaySize.Width || Info.DisplaySize.Height != CreateInfo.DisplaySize.Height)
        return RTXPTErrorCode::InvalidArgument;
    const auto Code = Upscaler.RetryCreate();
    if (Code != RTXPTErrorCode::Ok)
        return Code;
    auto Selected = std::move(Upscaler);
    return Initialize(Device, Context, CreateInfo, &Selected);
}

RTXPTErrorCode RTXPTEmbeddedView::Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedViewCreateInfo& CreateInfo, RTXPTViewUpscaler* SelectedUpscaler)
{
    m_Impl                      = std::make_unique<Impl>();
    auto& R                     = *m_Impl;
    R.EnableProfiling           = CreateInfo.EnableProfiling;
    R.DeferredFrameCommit       = CreateInfo.DeferredFrameCommit;
    const bool bSuperResolution = CreateInfo.Mode == RTXPTViewMode::RealtimeDLSSSR;
    const bool bReconstruction  = CreateInfo.Mode == RTXPTViewMode::RealtimeDLSSRR;
    //The DLSS modes render at the size their query reports for the display size.
    const bool bQueriedSize = bSuperResolution || bReconstruction;
    if (!Device || !Context || !CreateInfo.Shaders || !CreateInfo.Cache || !CreateInfo.Scene || !CreateInfo.Scene->IsReady() ||
        !CreateInfo.DisplaySize.Width || !CreateInfo.DisplaySize.Height ||
        (!bQueriedSize && (!CreateInfo.RenderSize.Width || !CreateInfo.RenderSize.Height)))
        return RTXPTErrorCode::InvalidArgument;
    const bool bRenderSizeValid = bQueriedSize ?
        CreateInfo.RenderSize.Width == 0 && CreateInfo.RenderSize.Height == 0 :
        CreateInfo.RenderSize.Width == CreateInfo.DisplaySize.Width && CreateInfo.RenderSize.Height == CreateInfo.DisplaySize.Height;
    if (!bRenderSizeValid || CreateInfo.DisplayRTVFormat != TEX_FORMAT_RGBA8_UNORM_SRGB ||
        (CreateInfo.Mode != RTXPTViewMode::Reference && CreateInfo.Mode != RTXPTViewMode::RealtimeRaw &&
         CreateInfo.Mode != RTXPTViewMode::RealtimeNative && !bQueriedSize))
        return RTXPTErrorCode::UnsupportedViewConfiguration;
    R.Mode = CreateInfo.Mode;
    if (R.IsDenoised() && !RTXPTIsNrdAvailable())
        return RTXPTErrorCode::DenoiserUnavailable;
    R.DisplayWidth  = CreateInfo.DisplaySize.Width;
    R.DisplayHeight = CreateInfo.DisplaySize.Height;
    R.Width         = CreateInfo.RenderSize.Width;
    R.Height        = CreateInfo.RenderSize.Height;
    if (bSuperResolution)
    {
        if (SelectedUpscaler)
        {
            R.Upscaler      = std::move(*SelectedUpscaler);
            const auto Size = R.Upscaler.GetInfo().RenderSize;
            R.Width         = Size.Width;
            R.Height        = Size.Height;
        }
        else
        {
            const auto Code = R.CreateUpscaler(CreateInfo.SuperResolutionFactory, CreateInfo.UpscaleQuality, CreateInfo.SuperResolutionProvider);
            if (Code != RTXPTErrorCode::Ok)
                return Code;
        }
    }
    if (bReconstruction)
    {
        const auto Code = R.CreateReconstruction(CreateInfo.RayReconstructionFactory, CreateInfo.ReconstructionQuality);
        if (Code != RTXPTErrorCode::Ok)
            return Code;
    }
    const Uint32 Width = R.Width, Height = R.Height;
    const auto&  S = *CreateInfo.Scene->m_Impl;
    R.Cache        = CreateInfo.Cache;
    R.PSOCache     = CreateInfo.PSOCache;
    R.Scene        = &S;
    R.Device       = Device;
    R.Context      = Context;
    RTXPTShaderSourceScope ShaderScope{CreateInfo.Shaders};
    auto*                  Factory = Device->GetEngineFactory();
    BufferDesc             ConstantsDesc;
    ConstantsDesc.Name           = "RTXPT embedded frame constants";
    ConstantsDesc.Size           = sizeof(SampleConstants);
    ConstantsDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage          = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    Device->CreateBuffer(ConstantsDesc, nullptr, &R.Constants);
    if (!R.Constants)
        return RTXPTErrorCode::FrameTargetsFailed;
    if (R.Mode == RTXPTViewMode::Reference &&
        (!R.CreateTarget("RTXPT radiance", TEX_FORMAT_RGBA16_FLOAT, BIND_UNORDERED_ACCESS, R.Radiance) ||
         !R.CreateTarget("RTXPT depth", TEX_FORMAT_R32_FLOAT, BIND_UNORDERED_ACCESS, R.Depth) ||
         !R.CreateTarget("RTXPT motion", TEX_FORMAT_RGBA16_FLOAT, BIND_UNORDERED_ACCESS, R.Motion) ||
         !R.CreateTarget("RTXPT accumulated", TEX_FORMAT_RGBA32_FLOAT, BIND_UNORDERED_ACCESS, R.Accumulated) ||
         !R.CreateTarget("RTXPT processed", TEX_FORMAT_RGBA16_FLOAT, BIND_UNORDERED_ACCESS, R.Processed)))
        return RTXPTErrorCode::FrameTargetsFailed;
    if (!CreateRTXPTDisplayTarget(Device, R.DisplayWidth, R.DisplayHeight, "RTXPT display output", &R.Output, &R.DisplayRTV))
        return RTXPTErrorCode::FrameTargetsFailed;

    //Light sampling keeps screen-sized feedback and reads this view's depth and motion every frame, so it is per view.
    R.Lighting.ViewportSize = R.Lighting.PrevViewportSize = float2{float(Width), float(Height)};
    R.FollowEnvironment();
    RTXPTLightsBakerCreateInfo LightsBakerCI;
    LightsBakerCI.pEngineFactory   = Factory;
    LightsBakerCI.pStateCache      = R.Cache;
    LightsBakerCI.pPSOCache        = R.PSOCache;
    LightsBakerCI.Width            = Width;
    LightsBakerCI.Height           = Height;
    LightsBakerCI.ComputeSupported = true;
    if (!R.LightsBaker.CreateResources(Device, LightsBakerCI))
        return RTXPTErrorCode::LightSamplingSetupFailed;

    const RTXPTSceneFrame& Frame = S.Frames.GetFrame();
    if (!Frame.pTLAS || !R.InitializeRays(Frame.MaterialTextureCount) || !R.BindFrame(Frame))
        return RTXPTErrorCode::RayTracingSetupFailed;
    if (R.IsRealtime())
    {
        RTXPTRealtimePipelineCreateInfo RealtimeCI;
        RealtimeCI.Device                        = Device;
        RealtimeCI.Cache                         = R.Cache;
        RealtimeCI.PSOCache                      = R.PSOCache;
        RealtimeCI.Constants                     = R.Constants;
        RealtimeCI.Width                         = Width;
        RealtimeCI.Height                        = Height;
        RealtimeCI.DisplayFormat                 = CreateInfo.DisplayRTVFormat;
        const auto NativeDenoiser                = CreateInfo.NativeDenoiser == RTXPTNrdMethod::RELAX ? RTXPTRealtimeDenoiser::NrdRelax : RTXPTRealtimeDenoiser::NrdReblur;
        RealtimeCI.Denoiser                      = R.IsDenoised() ? NativeDenoiser : RTXPTRealtimeDenoiser::None;
        RealtimeCI.SunShadowDenoising            = CreateInfo.NativeSunShadowDenoising;
        RealtimeCI.Reconstructor                 = bReconstruction ? RTXPTRealtimeReconstructor::DLSSRR : RTXPTRealtimeReconstructor::None;
        RealtimeCI.HistoryConfidence             = CreateInfo.NativeHistoryConfidence;
        RealtimeCI.ReconstructionDisplayExposure = CreateInfo.ReconstructionDisplayExposure;
        RealtimeCI.SuperResolution               = bSuperResolution;
        RealtimeCI.TemporalSuperResolution       = R.Upscaler.IsTemporal();
        RealtimeCI.DisplayWidth                  = bQueriedSize ? R.DisplayWidth : 0;
        RealtimeCI.DisplayHeight                 = bQueriedSize ? R.DisplayHeight : 0;
        const auto Code                          = R.Realtime.Initialize(RealtimeCI);
        if (Code != RTXPTErrorCode::Ok)
            return Code;
        //Native and DLSSSR publish the merged output as DenoisedRadiance, never as current-frame noisy radiance.
        if (R.Mode == RTXPTViewMode::RealtimeRaw)
            R.Radiance = R.Realtime.GetTargets().GetOutputColorSRV()->GetTexture();
    }
    else if (!R.Accumulation.Initialize(Device, Factory, R.Cache, R.PSOCache, true) ||
             !R.PostProcess.Initialize(Device, R.Cache, R.PSOCache, Width, Height, CreateInfo.DisplayRTVFormat))
        return RTXPTErrorCode::PostProcessSetupFailed;
    Context->Flush();
    Context->WaitForIdle();
    R.Ready = true;
    return RTXPTErrorCode::Ok;
}

RTXPTErrorCode RTXPTEmbeddedView::Render(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, bool ResetHistory, const RTXPTViewDisplaySettings& Display, RTXPTAutoExposure* AutoExposure)
{
    auto& R = *m_Impl;
    if (R.PendingFrame)
        InvalidateOutput();
    RTXPTViewUpscalerFrame UpscalerFrame{R.Upscaler.IsReady() ? &R.Upscaler : nullptr};
    ResetHistory  = ResetHistory || !R.OutputValid;
    R.OutputValid = false;
    R.History     = {};
    R.AutoExposureUsed.Release();
    R.AutoExposureState.Release();
    R.Realtime.InvalidateOutput();
    if (!R.Ready)
        return RTXPTErrorCode::NotInitialized;
    const bool bAuto = Display.AutoExposure.Enabled;
    if (!IsValidRTXPTViewDisplaySettings(Display) || (bAuto && (!AutoExposure || !AutoExposure->IsReady())))
        return RTXPTErrorCode::InvalidArgument;
    const auto ToneParameters = MakeRTXPTToneMappingParameters(Display);
    if (Camera.ViewportWidth != R.Width || Camera.ViewportHeight != R.Height ||
        View.ViewportSize.x != float(R.Width) || View.ViewportSize.y != float(R.Height))
        return RTXPTErrorCode::InvalidArgument;
    //DLSSSR and DLSSRR alone jitter the camera, once, by the sample their feature requests for this successful frame.
    PathTracerCameraData FrameCamera = Camera;
    PathTracerViewData   FrameView   = View;
    float2               Jitter{0, 0};
    if (R.GetFeatureJitter(R.FrameIndex, Jitter))
    {
        if (Camera.Jitter != float2{0, 0} || View.PixelOffset != float2{0, 0})
            return RTXPTErrorCode::InvalidArgument;
        RTXPTSuperResolutionInputs::ApplyJitter(Jitter, FrameCamera, FrameView);
    }
    const auto&            S     = *R.Scene;
    const RTXPTSceneFrame& Frame = S.Frames.GetFrame();
    if (!Frame.pTLAS || !Frame.Prepared.IsValid())
        return RTXPTErrorCode::FrameNotPrepared;
    if (!S.Environment.GetStats().Ready)
        return RTXPTErrorCode::EnvironmentFailed;
    const bool CameraCorrespondenceValid = !ResetHistory && R.LastScene.IsPreviousOf(Frame.Prepared);
    if (R.IsRealtime())
        ResetHistory = !CameraCorrespondenceValid;
    if (!R.BindFrame(Frame))
        return RTXPTErrorCode::SceneBindingsFailed;
    //The exposure of the frame is written before anything reads it; a later failure aborts the frame's meter.
    RTXPTAutoExposure* const FrameAutoExposure = bAuto ? AutoExposure : nullptr;
    RTXPTAutoExposureFrame   AutoExposureGuard{FrameAutoExposure};
    if (FrameAutoExposure)
    {
        const auto BeginCode = FrameAutoExposure->BeginFrame(R.Context, Display.AutoExposure, Time);
        if (BeginCode != RTXPTErrorCode::Ok)
            return BeginCode;
    }
    //Reference accumulation restarts on any camera/projection or scene content change. The host additionally resets
    //cuts, pipeline changes and discontinuities; successful executed frames alone advance history.
    if (R.Mode == RTXPTViewMode::Reference)
        ResetHistory = ResetHistory || !R.Samples || std::memcmp(&Camera, &R.PreviousCamera, sizeof(Camera)) != 0 ||
            std::memcmp(&View, &R.PreviousView, sizeof(View)) != 0 || Frame.Snapshot->ContentRevision != R.ContentRevision;
    const Uint32    Samples = ResetHistory ? 1u : R.Samples + 1u;
    SampleConstants Constants;
    Constants.view                      = FrameView;
    Constants.previousView              = ResetHistory ? FrameView : R.PreviousView;
    Constants.camera                    = FrameCamera;
    Constants.viewProj                  = View.MatWorldToClip;
    Constants.viewProjInv               = View.MatClipToWorldNoOffset;
    Constants.cameraPositionAndTime     = float4{Camera.PosW, float(Time)};
    Constants.viewportSizeAndFrameIndex = float4{float(R.Width), float(R.Height), 1.0f / R.Width, float(R.FrameIndex)};
    auto& PT                            = Constants.ptConsts;
    PT.imageWidth                       = R.Width;
    PT.imageHeight                      = R.Height;
    PT.frameIndex                       = R.FrameIndex;
    PT.sampleBaseIndex                  = Samples;
    PT.sampleIndex                      = Samples;
    PT.resetAccumulation                = ResetHistory;
    PT.camera                           = FrameCamera;
    PT.prevCamera                       = ResetHistory ? FrameCamera : R.PreviousCamera;
    PT.analyticLightCount               = Frame.pLights->GetStats().LightCount;
    PT.NEEType                          = R.Lighting.ImportanceSamplingType;
    PT.environmentNEEEnabled            = (Frame.pLights->GetEmissiveTriangleCount() << 1u) | (S.Environment.GetLightsBakerParams().Enabled > 0.0f ? 1u : 0u);
    Constants.envMap                    = S.Environment.GetConstants();
    Constants.sun                       = MakeRTXPTSunConstants(*Frame.pLights);
    if (R.IsRealtime())
    {
        // Build supplies per-pixel path validity. Also reset the shared light proposals conservatively when
        // actual scene instances lack correspondence (including missing/recovered parts). Native's denoiser history
        // follows the camera correspondence alone; instances without correspondence lose it per pixel instead.
        const bool FeedbackReset = ResetHistory || Frame.InvalidHistoryInstances != 0;
        if (FeedbackReset)
            R.LightsBaker.RequestFeedbackReset();
        PT.sampleBaseIndex = PT.sampleIndex = R.FrameIndex;
        //The realtime layers, clamps and denoiser inputs are in the working units of the scene; Reference traces in
        //scene units.
        PT.radianceScale = S.RadianceScale;
        RTXPTRealtimeHistoryAttribs RealtimeHistory;
        RealtimeHistory.Reset                 = ResetHistory;
        RealtimeHistory.TimeDeltaSeconds      = !ResetHistory && Time > R.PreviousTime ? static_cast<float>(Time - R.PreviousTime) : -1.0f;
        RealtimeHistory.Upscaler              = R.Upscaler.IsReady() ? &R.Upscaler : nullptr;
        RealtimeHistory.InvalidSurfaceHistory = R.Scene->Frames.GetStats().InvalidHistoryInstances != 0;
        RealtimeHistory.Reconstruction        = R.GetReconstruction();
        auto        Timing                    = R.Profiler.BeginFrame(R.Device, R.Context, R.EnableProfiling, {}, R.FrameIndex + 1);
        const auto& Stats                     = R.Realtime.GetStats();
        const auto  ReconstructedBefore       = Stats.RayReconstructionExecutes;
        const auto  Code                      = R.Realtime.Render(R.Context, R.Rays, R.LightsBaker, Constants, ToneParameters, Display.Bloom, FrameAutoExposure, RealtimeHistory, R.DisplayRTV, &R.Profiler);
        if (Code == RTXPTErrorCode::RayReconstructionInputsFailed || Code == RTXPTErrorCode::RayReconstructionExecuteFailed)
        {
            ++R.RayReconstruction.Failures;
            //DLSS RR likewise creates its feature with the first Execute.
            if (Code == RTXPTErrorCode::RayReconstructionExecuteFailed && ReconstructedBefore == 0)
                return RTXPTErrorCode::RayReconstructionCreateFailed;
        }
        if (Code != RTXPTErrorCode::Ok)
            return Code;
        R.History.CameraCorrespondenceValid = CameraCorrespondenceValid;
        R.History.LightFeedbackReset        = FeedbackReset;
        if (R.IsReconstructed())
        {
            R.RayReconstruction.Jitter       = Jitter;
            R.RayReconstruction.LastReset    = ResetHistory;
            R.RayReconstruction.Exposure     = Stats.RayReconstructionExposure;
            R.RayReconstruction.AutoExposure = Stats.RayReconstructionAutoExposure;
            R.RayReconstruction.ForwardSign  = Stats.RayReconstructionForwardSign;
            R.RayReconstruction.Executes     = Stats.RayReconstructionExecutes;
            R.RayReconstruction.Resets       = Stats.RayReconstructionResets;
        }
        R.AcceptFrame(FrameCamera, FrameView, Time, 0, Frame.Snapshot->ContentRevision, Frame.Prepared);
        R.PublishAutoExposure(FrameAutoExposure);
        UpscalerFrame.Succeed(!R.DeferredFrameCommit);
        AutoExposureGuard.Succeed();
        return RTXPTErrorCode::Ok;
    }
    {
        MapHelper<SampleConstants> Mapped{R.Context, R.Constants, MAP_WRITE, MAP_FLAG_DISCARD};
        if (!Mapped)
            return RTXPTErrorCode::ConstantsUpdateFailed;
        *Mapped = Constants;
    }
    RTXPTRayTracingDispatch Trace;
    Trace.pOutputColorUAV   = R.Radiance->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    Trace.pDepthUAV         = R.Depth->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    Trace.pMotionVectorsUAV = R.Motion->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    Trace.Width             = R.Width;
    Trace.Height            = R.Height;
    R.LightsBaker.UpdateFrame(View.MatWorldToClip, Camera.PosW);
    if (!R.LightsBaker.PrepareLightBuffers(R.Context) ||
        !R.LightsBaker.UpdateEnd(R.Context, R.Depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), R.Motion->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)) ||
        !R.Rays.Trace(R.Context, Trace))
        return RTXPTErrorCode::TraceFailed;
    RTXPTAccumulationDispatch Accumulation;
    Accumulation.pInputColorSRV          = R.Radiance->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    Accumulation.pAccumulatedRadianceUAV = R.Accumulated->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    Accumulation.pProcessedOutputUAV     = R.Processed->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    Accumulation.InputWidth = Accumulation.OutputWidth = R.Width;
    Accumulation.InputHeight = Accumulation.OutputHeight = R.Height;
    Accumulation.BlendFactor                             = 1.0f / Samples;
    if (!R.Accumulation.Render(R.Context, Accumulation))
        return RTXPTErrorCode::AccumulationFailed;
    //The display post processing meters, blooms and tone maps the processed radiance, the accumulated HDR, and leaves it
    //and the accumulated radiance unchanged.
    const auto PostCode = R.PostProcess.Apply(R.Context, R.Processed, ToneParameters, Display.Bloom, FrameAutoExposure, R.DisplayRTV);
    if (PostCode != RTXPTErrorCode::Ok)
        return PostCode;
    R.AcceptFrame(Camera, View, Time, Samples, Frame.Snapshot->ContentRevision, Frame.Prepared);
    R.PublishAutoExposure(FrameAutoExposure);
    AutoExposureGuard.Succeed();
    return RTXPTErrorCode::Ok;
}

void RTXPTEmbeddedView::CommitFrame()
{
    auto& R = *m_Impl;
    if (R.PendingFrame)
    {
        ++R.FrameIndex;
        R.PendingFrame = false;
        R.Upscaler.CommitFrame();
    }
}

void RTXPTEmbeddedView::InvalidateOutput()
{
    auto& R       = *m_Impl;
    R.OutputValid = false;
    R.History     = {};
    R.LastScene   = {};
    R.Samples     = 0;
    R.AutoExposureUsed.Release();
    R.AutoExposureState.Release();
    R.Realtime.InvalidateOutput();
    R.Upscaler.InvalidateFrame();
    R.PendingFrame = false;
}

ITexture*           RTXPTEmbeddedView::GetOutput() const { return m_Impl->Output; }
RTXPTViewOutputInfo RTXPTEmbeddedView::GetOutputInfo(RTXPTViewOutput Output, Uint32 Layer) const
{
    const auto&         R = *m_Impl;
    RTXPTViewOutputInfo Info;
    ITexture*           Texture         = nullptr;
    const auto&         Targets         = R.Realtime.GetTargets();
    const bool          bNative         = R.IsDenoised();
    const bool          bUpscaled       = R.Mode == RTXPTViewMode::RealtimeDLSSSR;
    const bool          bReconstruction = R.IsReconstructed();
    const auto&         SRInputs        = R.Realtime.GetSuperResolutionInputs();
    const auto          TextureOf       = [](ITextureView* View) -> ITexture* {
        return View ? View->GetTexture() : nullptr;
    };
    //Ray reconstruction's input and output textures; null for the other modes.
    const auto ReconstructionTexture = [&](ITextureView* View) -> ITexture* {
        return bReconstruction ? TextureOf(View) : nullptr;
    };
    if (Layer != 0 && (Output != RTXPTViewOutput::LayerHistoryConfidence || Layer >= kRTXPTDenoisingLayerCount))
        return Info;
    switch (Output)
    {
        case RTXPTViewOutput::Display:
            Texture       = R.Output;
            Info.Encoding = RTXPTOutputEncoding::SRGB;
            break;
        case RTXPTViewOutput::NoisyRadiance: Texture = R.Radiance; break;
        case RTXPTViewOutput::AccumulatedRadiance: Texture = R.Accumulated; break;
        case RTXPTViewOutput::DenoisedRadiance: Texture = bNative ? TextureOf(Targets.GetOutputColorSRV()) : nullptr; break;
        case RTXPTViewOutput::RayDistance: Texture = TextureOf(Targets.GetDepthSRV()); break;
        case RTXPTViewOutput::PrimaryMotion: Texture = TextureOf(Targets.GetScreenMotionVectorsSRV()); break;
        case RTXPTViewOutput::SpecularMotion: Texture = TextureOf(Targets.GetSpecularMotionSRV()); break;
        case RTXPTViewOutput::SpecularHitDistance: Texture = TextureOf(Targets.GetSpecularHitTSRV()); break;
        case RTXPTViewOutput::AverageLayerRadiance: Texture = TextureOf(Targets.GetDenoiserAvgLayerRadianceHalfResSRV()); break;
        case RTXPTViewOutput::LayerHeader: Texture = TextureOf(Targets.GetDenoisingLayersHeaderSRV()); break;
        case RTXPTViewOutput::LayerHistoryConfidence: Texture = bNative ? TextureOf(Targets.GetDenoiserHistoryConfidenceSRV(Layer)) : nullptr; break;
        case RTXPTViewOutput::UpscaledRadiance: Texture = bUpscaled ? TextureOf(Targets.GetProcessedOutputColorSRV()) : nullptr; break;
        case RTXPTViewOutput::SuperResolutionDepth: Texture = bUpscaled ? TextureOf(SRInputs.GetDepthSRV()) : nullptr; break;
        case RTXPTViewOutput::SuperResolutionMotion: Texture = bUpscaled ? TextureOf(SRInputs.GetMotionVectorsSRV()) : nullptr; break;
        case RTXPTViewOutput::SuperResolutionIgnoreHistoryMask: Texture = bUpscaled ? TextureOf(SRInputs.GetIgnoreHistoryMaskSRV()) : nullptr; break;
        case RTXPTViewOutput::ReconstructedRadiance: Texture = ReconstructionTexture(Targets.GetProcessedOutputColorSRV()); break;
        case RTXPTViewOutput::RayReconstructionColor: Texture = ReconstructionTexture(Targets.GetOutputColorSRV()); break;
        case RTXPTViewOutput::RayReconstructionDepth: Texture = ReconstructionTexture(Targets.GetRRDepthSRV()); break;
        case RTXPTViewOutput::RayReconstructionMotion: Texture = ReconstructionTexture(Targets.GetRRMotionVectorsSRV()); break;
        case RTXPTViewOutput::RayReconstructionSpecularMotion: Texture = ReconstructionTexture(Targets.GetRRSpecMotionVectorsSRV()); break;
        case RTXPTViewOutput::RayReconstructionNormalRoughness: Texture = ReconstructionTexture(Targets.GetRRNormalsAndRoughnessSRV()); break;
        case RTXPTViewOutput::RayReconstructionDiffuseAlbedo: Texture = ReconstructionTexture(Targets.GetRRDiffuseAlbedoSRV()); break;
        case RTXPTViewOutput::RayReconstructionSpecularAlbedo: Texture = ReconstructionTexture(Targets.GetRRSpecAlbedoSRV()); break;
        case RTXPTViewOutput::RayReconstructionExposure: Texture = bReconstruction ? R.Realtime.GetReconstructionExposure() : nullptr; break;
        case RTXPTViewOutput::BloomRadiance: Texture = R.IsRealtime() ? R.Realtime.GetBloom().GetOutput() : R.PostProcess.GetBloom().GetOutput(); break;
        case RTXPTViewOutput::AutoExposureUsed: Texture = R.AutoExposureUsed; break;
        case RTXPTViewOutput::AutoExposureState: Texture = R.AutoExposureState; break;
        default: return Info;
    }
    if (!Texture)
        return Info;
    const auto& Desc    = Texture->GetDesc();
    Info.Size           = {Desc.Width, Desc.Height};
    Info.ResourceFormat = Desc.Format;
    Info.SRVFormat      = Texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)->GetDesc().Format;
    Info.Valid          = R.OutputValid;
    Info.Texture        = Info.Valid ? Texture : nullptr;
    Info.FrameIndex     = Info.Valid ? R.FrameIndex + (R.PendingFrame ? 1u : 0u) : 0;
    return Info;
}
const RTXPTViewHistoryInfo&           RTXPTEmbeddedView::GetHistoryInfo() const { return m_Impl->History; }
RTXPTGpuProfiler&                     RTXPTEmbeddedView::GetProfiler() { return m_Impl->Profiler; }
Uint32                                RTXPTEmbeddedView::GetTraceCount() const { return m_Impl->Rays.GetStats().TraceCount; }
Uint32                                RTXPTEmbeddedView::GetAccumulationCount() const { return m_Impl->Samples; }
RTXPTViewMode                         RTXPTEmbeddedView::GetMode() const { return m_Impl->Mode; }
RTXPTViewSize                         RTXPTEmbeddedView::GetRenderSize() const { return {m_Impl->Width, m_Impl->Height}; }
RTXPTViewSize                         RTXPTEmbeddedView::GetDisplaySize() const { return {m_Impl->DisplayWidth, m_Impl->DisplayHeight}; }
const RTXPTViewSuperResolutionInfo&   RTXPTEmbeddedView::GetSuperResolutionInfo() const { return m_Impl->Upscaler.GetInfo(); }
const RTXPTViewRayReconstructionInfo& RTXPTEmbeddedView::GetRayReconstructionInfo() const { return m_Impl->RayReconstruction; }
const RTXPTRealtimeStats&             RTXPTEmbeddedView::GetRealtimeStats() const { return m_Impl->Realtime.GetStats(); }
IBuffer*                              RTXPTEmbeddedView::GetRealtimeLayerBuffer() const
{
    return m_Impl->OutputValid ? m_Impl->Realtime.GetTargets().GetDenoisingLayersBuffer() : nullptr;
}
const RTXPTDisplayBloomStats& RTXPTEmbeddedView::GetBloomStats() const
{
    return m_Impl->IsRealtime() ? m_Impl->Realtime.GetBloom().GetStats() : m_Impl->PostProcess.GetBloom().GetStats();
}

void CreateRTXPTEmbeddedStateCache(IRenderDevice* Device, IRenderStateCache** Cache)
{
    RenderStateCacheCreateInfo CacheInfo;
    CacheInfo.pDevice          = Device;
    CacheInfo.pArchiverFactory = LoadAndGetArchiverFactory();
    CacheInfo.FileHashMode     = RENDER_STATE_CACHE_FILE_HASH_MODE_BY_NAME;
    CreateRenderStateCache(CacheInfo, Cache);
}
} // namespace Diligent
