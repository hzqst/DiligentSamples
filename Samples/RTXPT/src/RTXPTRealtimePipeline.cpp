#include "RTXPTRealtimePipeline.hpp"
#include "MapHelper.hpp"
#include "RTXPTNrdConfig.hpp"
#include "../shaders/PathTracer/SubSampling.h"
#include <cmath>
#include <limits>

namespace Diligent
{
namespace
{
//RealtimeNative traces and prepares its layers with the Sample's realtime defaults (RTXPTRealtimeSettings), the
//configuration whose RELAX output the M3.0 baseline thresholds were measured on.
constexpr float  kNativeFireflyFilterThreshold         = 0.10f;
constexpr float  kFireflyFilterThresholdScale          = 1000.0f;
constexpr float  kNativeDenoiserRadianceClampK         = 8.0f;
constexpr float  kNativeSuppressPrimaryIndirectSpecK   = 0.6f;
constexpr float  kNativeAntiAliasingFallthrough        = 0.6f;
constexpr Uint32 kNativeAllowPrimarySurfaceReplacement = 1;
constexpr Uint32 kMaxLayerVertexDepth                  = 9;

//Every denoiser input and output the NRD layers read or write must be a UAV format of the device.
bool SupportsNrdInputs(IRenderDevice* Device, const RTXPTRenderTargetFormats& Formats)
{
    if (!Device->GetDeviceInfo().Features.ComputeShaders)
        return false;
    for (const auto Format : {Formats.DenoiserViewspaceZ, Formats.DenoiserMotionVectors, Formats.DenoiserNormalRoughness,
                              Formats.DenoiserDiffRadianceHitDist, Formats.DenoiserSpecRadianceHitDist,
                              Formats.DenoiserDisocclusionThresholdMix, Formats.DenoiserHistoryConfidence,
                              Formats.DenoiserOutDiffRadianceHitDist, Formats.DenoiserOutSpecRadianceHitDist})
    {
        if ((Device->GetTextureFormatInfoExt(Format).BindFlags & BIND_UNORDERED_ACCESS) == 0)
            return false;
    }
    return true;
}
} // namespace

RTXPTErrorCode RTXPTRealtimePipeline::Initialize(const RTXPTRealtimePipelineCreateInfo& CreateInfo)
{
    m_Ready = false;
    m_Stats = {};
    m_Targets.Reset();
    m_Nrd.Reset();
    m_PixelList.Release();
    m_Constants         = CreateInfo.Constants;
    m_DisplayFormat     = CreateInfo.DisplayFormat;
    m_Denoiser          = CreateInfo.Denoiser;
    auto* const  Device = CreateInfo.Device;
    auto* const  Cache  = CreateInfo.Cache;
    const Uint32 Width = CreateInfo.Width, Height = CreateInfo.Height;
    if (!Device || !Cache || !CreateInfo.Constants || !Width || !Height || CreateInfo.DisplayFormat == TEX_FORMAT_UNKNOWN ||
        ComputeRTXPTLayerPlaneStride(Width, Height) > std::numeric_limits<Uint32>::max())
        return RTXPTErrorCode::InvalidArgument;
    const bool bNative = m_Denoiser == RTXPTRealtimeDenoiser::NrdRelax;
    if (bNative && !RTXPTIsNrdAvailable())
        return RTXPTErrorCode::DenoiserUnavailable;
    RTXPTRenderTargetCreateInfo Targets;
    if (bNative && !SupportsNrdInputs(Device, Targets.Formats))
        return RTXPTErrorCode::DenoiserUnsupportedDevice;

    Targets.Dimensions              = {Width, Height, Width, Height, false};
    Targets.CreateRealtimeResources = true;
    if (!m_Targets.Resize(Device, Targets))
        return RTXPTErrorCode::FrameTargetsFailed;
    //The full-rate shader still declares the compaction list; with a 1x1 pattern it never reads entries.
    BufferDesc List;
    List.Name      = "RTXPT realtime full-rate pixel list";
    List.Size      = RTXPT_SUB_SAMPLING_LIST_HEADER_BYTES;
    List.BindFlags = BIND_UNORDERED_ACCESS;
    List.Mode      = BUFFER_MODE_RAW;
    Device->CreateBuffer(List, nullptr, &m_PixelList);
    if (!m_PixelList)
        return RTXPTErrorCode::FrameTargetsFailed;
    auto* Factory = Device->GetEngineFactory();
    if (!m_Guides.Initialize(Device, Factory, Cache, m_Constants, true) ||
        !m_Merge.Initialize(Device, Factory, Cache, m_Constants, true, bNative ? RTXPTPostProcessPassSet::RealtimeNative : RTXPTPostProcessPassSet::RealtimeRaw) ||
        !m_ToneMapping.Initialize(Device, Factory, Cache, CreateInfo.PSOCache, m_DisplayFormat, true) ||
        !m_ToneMapping.ResizeResources(Device, Width, Height, Targets.Formats.OutputColor))
        return RTXPTErrorCode::PostProcessSetupFailed;
    if (bNative)
    {
        //RELAX with the Sample's default settings, the configuration of the M3.0 baseline.
        m_NrdSettings           = {};
        m_NrdSettings.NRDMethod = RTXPTNrdMethod::RELAX;
        RTXPTNrdIntegrationCreateInfo NrdCI;
        NrdCI.pEngineFactory   = Factory;
        NrdCI.pStateCache      = Cache;
        NrdCI.Method           = m_NrdSettings.NRDMethod;
        NrdCI.Width            = Width;
        NrdCI.Height           = Height;
        NrdCI.ComputeSupported = true;
        if (!m_Nrd.Prepare(Device, NrdCI))
            return RTXPTErrorCode::DenoiserSetupFailed;
        m_Stats.NrdInstances      = kRTXPTDenoisingLayerCount;
        m_Stats.NrdPoolBytes      = m_Nrd.GetPoolTextureBytes();
        m_Stats.HistoryConfidence = CreateInfo.HistoryConfidence;
    }
    m_Ready = true;
    return RTXPTErrorCode::Ok;
}

/* Build initializes headers/surfaces, then light preparation consumes this frame's depth/motion before Fill.
 * Fill produces one current sample per layer; guides are spatial/current-frame only. Raw merges the noisy layers and
 * Native denoises them first (Denoise); both produce linear HDR, and ToneMapping writes the host's explicit RTV.
 * Only completion of the entire chain publishes valid output.
 */
RTXPTErrorCode RTXPTRealtimePipeline::Render(IDeviceContext* Context, RTXPTRayTracingPass& Rays, RTXPTLightsBaker& Lights, const SampleConstants& Input, const RTXPTToneMappingParameters& ToneParameters, const RTXPTRealtimeHistoryAttribs& History, ITextureView* DisplayRTV, RTXPTGpuProfiler* Profiler)
{
    m_Stats.OutputValid = false;
    if (!m_Ready)
        return RTXPTErrorCode::NotInitialized;
    const Uint32 Width = m_Targets.GetRenderWidth(), Height = m_Targets.GetRenderHeight();
    if (!Context || !DisplayRTV || DisplayRTV->GetDesc().Format != m_DisplayFormat ||
        DisplayRTV->GetTexture()->GetDesc().Width != Width || DisplayRTV->GetTexture()->GetDesc().Height != Height ||
        Input.ptConsts.imageWidth != Width || Input.ptConsts.imageHeight != Height ||
        Input.ptConsts.NEEType > 1 || ToneParameters.AutoExposure)
        return RTXPTErrorCode::InvalidArgument;

    SampleConstants Constants = Input;
    auto&           PT        = Constants.ptConsts;
    PT.invSubSampleCount      = 1.0f;
    PT.useReSTIRDI = PT.useReSTIRGI = PT.superResolutionActive = 0;
    PT.perPixelJitterAAScale                                   = 0;
    PT.DLSSRRBrightnessClampK                                  = 0;
    PT._activeDenoisingLayerCount                              = kRTXPTDenoisingLayerCount;
    PT.maxDenoisingLayerVertexDepth                            = std::min(PT.bounceCount, kMaxLayerVertexDepth);
    PT.genericTSLineStride                                     = static_cast<Uint32>(ComputeRTXPTLayerLineStride(Width));
    PT.genericTSPlaneStride                                    = static_cast<Uint32>(ComputeRTXPTLayerPlaneStride(Width, Height));
    PT.preExposedGrayLuminance                                 = m_ToneMapping.ComputePreExposedGrayLuminance(ToneParameters, true);
    if (m_Denoiser == RTXPTRealtimeDenoiser::NrdRelax)
    {
        //As RTXPTSample::UpdateFrameConstants converts the Sample's realtime firefly threshold.
        PT.denoisingEnabled                                = 1;
        PT.fireflyFilterThreshold                          = kNativeFireflyFilterThreshold * std::sqrt(PT.preExposedGrayLuminance) * kFireflyFilterThresholdScale;
        PT.denoiserRadianceClampK                          = kNativeDenoiserRadianceClampK;
        PT.denoisingLayersSuppressPrimaryIndirectSpecularK = kNativeSuppressPrimaryIndirectSpecK;
        PT.denoisingLayersAntiAliasingFallthrough          = kNativeAntiAliasingFallthrough;
        PT.allowPrimarySurfaceReplacement                  = kNativeAllowPrimarySurfaceReplacement;
    }
    else
    {
        PT.denoisingEnabled                                = 0;
        PT.fireflyFilterThreshold                          = 0;
        PT.denoiserRadianceClampK                          = 0;
        PT.denoisingLayersSuppressPrimaryIndirectSpecularK = 0;
    }
    {
        MapHelper<SampleConstants> Mapped{Context, m_Constants, MAP_WRITE, MAP_FLAG_DISCARD};
        if (!Mapped)
            return RTXPTErrorCode::ConstantsUpdateFailed;
        *Mapped = Constants;
    }
    SampleMiniConstants Mini{};
    Mini.params1 = uint4{1, 1, 1, 1};
    RTXPTRayTracingDispatch Dispatch;
    Dispatch.Width                     = Width;
    Dispatch.Height                    = Height;
    Dispatch.pMiniConstants            = &Mini;
    Dispatch.pSubSamplingPixelList     = m_PixelList->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
    Dispatch.pOutputColorUAV           = m_Targets.GetOutputColorUAV();
    Dispatch.pDepthUAV                 = m_Targets.GetDepthUAV();
    Dispatch.pMotionVectorsUAV         = m_Targets.GetScreenMotionVectorsUAV();
    Dispatch.pThroughputUAV            = m_Targets.GetThroughputUAV();
    Dispatch.pSpecularHitTUAV          = m_Targets.GetSpecularHitTUAV();
    Dispatch.pStableRadianceUAV        = m_Targets.GetStableRadianceUAV();
    Dispatch.pDenoisingLayersHeaderUAV = m_Targets.GetDenoisingLayersHeaderUAV();
    Dispatch.pDenoisingLayersBufferUAV = m_Targets.GetDenoisingLayersBufferUAV();
    {
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::Build};
        if (!Lights.PrepareLightBuffers(Context) || !Rays.Dispatch(Context, RTXPTPathTraceVariant::BuildDenoisingLayers, Dispatch))
            return RTXPTErrorCode::TraceFailed;
    }
    ++m_Stats.BuildDispatches;
    RTXPTRayTracingPass::InsertUAVBarrier(Context, m_Targets.GetDenoisingLayersBuffer());
    for (auto* View : {Dispatch.pDenoisingLayersHeaderUAV, Dispatch.pStableRadianceUAV, Dispatch.pDepthUAV,
                       Dispatch.pMotionVectorsUAV, Dispatch.pThroughputUAV})
        RTXPTRayTracingPass::InsertUAVBarrier(Context, View);
    Lights.UpdateFrame(Input.view.MatWorldToClip, Input.camera.PosW);
    {
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::LightsBaker};
        if (!Lights.UpdateEnd(Context, m_Targets.GetDepthSRV(), m_Targets.GetScreenMotionVectorsSRV()))
            return RTXPTErrorCode::LightSamplingSetupFailed;
    }
    RTXPTRayTracingPass::InsertUAVBarrier(Context, Dispatch.pSpecularHitTUAV);
    {
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::Fill};
        if (!Rays.Dispatch(Context, RTXPTPathTraceVariant::FillDenoisingLayers, Dispatch))
            return RTXPTErrorCode::TraceFailed;
    }
    ++m_Stats.FillDispatches;
    RTXPTRayTracingPass::InsertUAVBarrier(Context, m_Targets.GetDenoisingLayersBuffer());
    RTXPTRayTracingPass::InsertUAVBarrier(Context, Dispatch.pSpecularHitTUAV);
    {
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::Guides};
        if (!m_Guides.Bake(Context, m_Targets, RTXPTDenoisingGuideDebugView::Disabled, RTXPTGuideHistoryMode::CurrentFrame))
            return RTXPTErrorCode::GuidesFailed;
    }
    ++m_Stats.GuideBakes;
    if (m_Denoiser == RTXPTRealtimeDenoiser::NrdRelax)
    {
        const auto Code = Denoise(Context, Constants, History, Profiler);
        if (Code != RTXPTErrorCode::Ok)
            return Code;
    }
    else
    {
        RTXPTDenoiserPostProcessAttribs Merge;
        Merge.pRenderTargets  = &m_Targets;
        Merge.pMergeOutputUAV = m_Targets.GetOutputColorUAV();
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::Merge};
        if (!m_Merge.RunNoDenoiserFinalMerge(Context, Merge))
            return RTXPTErrorCode::RealtimeMergeFailed;
    }
    ++m_Stats.MergeDispatches;
    RTXPTRayTracingPass::InsertUAVBarrier(Context, Dispatch.pOutputColorUAV);
    RTXPTToneMappingRenderAttribs Tone;
    Tone.pSourceSRV = m_Targets.GetOutputColorSRV();
    Tone.pLdrRTV    = DisplayRTV;
    Tone.Width      = Width;
    Tone.Height     = Height;
    Tone.pParams    = &ToneParameters;
    {
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::ToneMapping};
        if (!m_ToneMapping.Render(Context, Tone))
            return RTXPTErrorCode::ToneMappingFailed;
    }
    ++m_Stats.CompletedFrames;
    m_Stats.OutputValid = true;
    return RTXPTErrorCode::Ok;
}

