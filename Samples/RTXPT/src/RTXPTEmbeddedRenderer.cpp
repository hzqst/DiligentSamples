#include "RTXPTEmbeddedRenderer.hpp"
#include "RTXPTScene.hpp"
#include "RTXPTMaterials.hpp"
#include "RTXPTLights.hpp"
#include "RTXPTLightsBaker.hpp"
#include "RTXPTEnvMapBaker.hpp"
#include "RTXPTAccelerationStructures.hpp"
#include "RTXPTEmissiveTrianglePass.hpp"
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
    RefCntAutoPtr<IRenderStateCache>   Cache;
    RefCntAutoPtr<IPipelineStateCache> PSOCache;
    //The scene buffers and material textures the views bind, kept so views can be created after the source scene and
    //materials are gone.
    RefCntAutoPtr<IBuffer>                    VertexBuffer, IndexBuffer, MaterialBuffer;
    std::vector<RefCntAutoPtr<IDeviceObject>> TextureReferences;
    std::vector<IDeviceObject*>               Textures;
    VALUE_TYPE                                IndexType = VT_UNDEFINED;
    RTXPTLights                               Lights;
    RTXPTEnvMapBaker                          Environment;
    RTXPTAccelerationStructures               AS;
    RTXPTEmissiveTrianglePass                 Emissive;
    RTXPTEnvMapSettings                       EnvironmentSettings;
    bool                                      Ready{};

    IDeviceObject* const* GetTextures() const { return Textures.empty() ? nullptr : Textures.data(); }
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
    RTXPTShaderSourceScope ShaderScope{CreateInfo.Shaders};
    auto*                  Factory = Device->GetEngineFactory();
    S.VertexBuffer                 = Scene.GetVertexBuffer0(Device, Context);
    S.IndexBuffer                  = Scene.GetIndexBuffer(Device, Context);
    S.MaterialBuffer               = Materials.GetMaterialBuffer();
    S.IndexType                    = Scene.GetIndexType();
    S.Textures.assign(Materials.GetTextureBindings(), Materials.GetTextureBindings() + Materials.GetTextureCount());
    S.TextureReferences.assign(S.Textures.begin(), S.Textures.end());

    const auto& Graph = Scene.GetSceneGraphData();
    Error             = "RTXPT scene lights or acceleration structures could not be built";
    if (!S.Lights.Upload(Device, Graph) || !S.Lights.UploadEmissiveTriangles(Device, Graph) ||
        !S.AS.BuildStaticScene(Device, Context, Graph, S.IndexType, true) ||
        !S.AS.UploadSubInstanceTransforms(Context))
        return false;

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

    //The pass writes the emissive triangles of the lights once; views only read them.
    Error = "RTXPT emissive geometry preparation failed";
    RTXPTEmissiveTrianglePassCreateInfo EmissiveCI;
    EmissiveCI.pEngineFactory              = Factory;
    EmissiveCI.pStateCache                 = S.Cache;
    EmissiveCI.pPSOCache                   = S.PSOCache;
    EmissiveCI.pMaterialBuffer             = S.MaterialBuffer;
    EmissiveCI.pSubInstanceBuffer          = S.AS.GetSubInstanceBuffer();
    EmissiveCI.pSubInstanceTransformBuffer = S.AS.GetSubInstanceTransformBuffer();
    //Static scenes have no skinning, so the static vertices stand in for the skinned ones.
    EmissiveCI.pVertexBuffer           = S.VertexBuffer;
    EmissiveCI.pSkinnedVertexBuffer    = S.VertexBuffer;
    EmissiveCI.pIndexBuffer            = S.IndexBuffer;
    EmissiveCI.IndexValueType          = S.IndexType;
    EmissiveCI.pEmissiveTriangleBuffer = S.Lights.GetEmissiveTriangleBuffer();
    EmissiveCI.pMaterialTextures       = S.GetTextures();
    EmissiveCI.MaterialTextureCount    = static_cast<Uint32>(S.Textures.size());
    EmissiveCI.EnableMaterialTextures  = !S.Textures.empty();
    EmissiveCI.ComputeSupported        = true;
    if (!S.Emissive.Initialize(Device, EmissiveCI))
        return false;
    if (S.Lights.GetEmissiveTriangleCount() && !S.Emissive.Dispatch(Context, S.AS.GetStats().SubInstanceCount))
        return false;
    Context->Flush();
    Context->WaitForIdle();
    S.Ready = true;
    Error.clear();
    return true;
}

