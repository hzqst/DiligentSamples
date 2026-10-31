#include "RTXPTRealtimeRawPipeline.hpp"
#include "MapHelper.hpp"
#include "../shaders/PathTracer/SubSampling.h"
#include <limits>

namespace Diligent
{
RTXPTErrorCode RTXPTRealtimeRawPipeline::Initialize(IRenderDevice* Device, IRenderStateCache* Cache, IPipelineStateCache* PSOCache, IBuffer* Constants, Uint32 Width, Uint32 Height, TEXTURE_FORMAT DisplayFormat)
{
    m_Ready = false;
    m_Stats = {};
    m_Targets.Reset();
    m_PixelList.Release();
    m_Constants     = Constants;
    m_DisplayFormat = DisplayFormat;
    if (!Device || !Cache || !Constants || !Width || !Height || DisplayFormat == TEX_FORMAT_UNKNOWN ||
        ComputeRTXPTLayerPlaneStride(Width, Height) > std::numeric_limits<Uint32>::max())
        return RTXPTErrorCode::InvalidArgument;

    RTXPTRenderTargetCreateInfo Targets;
    Targets.Dimensions              = {Width, Height, Width, Height, false};
    Targets.CreateRealtimeResources = true;
    if (!m_Targets.Resize(Device, Targets))
        return RTXPTErrorCode::FrameTargetsFailed;
    //The full-rate shader still declares the compaction list; with a 1x1 pattern it never reads entries.
    BufferDesc List;
    List.Name      = "RTXPT Raw full-rate pixel list";
    List.Size      = RTXPT_SUB_SAMPLING_LIST_HEADER_BYTES;
    List.BindFlags = BIND_UNORDERED_ACCESS;
    List.Mode      = BUFFER_MODE_RAW;
    Device->CreateBuffer(List, nullptr, &m_PixelList);
    if (!m_PixelList)
        return RTXPTErrorCode::FrameTargetsFailed;
    auto* Factory = Device->GetEngineFactory();
    if (!m_Guides.Initialize(Device, Factory, Cache, Constants, true) ||
        !m_Merge.Initialize(Device, Factory, Cache, Constants, true, RTXPTPostProcessPassSet::RealtimeRaw) ||
        !m_ToneMapping.Initialize(Device, Factory, Cache, PSOCache, DisplayFormat, true) ||
        !m_ToneMapping.ResizeResources(Device, Width, Height, Targets.Formats.OutputColor))
        return RTXPTErrorCode::PostProcessSetupFailed;
    m_Ready = true;
    return RTXPTErrorCode::Ok;
}

/* Build initializes headers/surfaces, then light preparation consumes this frame's depth/motion before Fill.
 * Fill produces one current sample per layer; guides are spatial/current-frame only. Merge produces linear HDR,
 * and ToneMapping writes the host's explicit RTV. Only completion of the entire chain publishes valid output.
 */
RTXPTErrorCode RTXPTRealtimeRawPipeline::Render(IDeviceContext* Context, RTXPTRayTracingPass& Rays, RTXPTLightsBaker& Lights, const SampleConstants& Input, const RTXPTToneMappingParameters& ToneParameters, ITextureView* DisplayRTV)
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
    PT.denoisingEnabled       = 0;
    PT.useReSTIRDI = PT.useReSTIRGI = PT.superResolutionActive = 0;
    PT.perPixelJitterAAScale                                   = 0;
    PT.fireflyFilterThreshold                                  = 0;
    PT.denoiserRadianceClampK = PT.DLSSRRBrightnessClampK = 0;
    PT.denoisingLayersSuppressPrimaryIndirectSpecularK    = 0;
    PT._activeDenoisingLayerCount                         = kRTXPTDenoisingLayerCount;
    constexpr Uint32 MaxLayerVertexDepth                  = 9;
    PT.maxDenoisingLayerVertexDepth                       = std::min(PT.bounceCount, MaxLayerVertexDepth);
    PT.genericTSLineStride                                = static_cast<Uint32>(ComputeRTXPTLayerLineStride(Width));
    PT.genericTSPlaneStride                               = static_cast<Uint32>(ComputeRTXPTLayerPlaneStride(Width, Height));
    PT.preExposedGrayLuminance                            = m_ToneMapping.ComputePreExposedGrayLuminance(ToneParameters, true);
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
    if (!Lights.PrepareLightBuffers(Context) || !Rays.Dispatch(Context, RTXPTPathTraceVariant::BuildDenoisingLayers, Dispatch))
        return RTXPTErrorCode::TraceFailed;
    ++m_Stats.BuildDispatches;
    RTXPTRayTracingPass::InsertUAVBarrier(Context, m_Targets.GetDenoisingLayersBuffer());
    for (auto* View : {Dispatch.pDenoisingLayersHeaderUAV, Dispatch.pStableRadianceUAV, Dispatch.pDepthUAV,
                       Dispatch.pMotionVectorsUAV, Dispatch.pThroughputUAV})
        RTXPTRayTracingPass::InsertUAVBarrier(Context, View);
    Lights.UpdateFrame(Input.view.MatWorldToClip, Input.camera.PosW);
    if (!Lights.UpdateEnd(Context, m_Targets.GetDepthSRV(), m_Targets.GetScreenMotionVectorsSRV()))
        return RTXPTErrorCode::LightSamplingSetupFailed;
    RTXPTRayTracingPass::InsertUAVBarrier(Context, Dispatch.pSpecularHitTUAV);
    if (!Rays.Dispatch(Context, RTXPTPathTraceVariant::FillDenoisingLayers, Dispatch))
        return RTXPTErrorCode::TraceFailed;
    ++m_Stats.FillDispatches;
    RTXPTRayTracingPass::InsertUAVBarrier(Context, m_Targets.GetDenoisingLayersBuffer());
    RTXPTRayTracingPass::InsertUAVBarrier(Context, Dispatch.pSpecularHitTUAV);
    if (!m_Guides.Bake(Context, m_Targets, RTXPTDenoisingGuideDebugView::Disabled, RTXPTGuideHistoryMode::CurrentFrame))
        return RTXPTErrorCode::GuidesFailed;
    ++m_Stats.GuideBakes;
    RTXPTDenoiserPostProcessAttribs Merge;
    Merge.pRenderTargets  = &m_Targets;
    Merge.pMergeOutputUAV = m_Targets.GetOutputColorUAV();
    if (!m_Merge.RunNoDenoiserFinalMerge(Context, Merge))
        return RTXPTErrorCode::RealtimeMergeFailed;
    ++m_Stats.MergeDispatches;
    RTXPTRayTracingPass::InsertUAVBarrier(Context, Dispatch.pOutputColorUAV);
    RTXPTToneMappingRenderAttribs Tone;
    Tone.pSourceSRV = m_Targets.GetOutputColorSRV();
    Tone.pLdrRTV    = DisplayRTV;
    Tone.Width      = Width;
    Tone.Height     = Height;
    Tone.pParams    = &ToneParameters;
    if (!m_ToneMapping.Render(Context, Tone))
        return RTXPTErrorCode::ToneMappingFailed;
    ++m_Stats.CompletedFrames;
    m_Stats.OutputValid = true;
    return RTXPTErrorCode::Ok;
}
} // namespace Diligent
