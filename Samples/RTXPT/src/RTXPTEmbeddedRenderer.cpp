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

    bool Prepare(IDeviceContext* Context, std::string& Error)
    {
        RTXPTShaderSourceScope ShaderScope{Shaders};
        return Frames.Prepare(Context, Runtime.GetSnapshot(), Error);
    }
};

RTXPTEmbeddedScene::RTXPTEmbeddedScene() :
    m_Impl{std::make_unique<Impl>()} {}
RTXPTEmbeddedScene::~RTXPTEmbeddedScene() = default;

bool RTXPTEmbeddedScene::Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedSceneCreateInfo& CreateInfo, std::string& Error)
{
    auto& S = *m_Impl;
    Error   = "RTXPT embedded scene requires a device, a context, a scene, materials and shaders";
    if (!Device || !Context || !CreateInfo.Scene || !CreateInfo.Materials || !CreateInfo.Shaders)
        return false;
    auto& Scene          = *CreateInfo.Scene;
    auto& Materials      = *CreateInfo.Materials;
    Error                = "RTXPT requires ray tracing, bindless resources, compute and standalone RT shaders";
    const auto& Features = Device->GetDeviceInfo().Features;
    if (Features.RayTracing != DEVICE_FEATURE_STATE_ENABLED || Features.BindlessResources != DEVICE_FEATURE_STATE_ENABLED ||
        Features.ComputeShaders != DEVICE_FEATURE_STATE_ENABLED ||
        !(Device->GetAdapterInfo().RayTracing.CapFlags & RAY_TRACING_CAP_FLAG_STANDALONE_SHADERS))
        return false;
    if (!CreateInfo.Cache)
    {
        Error = "RTXPT embedded renderer requires a render state cache";
        return false;
    }
    if (Scene.HasSkinnedGeometry())
    {
        Error = "RTXPT embedded renderer supports static geometry only";
        return false;
    }
    S.Cache    = CreateInfo.Cache;
    S.PSOCache = CreateInfo.PSOCache;
    S.Shaders  = CreateInfo.Shaders;
    S.Device   = Device;
    RTXPTShaderSourceScope ShaderScope{CreateInfo.Shaders};
    auto*                  Factory = Device->GetEngineFactory();

    if (S.Runtime.BeginUpdate() != RTXPTSceneResult::Ok)
        return false;
    RTXPTSceneSnapshotPtr Snapshot;
    if (!ImportRTXPTScene(Device, Context, Scene, Materials, S.Runtime, S.Imported, Error))
    {
        S.Runtime.CancelUpdate();
        return false;
    }
    if (S.Runtime.CommitUpdate(Snapshot) != RTXPTSceneResult::Ok)
    {
        Error = "RTXPT scene import could not be committed";
        return false;
    }

    RTXPTSceneFrameBuilderCreateInfo FramesCI;
    FramesCI.pEngineFactory = Factory;
    FramesCI.pStateCache    = S.Cache;
    FramesCI.pPSOCache      = S.PSOCache;
    if (!S.Frames.Initialize(Device, FramesCI, Error))
        return false;

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
    Error = "RTXPT scene environment failed to load or bake";
    RTXPTEnvMapBakerUpdateAttribs EnvironmentUpdate;
    EnvironmentUpdate.pEngineFactory   = Factory;
    EnvironmentUpdate.AssetsRoot       = Scene.GetAssetsRoot();
    EnvironmentUpdate.Settings         = S.EnvironmentSettings;
    EnvironmentUpdate.ForceRebuild     = true;
    EnvironmentUpdate.ComputeSupported = true;
    if (!S.Environment.CreateResources(Device, Context, Factory, S.Cache, S.PSOCache, true) ||
        !S.Environment.Update(Device, Context, EnvironmentUpdate))
        return false;

    if (!S.Frames.Prepare(Context, Snapshot, Error))
        return false;
    Context->Flush();
    Context->WaitForIdle();
    S.Ready = true;
    Error.clear();
    return true;
}

bool RTXPTEmbeddedScene::IsReady() const { return m_Impl->Ready; }

bool RTXPTEmbeddedScene::Prepare(IDeviceContext* Context, std::string& Error)
{
    if (!m_Impl->Ready || !Context)
    {
        Error = "RTXPT scene is not initialized";
        return false;
    }
    return m_Impl->Prepare(Context, Error);
}

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
    RTXPTLightsBakerSettings           Lighting;
    PathTracerCameraData               PreviousCamera{};
    PathTracerViewData                 PreviousView{};
    Uint32                             Width{}, Height{}, FrameIndex{}, Samples{};
    //The scene frame revisions this view's light sampling and history last followed.
    Uint64 LightingRevision{}, EmissiveRevision{}, ContentRevision{};
    bool   LightsBakerReady{};
    bool   Ready{};

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