/* Denoises each active layer with its own RELAX instance (RTXPTNrdDenoiser): prepare demodulates the layer and writes
 * its history confidence (0 where the layer path has no valid previous correspondence), NRD denoises it with the
 * layer's history, and the merge remodulates it into the linear HDR output. History.Reset restarts every layer
 * (CLEAR_AND_RESTART). NRD advances its history with the dispatch, so a later failure of this frame leaves the host
 * to restart the next one, as for every failure.
 */
RTXPTErrorCode RTXPTRealtimePipeline::Denoise(IDeviceContext* Context, const SampleConstants& Constants, const RTXPTRealtimeHistoryAttribs& History, RTXPTGpuProfiler* Profiler)
{
    RTXPTNrdDenoiserFrameAttribs Attribs;
    Attribs.PostProcess.pRenderTargets  = &m_Targets;
    Attribs.PostProcess.pMergeOutputUAV = m_Targets.GetOutputColorUAV();
    Attribs.pFrameConstants             = &Constants;
    Attribs.pRealtime                   = &m_NrdSettings;
    Attribs.LayerCount                  = Constants.ptConsts._activeDenoisingLayerCount;
    Attribs.FrameIndex                  = Constants.ptConsts.frameIndex;
    Attribs.TimeDeltaSeconds            = History.TimeDeltaSeconds;
    Attribs.ResetHistory                = History.Reset;
    Attribs.HistoryConfidence           = m_Stats.HistoryConfidence;
    {
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::Denoise};
        if (!m_Nrd.Denoise(Context, m_Merge, Attribs, Profiler))
            return RTXPTErrorCode::DenoiseFailed;
    }
    ++m_Stats.DenoiseDispatches;
    m_Stats.HistoryResets += History.Reset ? 1u : 0u;
    m_Stats.LastHistoryReset = History.Reset;
    return RTXPTErrorCode::Ok;
}
} // namespace Diligent
