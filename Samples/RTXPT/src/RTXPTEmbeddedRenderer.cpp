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
#include "RTXPTToneMappingPass.hpp"
#include "RTXPTRealtimeRawPipeline.hpp"
#include "RTXPTShaderSource.hpp"
#include "RenderStateCache.h"
#include "PipelineStateCache.h"
#include "ArchiverFactoryLoader.h"
#include "MapHelper.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace Diligent
{
namespace
{
//Next event estimation of the reference tracer: light candidates sampled per vertex, and how many of them are traced.
constexpr Uint32 kNEECandidateSamples = 5;
constexpr Uint32 kNEEFullSamples      = 1;
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
    bool                   Ready{};
    bool                   EnableProfiling{};
    RTXPTGpuProfiler       Profiler;

    RTXPTErrorCode Prepare(IDeviceContext* Context, const RTXPTSceneSnapshotPtr& Snapshot)
    {
        RTXPTShaderSourceScope  ShaderScope{Shaders};
        auto                    Timing = Profiler.BeginFrame(Device, Context, EnableProfiling, {}, Frames.GetStats().PrepareCount + 1);
        RTXPTGpuProfiler::Scope PrepareTiming{&Profiler, Context, RTXPTGpuProfiler::Stage::ScenePrepare};
        return Frames.Prepare(Context, Snapshot, &Profiler);
    }
};

RTXPTEmbeddedScene::RTXPTEmbeddedScene() :
    m_Impl{std::make_unique<Impl>()} {}
RTXPTEmbeddedScene::~RTXPTEmbeddedScene() = default;

RTXPTErrorCode RTXPTEmbeddedScene::Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedSceneCreateInfo& CreateInfo)
{
    auto& S           = *m_Impl;
    S.EnableProfiling = CreateInfo.EnableProfiling;
    if (!Device || !Context || !CreateInfo.Scene || !CreateInfo.Materials || !CreateInfo.Shaders || !CreateInfo.Cache)
        return RTXPTErrorCode::InvalidArgument;
    auto&       Scene     = *CreateInfo.Scene;
    auto&       Materials = *CreateInfo.Materials;
    const auto& Features  = Device->GetDeviceInfo().Features;
    if (Features.RayTracing != DEVICE_FEATURE_STATE_ENABLED || Features.BindlessResources != DEVICE_FEATURE_STATE_ENABLED ||
        Features.ComputeShaders != DEVICE_FEATURE_STATE_ENABLED ||
        !(Device->GetAdapterInfo().RayTracing.CapFlags & RAY_TRACING_CAP_FLAG_STANDALONE_SHADERS))
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
    //Only the scene can opt into an environment. Absence means black, never the Sample's default sky.
    S.EnvironmentSettings         = RTXPTEnvMapBaker::MakeSceneDefaultSettings(Graph);
    S.EnvironmentSettings.Enabled = std::any_of(Graph.Lights.begin(), Graph.Lights.end(),
                                                [](const auto& Light) { return Light.Type == "EnvironmentLight"; });
    //The Sample's disabled-environment shader evaluates a procedural fallback. Zero its multiplier as well
    //so an unlit embedded scene is black, including primary misses and indirect rays.
    if (!S.EnvironmentSettings.Enabled)
        S.EnvironmentSettings.RadianceScale = float3{0, 0, 0};
    S.Environment.SetFileSource(Scene.GetFileSource());
    RTXPTEnvMapBakerUpdateAttribs EnvironmentUpdate;
    EnvironmentUpdate.pEngineFactory   = Factory;
    EnvironmentUpdate.AssetsRoot       = Scene.GetAssetsRoot();
    EnvironmentUpdate.Settings         = S.EnvironmentSettings;
    EnvironmentUpdate.ForceRebuild     = true;
    EnvironmentUpdate.ComputeSupported = true;
    if (!S.Environment.CreateResources(Device, Context, Factory, S.Cache, S.PSOCache, true) ||
        !S.Environment.Update(Device, Context, EnvironmentUpdate))
        return RTXPTErrorCode::EnvironmentFailed;

    const RTXPTErrorCode PrepareCode = S.Frames.Prepare(Context, Snapshot);
    if (PrepareCode != RTXPTErrorCode::Ok)
        return PrepareCode;
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
    if (!Context)
        return RTXPTErrorCode::InvalidArgument;
    return m_Impl->Prepare(Context, m_Impl->Runtime.GetSnapshot());
}