bool RTXPTEmbeddedView::Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedViewCreateInfo& CreateInfo, std::string& Error)
{
    auto&        R     = *m_Impl;
    const Uint32 Width = CreateInfo.Width, Height = CreateInfo.Height;
    Error = "RTXPT view requires a ready scene, a render state cache and a nonempty size";
    if (!Device || !Context || !CreateInfo.Shaders || !CreateInfo.Cache || !CreateInfo.Scene || !CreateInfo.Scene->IsReady() ||
        !Width || !Height)
        return false;
    const auto& S = *CreateInfo.Scene->m_Impl;
    R.Cache       = CreateInfo.Cache;
    R.PSOCache    = CreateInfo.PSOCache;
    R.Scene       = &S;
    R.Device      = Device;
    R.Context     = Context;
    R.Width       = Width;
    R.Height      = Height;
    RTXPTShaderSourceScope ShaderScope{CreateInfo.Shaders};
    auto*                  Factory = Device->GetEngineFactory();
    BufferDesc             ConstantsDesc;
    ConstantsDesc.Name           = "RTXPT embedded frame constants";
    ConstantsDesc.Size           = sizeof(SampleConstants);
    ConstantsDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage          = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    Device->CreateBuffer(ConstantsDesc, nullptr, &R.Constants);
    Error = "RTXPT frame target creation failed";
    if (!R.Constants ||
        !R.CreateTarget("RTXPT radiance", TEX_FORMAT_RGBA16_FLOAT, BIND_UNORDERED_ACCESS, R.Radiance) ||
        !R.CreateTarget("RTXPT depth", TEX_FORMAT_R32_FLOAT, BIND_UNORDERED_ACCESS, R.Depth) ||
        !R.CreateTarget("RTXPT motion", TEX_FORMAT_RGBA16_FLOAT, BIND_UNORDERED_ACCESS, R.Motion) ||
        !R.CreateTarget("RTXPT accumulated", TEX_FORMAT_RGBA32_FLOAT, BIND_UNORDERED_ACCESS, R.Accumulated) ||
        !R.CreateTarget("RTXPT processed", TEX_FORMAT_RGBA16_FLOAT, BIND_UNORDERED_ACCESS, R.Processed) ||
        !R.CreateTarget("RTXPT display output", TEX_FORMAT_RGBA8_TYPELESS, BIND_RENDER_TARGET, R.Output))
        return false;
    //Encode once on the RTV. The UNORM SRV lets the host copy encoded display bytes without decoding them again.
    TextureViewDesc DisplayView;
    DisplayView.ViewType = TEXTURE_VIEW_RENDER_TARGET;
    DisplayView.Format   = TEX_FORMAT_RGBA8_UNORM_SRGB;
    R.Output->CreateView(DisplayView, &R.DisplayRTV);
    if (!R.DisplayRTV)
        return false;

    //Light sampling keeps screen-sized feedback and reads this view's depth and motion every frame, so it is per view.
    R.Lighting.ImportanceSamplingType = 1;
    R.Lighting.ViewportSize = R.Lighting.PrevViewportSize = float2{float(Width), float(Height)};
    R.Lighting.EnvMapParams                               = S.Environment.GetLightsBakerParams();
    R.Lighting.pEnvironmentRadianceMap                    = S.Environment.GetRadianceMapSRV();
    R.Lighting.EnvMapImportanceMapResolution              = S.Environment.GetStats().ImportanceResolution;
    R.Lighting.EnvMapImportanceMapMipCount                = S.Environment.GetStats().ImportanceMipLevels;
    Error                                                 = "RTXPT light sampling preparation failed";
    RTXPTLightsBakerCreateInfo LightsBakerCI;
    LightsBakerCI.pEngineFactory   = Factory;
    LightsBakerCI.pStateCache      = R.Cache;
    LightsBakerCI.pPSOCache        = R.PSOCache;
    LightsBakerCI.Width            = Width;
    LightsBakerCI.Height           = Height;
    LightsBakerCI.ComputeSupported = true;
    if (!R.LightsBaker.CreateResources(Device, LightsBakerCI))
        return false;

    Error                        = "RTXPT ray tracing shaders or bindings failed";
    const RTXPTSceneFrame& Frame = S.Frames.GetFrame();
    if (!Frame.pTLAS || !R.InitializeRays(Frame.MaterialTextureCount) || !R.BindFrame(Frame))
        return false;
    Error = "RTXPT accumulation or tone mapping initialization failed";
    if (!R.Accumulation.Initialize(Device, Factory, R.Cache, R.PSOCache, true) ||
        !R.ToneMapping.Initialize(Device, Factory, R.Cache, R.PSOCache, TEX_FORMAT_RGBA8_UNORM_SRGB, true) ||
        !R.ToneMapping.ResizeResources(Device, Width, Height, TEX_FORMAT_RGBA32_FLOAT))
        return false;
    R.ToneParameters.ToneMapOperator = RTXPTToneMapperOperator::Reinhard;
    Context->Flush();
    Context->WaitForIdle();
    R.Ready = true;
    Error.clear();
    return true;
}