bool RTXPTEmbeddedScene::IsReady() const { return m_Impl->Ready; }

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
    PathTracerCameraData               PreviousCamera{};
    PathTracerViewData                 PreviousView{};
    Uint32                             Width{}, Height{}, FrameIndex{}, Samples{};
    bool                               Ready{};

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
    RTXPTLightsBakerSettings Lighting;
    Lighting.ImportanceSamplingType = 1;
    Lighting.ViewportSize = Lighting.PrevViewportSize = float2{float(Width), float(Height)};
    Lighting.EnvMapParams                             = S.Environment.GetLightsBakerParams();
    Lighting.pEnvironmentRadianceMap                  = S.Environment.GetRadianceMapSRV();
    Lighting.EnvMapImportanceMapResolution            = S.Environment.GetStats().ImportanceResolution;
    Lighting.EnvMapImportanceMapMipCount              = S.Environment.GetStats().ImportanceMipLevels;
    Lighting.ResetFeedback                            = true;
    Error                                             = "RTXPT light sampling preparation failed";
    RTXPTLightsBakerCreateInfo LightsBakerCI;
    LightsBakerCI.pEngineFactory   = Factory;
    LightsBakerCI.pStateCache      = R.Cache;
    LightsBakerCI.pPSOCache        = R.PSOCache;
    LightsBakerCI.Width            = Width;
    LightsBakerCI.Height           = Height;
    LightsBakerCI.ComputeSupported = true;
    if (!R.LightsBaker.CreateResources(Device, LightsBakerCI) ||
        !R.LightsBaker.UpdateBegin(Device, S.Lights, Lighting))
        return false;

    Error = "RTXPT ray tracing shaders or bindings failed";
    RTXPTRayTracingPassCreateInfo RaysCI;
    RaysCI.pEngineFactory                = Factory;
    RaysCI.pStateCache                   = R.Cache;
    RaysCI.pPSOCache                     = R.PSOCache;
    RaysCI.pFrameConstants               = R.Constants;
    RaysCI.pMaterialBuffer               = S.MaterialBuffer;
    RaysCI.pSubInstanceBuffer            = S.AS.GetSubInstanceBuffer();
    RaysCI.pLightBuffer                  = R.LightsBaker.GetLightBuffer();
    RaysCI.pLightExBuffer                = R.LightsBaker.GetLightExBuffer();
    RaysCI.pLightingControlBuffer        = R.LightsBaker.GetControlBuffer();
    RaysCI.pLightProxyCounters           = R.LightsBaker.GetLightProxyCounters();
    RaysCI.pLightSamplingProxies         = R.LightsBaker.GetLightSamplingProxies();
    RaysCI.pLocalSamplingBuffer          = R.LightsBaker.GetLocalSamplingBuffer();
    RaysCI.pEnvironmentLightLookup       = R.LightsBaker.GetEnvironmentLightLookup();
    RaysCI.pFeedbackTotalWeightUAV       = R.LightsBaker.GetFeedbackTotalWeightUAV();
    RaysCI.pFeedbackCandidatesUAV        = R.LightsBaker.GetFeedbackCandidatesUAV();
    RaysCI.pEnvironmentMapSRV            = S.Environment.GetEnvironmentMapSRV();
    RaysCI.pEnvironmentImportanceMapSRV  = S.Environment.GetImportanceMapSRV();
    RaysCI.pEnvironmentRadianceMapSRV    = S.Environment.GetRadianceMapSRV();
    RaysCI.pEnvironmentSampler           = S.Environment.GetEnvironmentSampler();
    RaysCI.pEnvironmentImportanceSampler = S.Environment.GetImportanceSampler();
    //Static scenes have no skinning, so the static vertices stand in for the skinned ones.
    RaysCI.pVertexBuffer                = S.VertexBuffer;
    RaysCI.pSkinnedVertexBuffer         = S.VertexBuffer;
    RaysCI.pIndexBuffer                 = S.IndexBuffer;
    RaysCI.IndexValueType               = S.IndexType;
    RaysCI.pTLAS                        = S.AS.GetTLAS();
    RaysCI.pMaterialTextures            = S.GetTextures();
    RaysCI.MaterialTextureCount         = static_cast<Uint32>(S.Textures.size());
    RaysCI.EnableMaterialTextures       = !S.Textures.empty();
    RaysCI.EnableAnyHit                 = true;
    RaysCI.RayTracingSupported          = true;
    RaysCI.StandaloneRTShadersSupported = true;
    RaysCI.NEECandidateSamples          = kNEECandidateSamples;
    RaysCI.NEEFullSamples               = kNEEFullSamples;
    if (!R.Rays.Initialize(Device, Context, RaysCI))
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
    const auto& S = *R.Scene;
    //Reference accumulation restarts on any camera/projection change. The host additionally resets cuts,
    //pipeline changes and discontinuities; successful executed frames alone advance history.
    ResetHistory = ResetHistory || !R.Samples || std::memcmp(&Camera, &R.PreviousCamera, sizeof(Camera)) != 0 ||
        std::memcmp(&View, &R.PreviousView, sizeof(View)) != 0;
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
    PT.analyticLightCount               = S.Lights.GetStats().LightCount;
    PT.environmentNEEEnabled            = (S.Lights.GetEmissiveTriangleCount() << 1u) | (S.EnvironmentSettings.Enabled ? 1u : 0u);
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
    R.PreviousCamera = Camera;
    R.PreviousView   = View;
    R.Samples        = Samples;
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
