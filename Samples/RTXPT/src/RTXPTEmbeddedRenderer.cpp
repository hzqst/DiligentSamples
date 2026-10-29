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

namespace Diligent
{
struct RTXPTEmbeddedRenderer::Impl
{
    RefCntAutoPtr<IRenderDevice>                   Device;
    RefCntAutoPtr<IDeviceContext>                  Context;
    RefCntAutoPtr<IRenderStateCache>               Cache;
    RefCntAutoPtr<IPipelineStateCache>             PSOCache;
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Shaders;
    RefCntAutoPtr<IBuffer>                         Constants;
    RefCntAutoPtr<ITexture>                        Radiance, Depth, Motion, Accumulated, Processed, Output;
    RefCntAutoPtr<ITextureView>                    DisplayRTV;
    RTXPTLights                                    Lights;
    RTXPTLightsBaker                               LightsBaker;
    RTXPTEnvMapBaker                               Environment;
    RTXPTAccelerationStructures                    AS;
    RTXPTEmissiveTrianglePass                      Emissive;
    RTXPTRayTracingPass                            Rays;
    RTXPTAccumulationPass                          Accumulation;
    RTXPTToneMappingPass                           ToneMapping;
    RTXPTToneMappingParameters                     ToneParameters;
    RTXPTEnvMapSettings                            EnvironmentSettings;
    PathTracerCameraData                           PreviousCamera{};
    PathTracerViewData                             PreviousView{};
    Uint32                                         Width{}, Height{}, FrameIndex{}, Samples{};
    bool                                           Ready{};

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

RTXPTEmbeddedRenderer::RTXPTEmbeddedRenderer() :
    m_Impl{std::make_unique<Impl>()} {}
RTXPTEmbeddedRenderer::~RTXPTEmbeddedRenderer() = default;

bool RTXPTEmbeddedRenderer::Initialize(IRenderDevice* Device, IDeviceContext* Context, RTXPTScene& Scene, RTXPTMaterials& Materials, IShaderSourceInputStreamFactory* Shaders, IRenderStateCache* Cache, IPipelineStateCache* PSOCache, Uint32 Width, Uint32 Height, std::string& Error)
{
    auto& R = *m_Impl;
    Error   = "RTXPT requires ray tracing, bindless resources, compute and standalone RT shaders";
    if (!Device || !Context || !Shaders || !Width || !Height)
        return false;
    const auto& Features = Device->GetDeviceInfo().Features;
    if (Features.RayTracing != DEVICE_FEATURE_STATE_ENABLED || Features.BindlessResources != DEVICE_FEATURE_STATE_ENABLED ||
        Features.ComputeShaders != DEVICE_FEATURE_STATE_ENABLED ||
        !(Device->GetAdapterInfo().RayTracing.CapFlags & RAY_TRACING_CAP_FLAG_STANDALONE_SHADERS))
        return false;
    if (!Cache)
    {
        Error = "RTXPT embedded reference renderer requires a render state cache";
        return false;
    }
    if (Scene.HasSkinnedGeometry())
    {
        Error = "RTXPT embedded reference renderer supports static geometry only";
        return false;
    }
    R.Device   = Device;
    R.Context  = Context;
    R.Shaders  = Shaders;
    R.Cache    = Cache;
    R.PSOCache = PSOCache;
    R.Width    = Width;
    R.Height   = Height;
    RTXPTShaderSourceScope     ShaderScope{Shaders};
    auto*                      Factory = Device->GetEngineFactory();
    BufferDesc ConstantsDesc;
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

    const auto& Graph = Scene.GetSceneGraphData();
    Error             = "RTXPT scene lights or acceleration structures could not be built";
    if (!R.Lights.Upload(Device, Graph) || !R.Lights.UploadEmissiveTriangles(Device, Graph) ||
        !R.AS.BuildStaticScene(Device, Context, Graph, Scene.GetIndexType(), true) ||
        !R.AS.UploadSubInstanceTransforms(Context))
        return false;

    //Only the scene can opt into an environment. Absence means black, never the Sample's default sky.
    R.EnvironmentSettings         = RTXPTEnvMapBaker::MakeSceneDefaultSettings(Graph);
    R.EnvironmentSettings.Enabled = std::any_of(Graph.Lights.begin(), Graph.Lights.end(),
                                                [](const auto& Light) { return Light.Type == "EnvironmentLight"; });
    //The Sample's disabled-environment shader evaluates a procedural fallback. Zero its multiplier as well
    //so an unlit embedded scene is black, including primary misses and indirect rays.
    if (!R.EnvironmentSettings.Enabled)
        R.EnvironmentSettings.RadianceScale = float3{0, 0, 0};
    R.Environment.SetFileSource(Scene.GetFileSource());
    Error = "RTXPT scene environment failed to load or bake";
    if (!R.Environment.CreateResources(Device, Context, Factory, R.Cache, R.PSOCache, true) ||
        !R.Environment.Update(Device, Context, Factory, Scene.GetAssetsRoot(), R.EnvironmentSettings, true, true))
        return false;

    Error               = "RTXPT emissive geometry preparation failed";
    const bool Textured = Materials.GetTextureCount() != 0;
    if (!R.Emissive.Initialize(Device, Factory, R.Cache, R.PSOCache, Materials.GetMaterialBuffer(), R.AS.GetSubInstanceBuffer(),
                               R.AS.GetSubInstanceTransformBuffer(), Scene.GetVertexBuffer0(Device, Context), Scene.GetVertexBuffer0(Device, Context),
                               Scene.GetIndexBuffer(Device, Context), Scene.GetIndexType(), R.Lights.GetEmissiveTriangleBuffer(),
                               Materials.GetTextureBindings(), Materials.GetTextureCount(), Textured, true))
        return false;
    if (R.Lights.GetEmissiveTriangleCount() && !R.Emissive.Dispatch(Context, R.AS.GetStats().SubInstanceCount))
        return false;

    RTXPTLightsBakerSettings Lighting;
    Lighting.ImportanceSamplingType = 1;
    Lighting.ViewportSize = Lighting.PrevViewportSize = float2{float(Width), float(Height)};
    Lighting.EnvMapParams                             = R.Environment.GetLightsBakerParams();
    Lighting.pEnvironmentRadianceMap                  = R.Environment.GetRadianceMapSRV();
    Lighting.EnvMapImportanceMapResolution            = R.Environment.GetStats().ImportanceResolution;
    Lighting.EnvMapImportanceMapMipCount              = R.Environment.GetStats().ImportanceMipLevels;
    Lighting.ResetFeedback                            = true;
    Error                                             = "RTXPT light sampling preparation failed";
    if (!R.LightsBaker.CreateResources(Device, Factory, R.Cache, R.PSOCache, Width, Height, true) ||
        !R.LightsBaker.UpdateBegin(Device, R.Lights, Lighting))
        return false;

    Error = "RTXPT ray tracing shaders or bindings failed";
    if (!R.Rays.Initialize(Device, Context, Factory, R.Cache, R.PSOCache, R.Constants, Materials.GetMaterialBuffer(),
                           R.AS.GetSubInstanceBuffer(), R.LightsBaker.GetLightBuffer(), R.LightsBaker.GetLightExBuffer(),
                           R.LightsBaker.GetControlBuffer(), R.LightsBaker.GetLightProxyCounters(), R.LightsBaker.GetLightSamplingProxies(),
                           R.LightsBaker.GetLocalSamplingBuffer(), R.LightsBaker.GetEnvironmentLightLookup(),
                           R.LightsBaker.GetFeedbackTotalWeightUAV(), R.LightsBaker.GetFeedbackCandidatesUAV(),
                           R.Environment.GetEnvironmentMapSRV(), R.Environment.GetImportanceMapSRV(), R.Environment.GetRadianceMapSRV(),
                           R.Environment.GetEnvironmentSampler(), R.Environment.GetImportanceSampler(),
                           Scene.GetVertexBuffer0(Device, Context), Scene.GetVertexBuffer0(Device, Context), Scene.GetIndexBuffer(Device, Context), Scene.GetIndexType(),
                           R.AS.GetTLAS(), Materials.GetTextureBindings(), Materials.GetTextureCount(), Textured, true,
                           false, true, true, false, 0, 5, 1, false, false, false, false))
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

bool RTXPTEmbeddedRenderer::Render(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, bool ResetHistory, std::string& Error)
{
    auto& R = *m_Impl;
    if (!R.Ready)
    {
        Error = "RTXPT scene is not initialized";
        return false;
    }
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
    PT.analyticLightCount               = R.Lights.GetStats().LightCount;
    PT.environmentNEEEnabled            = (R.Lights.GetEmissiveTriangleCount() << 1u) | (R.EnvironmentSettings.Enabled ? 1u : 0u);
    Constants.envMap                    = R.Environment.GetConstants();
    {
        MapHelper<SampleConstants> Mapped{R.Context, R.Constants, MAP_WRITE, MAP_FLAG_DISCARD};
        if (!Mapped)
            return false;
        *Mapped = Constants;
    }
    Error = "RTXPT light sampling or path tracing dispatch failed";
    R.LightsBaker.UpdateFrame(View.MatWorldToClip, Camera.PosW);
    if (!R.LightsBaker.PrepareLightBuffers(R.Context) ||
        !R.LightsBaker.UpdateEnd(R.Context, R.Depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), R.Motion->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)) ||
        !R.Rays.Trace(R.Context, R.Radiance->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS),
                      R.Depth->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS), R.Motion->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS), R.Width, R.Height))
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

ITexture* RTXPTEmbeddedRenderer::GetOutput() const { return m_Impl->Output; }
Uint32    RTXPTEmbeddedRenderer::GetTraceCount() const { return m_Impl->Rays.GetStats().TraceCount; }
Uint32    RTXPTEmbeddedRenderer::GetAccumulationCount() const { return m_Impl->Samples; }

void CreateRTXPTEmbeddedStateCache(IRenderDevice* Device, IRenderStateCache** Cache)
{
    RenderStateCacheCreateInfo CacheInfo;
    CacheInfo.pDevice          = Device;
    CacheInfo.pArchiverFactory = LoadAndGetArchiverFactory();
    CacheInfo.FileHashMode     = RENDER_STATE_CACHE_FILE_HASH_MODE_BY_NAME;
    CreateRenderStateCache(CacheInfo, Cache);
}
} // namespace Diligent