bool RTXPTEmbeddedView::Render(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, bool ResetHistory, std::string& Error)
{
    auto& R = *m_Impl;
    if (!R.Ready)
    {
        Error = "RTXPT view is not initialized";
        return false;
    }
    const auto&            S     = *R.Scene;
    const RTXPTSceneFrame& Frame = S.Frames.GetFrame();
    if (!Frame.pTLAS)
    {
        Error = "RTXPT scene frame is not prepared";
        return false;
    }
    Error = "RTXPT light sampling or scene bindings failed";
    if (!R.BindFrame(Frame))
        return false;
    //Reference accumulation restarts on any camera/projection or scene content change. The host additionally resets
    //cuts, pipeline changes and discontinuities; successful executed frames alone advance history.
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
    {
        MapHelper<SampleConstants> Mapped{R.Context, R.Constants, MAP_WRITE, MAP_FLAG_DISCARD};
        if (!Mapped)
            return false;
        *Mapped = Constants;
    }
    RTXPTRayTracingDispatch Trace;
    Trace.pOutputColorUAV   = R.Radiance->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    Trace.pDepthUAV         = R.Depth->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    Trace.pMotionVectorsUAV = R.Motion->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    Trace.Width             = R.Width;
    Trace.Height            = R.Height;
    Error                   = "RTXPT light sampling or path tracing dispatch failed";
    R.LightsBaker.UpdateFrame(View.MatWorldToClip, Camera.PosW);
    if (!R.LightsBaker.PrepareLightBuffers(R.Context) ||
        !R.LightsBaker.UpdateEnd(R.Context, R.Depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), R.Motion->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)) ||
        !R.Rays.Trace(R.Context, Trace))
        return false;
    RTXPTAccumulationDispatch Accumulation;
    Accumulation.pInputColorSRV          = R.Radiance->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    Accumulation.pAccumulatedRadianceUAV = R.Accumulated->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    Accumulation.pProcessedOutputUAV     = R.Processed->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    Accumulation.InputWidth = Accumulation.OutputWidth = R.Width;
    Accumulation.InputHeight = Accumulation.OutputHeight = R.Height;
    Accumulation.BlendFactor                             = 1.0f / Samples;
    Error                                                = "RTXPT accumulation failed";
    if (!R.Accumulation.Render(R.Context, Accumulation))
        return false;
    RTXPTToneMappingRenderAttribs Tone;
    Tone.pSourceSRV = R.Processed->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    Tone.pLdrRTV    = R.DisplayRTV;
    Tone.Width      = R.Width;
    Tone.Height     = R.Height;
    Tone.pParams    = &R.ToneParameters;
    Error           = "RTXPT tone mapping failed";
    if (!R.ToneMapping.Render(R.Context, Tone))
        return false;
    R.PreviousCamera  = Camera;
    R.PreviousView    = View;
    R.Samples         = Samples;
    R.ContentRevision = Frame.Snapshot->ContentRevision;
    ++R.FrameIndex;
    Error.clear();
    return true;
}

ITexture* RTXPTEmbeddedView::GetOutput() const { return m_Impl->Output; }
Uint32    RTXPTEmbeddedView::GetTraceCount() const { return m_Impl->Rays.GetStats().TraceCount; }
Uint32    RTXPTEmbeddedView::GetAccumulationCount() const { return m_Impl->Samples; }

void CreateRTXPTEmbeddedStateCache(IRenderDevice* Device, IRenderStateCache** Cache)
{
    RenderStateCacheCreateInfo CacheInfo;
    CacheInfo.pDevice          = Device;
    CacheInfo.pArchiverFactory = LoadAndGetArchiverFactory();
    CacheInfo.FileHashMode     = RENDER_STATE_CACHE_FILE_HASH_MODE_BY_NAME;
    CreateRenderStateCache(CacheInfo, Cache);
}
} // namespace Diligent