RTXPTErrorCode RTXPTEmbeddedScene::Prepare(IDeviceContext* Context, const RTXPTSceneSnapshotPtr& Snapshot)
{
    if (!m_Impl->Ready)
        return RTXPTErrorCode::NotInitialized;
    if (!Context || !Snapshot)
        return RTXPTErrorCode::InvalidArgument;
    return m_Impl->Prepare(Context, Snapshot);
}

RTXPTSceneRuntime& RTXPTEmbeddedScene::GetRuntime() { return m_Impl->Runtime; }

const RTXPTImportedScene& RTXPTEmbeddedScene::GetImportedScene() const { return m_Impl->Imported; }

const RTXPTSceneFrameStats& RTXPTEmbeddedScene::GetFrameStats() const { return m_Impl->Frames.GetStats(); }
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
    RTXPTToneMappingPass               ToneMapping;
    RTXPTToneMappingParameters         ToneParameters;
    RTXPTRealtimeRawPipeline           Raw;
    RTXPTViewMode                      Mode = RTXPTViewMode::Reference;
    RTXPTLightsBakerSettings           Lighting;
    PathTracerCameraData               PreviousCamera{};
    PathTracerViewData                 PreviousView{};
    Uint32                             Width{}, Height{}, FrameIndex{}, Samples{};
    //The scene frame revisions this view's light sampling and history last followed.
    Uint64           LightingRevision{}, EmissiveRevision{}, ContentRevision{};
    bool             LightsBakerReady{};
    bool             Ready{};
    bool             OutputValid{};
    bool             EnableProfiling{};
    RTXPTGpuProfiler Profiler;

    void AcceptFrame(const PathTracerCameraData& Camera, const PathTracerViewData& View, Uint32 SampleCount, Uint64 Revision)
    {
        PreviousCamera  = Camera;
        PreviousView    = View;
        Samples         = SampleCount;
        ContentRevision = Revision;
        ++FrameIndex;
        OutputValid = true;
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
        RaysCI.EnableMaterialTextures       = true;
        RaysCI.EnableAnyHit                 = true;
        RaysCI.RayTracingSupported          = true;
        RaysCI.StandaloneRTShadersSupported = true;
        RaysCI.NEECandidateSamples          = kNEECandidateSamples;
        RaysCI.NEEFullSamples               = kNEEFullSamples;
        return Rays.Initialize(Device, Context, RaysCI);
    }

    //Follows the lighting of the scene frame and binds its resources.
    bool BindFrame(const RTXPTSceneFrame& Frame)
    {
        const auto& S = *Scene;
        if (!LightsBakerReady || Frame.LightingRevision != LightingRevision)
        {
            Lighting.ResetFeedback = !LightsBakerReady;
            if (!LightsBaker.UpdateBegin(Device, *Frame.pLights, Lighting))
                return false;
            LightsBakerReady = true;
            LightingRevision = Frame.LightingRevision;
            EmissiveRevision = Frame.EmissiveRevision;
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

    bool CreateTarget(const char* Name, TEXTURE_FORMAT Format, BIND_FLAGS Flags, RefCntAutoPtr<ITexture>& Target)
    {
        TextureDesc Desc;
        Desc.Name      = Name;
        Desc.Type      = RESOURCE_DIM_TEX_2D;
        Desc.Width     = Width;
        Desc.Height    = Height;
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
    m_Impl             = std::make_unique<Impl>();
    auto& R            = *m_Impl;
    R.EnableProfiling  = CreateInfo.EnableProfiling;
    const Uint32 Width = CreateInfo.RenderSize.Width, Height = CreateInfo.RenderSize.Height;
    if (!Device || !Context || !CreateInfo.Shaders || !CreateInfo.Cache || !CreateInfo.Scene || !CreateInfo.Scene->IsReady() ||
        !Width || !Height || !CreateInfo.DisplaySize.Width || !CreateInfo.DisplaySize.Height)
        return RTXPTErrorCode::InvalidArgument;
    if (Width != CreateInfo.DisplaySize.Width || Height != CreateInfo.DisplaySize.Height ||
        CreateInfo.DisplayRTVFormat != TEX_FORMAT_RGBA8_UNORM_SRGB ||
        (CreateInfo.Mode != RTXPTViewMode::Reference && CreateInfo.Mode != RTXPTViewMode::RealtimeRaw))
        return RTXPTErrorCode::UnsupportedViewConfiguration;
    const auto& S = *CreateInfo.Scene->m_Impl;
    R.Cache       = CreateInfo.Cache;
    R.PSOCache    = CreateInfo.PSOCache;
    R.Scene       = &S;
    R.Device      = Device;
    R.Context     = Context;
    R.Width       = Width;
    R.Height      = Height;
    R.Mode        = CreateInfo.Mode;
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
    if (!R.CreateTarget("RTXPT display output", TEX_FORMAT_RGBA8_TYPELESS, BIND_RENDER_TARGET, R.Output))
        return RTXPTErrorCode::FrameTargetsFailed;
    //Encode once on the RTV. The UNORM SRV lets the host copy encoded display bytes without decoding them again.
    TextureViewDesc DisplayView;
    DisplayView.ViewType = TEXTURE_VIEW_RENDER_TARGET;
    DisplayView.Format   = CreateInfo.DisplayRTVFormat;
    R.Output->CreateView(DisplayView, &R.DisplayRTV);
    if (!R.DisplayRTV)
        return RTXPTErrorCode::FrameTargetsFailed;

    //Light sampling keeps screen-sized feedback and reads this view's depth and motion every frame, so it is per view.
    R.Lighting.ImportanceSamplingType = 1;
    R.Lighting.ViewportSize = R.Lighting.PrevViewportSize = float2{float(Width), float(Height)};
    R.Lighting.EnvMapParams                               = S.Environment.GetLightsBakerParams();
    R.Lighting.pEnvironmentRadianceMap                    = S.Environment.GetRadianceMapSRV();
    R.Lighting.EnvMapImportanceMapResolution              = S.Environment.GetStats().ImportanceResolution;
    R.Lighting.EnvMapImportanceMapMipCount                = S.Environment.GetStats().ImportanceMipLevels;
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
    if (R.Mode == RTXPTViewMode::RealtimeRaw)
    {
        const auto Code = R.Raw.Initialize(Device, R.Cache, R.PSOCache, R.Constants, Width, Height, CreateInfo.DisplayRTVFormat);
        if (Code != RTXPTErrorCode::Ok)
            return Code;
        R.Radiance = R.Raw.GetTargets().GetOutputColorSRV()->GetTexture();
    }
    else if (!R.Accumulation.Initialize(Device, Factory, R.Cache, R.PSOCache, true) ||
             !R.ToneMapping.Initialize(Device, Factory, R.Cache, R.PSOCache, CreateInfo.DisplayRTVFormat, true) ||
             !R.ToneMapping.ResizeResources(Device, Width, Height, TEX_FORMAT_RGBA32_FLOAT))
        return RTXPTErrorCode::PostProcessSetupFailed;
    R.ToneParameters.ToneMapOperator = RTXPTToneMapperOperator::Reinhard;
    Context->Flush();
    Context->WaitForIdle();
    R.Ready = true;
    return RTXPTErrorCode::Ok;
}

RTXPTErrorCode RTXPTEmbeddedView::Render(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, bool ResetHistory, float Exposure)
{
    auto& R       = *m_Impl;
    ResetHistory  = ResetHistory || !R.OutputValid;
    R.OutputValid = false;
    R.Raw.InvalidateOutput();
    if (!R.Ready)
        return RTXPTErrorCode::NotInitialized;
    constexpr float MaxExposure = 4294967296.0f;
    if (!std::isfinite(Exposure) || Exposure < 1.0f / MaxExposure || Exposure > MaxExposure)
        return RTXPTErrorCode::InvalidArgument;
    auto ToneParameters                 = R.ToneParameters;
    ToneParameters.ExposureCompensation = std::log2(Exposure);
    if (Camera.ViewportWidth != R.Width || Camera.ViewportHeight != R.Height ||
        View.ViewportSize.x != float(R.Width) || View.ViewportSize.y != float(R.Height))
        return RTXPTErrorCode::InvalidArgument;
    const auto&            S     = *R.Scene;
    const RTXPTSceneFrame& Frame = S.Frames.GetFrame();
    if (!Frame.pTLAS)
        return RTXPTErrorCode::FrameNotPrepared;
    if (!R.BindFrame(Frame))
        return RTXPTErrorCode::SceneBindingsFailed;
    //Reference accumulation restarts on any camera/projection or scene content change. The host additionally resets
    //cuts, pipeline changes and discontinuities; successful executed frames alone advance history.
    if (R.Mode == RTXPTViewMode::Reference)
        ResetHistory = ResetHistory || !R.Samples || std::memcmp(&Camera, &R.PreviousCamera, sizeof(Camera)) != 0 ||
            std::memcmp(&View, &R.PreviousView, sizeof(View)) != 0 || Frame.Snapshot->ContentRevision != R.ContentRevision;
    const Uint32    Samples = ResetHistory ? 1u : R.Samples + 1u;
    SampleConstants Constants;
    Constants.view                      = View;
    Constants.previousView              = ResetHistory ? View : R.PreviousView;
    Constants.camera                    = Camera;
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
    PT.camera                           = Camera;
    PT.prevCamera                       = ResetHistory ? Camera : R.PreviousCamera;
    PT.analyticLightCount               = Frame.pLights->GetStats().LightCount;
    PT.environmentNEEEnabled            = (Frame.pLights->GetEmissiveTriangleCount() << 1u) | (S.EnvironmentSettings.Enabled ? 1u : 0u);
    Constants.envMap                    = S.Environment.GetConstants();
    if (R.Mode == RTXPTViewMode::RealtimeRaw)
    {
        PT.sampleBaseIndex = PT.sampleIndex = R.FrameIndex;
        auto       Timing                   = R.Profiler.BeginFrame(R.Device, R.Context, R.EnableProfiling, {}, R.FrameIndex + 1);
        const auto Code                     = R.Raw.Render(R.Context, R.Rays, R.LightsBaker, Constants, ToneParameters, R.DisplayRTV, &R.Profiler);
        if (Code != RTXPTErrorCode::Ok)
            return Code;
        R.AcceptFrame(Camera, View, 0, Frame.Snapshot->ContentRevision);
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
    RTXPTToneMappingRenderAttribs Tone;
    Tone.pSourceSRV = R.Processed->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    Tone.pLdrRTV    = R.DisplayRTV;
    Tone.Width      = R.Width;
    Tone.Height     = R.Height;
    Tone.pParams    = &ToneParameters;
    if (!R.ToneMapping.Render(R.Context, Tone))
        return RTXPTErrorCode::ToneMappingFailed;
    R.AcceptFrame(Camera, View, Samples, Frame.Snapshot->ContentRevision);
    return RTXPTErrorCode::Ok;
}

ITexture*           RTXPTEmbeddedView::GetOutput() const { return m_Impl->Output; }
RTXPTViewOutputInfo RTXPTEmbeddedView::GetOutputInfo(RTXPTViewOutput Output) const
{
    const auto&         R = *m_Impl;
    RTXPTViewOutputInfo Info;
    ITexture*           Texture   = nullptr;
    const auto&         Targets   = R.Raw.GetTargets();
    const auto          TextureOf = [](ITextureView* View) -> ITexture* {
        return View ? View->GetTexture() : nullptr;
    };
    switch (Output)
    {
        case RTXPTViewOutput::Display:
            Texture       = R.Output;
            Info.Encoding = RTXPTOutputEncoding::SRGB;
            break;
        case RTXPTViewOutput::NoisyRadiance: Texture = R.Radiance; break;
        case RTXPTViewOutput::AccumulatedRadiance: Texture = R.Accumulated; break;
        case RTXPTViewOutput::RayDistance: Texture = TextureOf(Targets.GetDepthSRV()); break;
        case RTXPTViewOutput::PrimaryMotion: Texture = TextureOf(Targets.GetScreenMotionVectorsSRV()); break;
        case RTXPTViewOutput::SpecularHitDistance: Texture = TextureOf(Targets.GetSpecularHitTSRV()); break;
        case RTXPTViewOutput::AverageLayerRadiance: Texture = TextureOf(Targets.GetDenoiserAvgLayerRadianceHalfResSRV()); break;
        case RTXPTViewOutput::LayerHeader: Texture = TextureOf(Targets.GetDenoisingLayersHeaderSRV()); break;
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
    Info.FrameIndex     = Info.Valid ? R.FrameIndex : 0;
    return Info;
}
RTXPTGpuProfiler&            RTXPTEmbeddedView::GetProfiler() { return m_Impl->Profiler; }
Uint32                       RTXPTEmbeddedView::GetTraceCount() const { return m_Impl->Rays.GetStats().TraceCount; }
Uint32                       RTXPTEmbeddedView::GetAccumulationCount() const { return m_Impl->Samples; }
const RTXPTRealtimeRawStats& RTXPTEmbeddedView::GetRealtimeRawStats() const { return m_Impl->Raw.GetStats(); }
IBuffer*                     RTXPTEmbeddedView::GetRealtimeLayerBuffer() const
{
    return m_Impl->OutputValid ? m_Impl->Raw.GetTargets().GetDenoisingLayersBuffer() : nullptr;
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
