#include "RTXPTRealtimePipeline.hpp"
#include "AsyncPrefix.hpp"
#include "MapHelper.hpp"
#include "RTXPTNrdConfig.hpp"
#include "RTXPTSuperResolutionInputs.hpp"
#if RTXPT_HAS_RAY_RECONSTRUCTION
#    include "RTXPTRayReconstructionInputs.hpp"
#endif
#include "../shaders/PathTracer/SubSampling.h"
#include <cmath>
#include <limits>

namespace Diligent
{
namespace
{
//RealtimeNative traces and prepares its layers with the Sample's realtime defaults (RTXPTRealtimeSettings), the
//configuration whose RELAX output the M3.0 baseline thresholds were measured on. RealtimeDLSSRR conditions its layers
//the same way (D5).
constexpr float  kNativeFireflyFilterThreshold         = 0.10f;
constexpr float  kFireflyFilterThresholdScale          = 1000.0f;
constexpr float  kNativeDenoiserRadianceClampK         = 8.0f;
constexpr float  kNativeSuppressPrimaryIndirectSpecK   = 0.6f;
constexpr float  kNativeAntiAliasingFallthrough        = 0.6f;
constexpr Uint32 kNativeAllowPrimarySurfaceReplacement = 1;
constexpr Uint32 kMaxLayerVertexDepth                  = 9;
//RealtimeDLSSRR clamps the noisy radiance RR receives to this multiple of the pre-exposed gray luminance, the Sample's
//default (RTXPTRealtimeSettings::DLSSRRBrightnessClampK, D5).
constexpr float kReconstructionBrightnessClampK = 4096.0f;

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
    m_SuperResolutionInputs.Reset();
    m_PixelList.Release();
    m_ReconstructionExposure.Release();
    m_ReconstructionExposureRead.Release();
    m_Constants                     = CreateInfo.Constants;
    m_DisplayFormat                 = CreateInfo.DisplayFormat;
    m_Denoiser                      = CreateInfo.Denoiser;
    m_Reconstructor                 = CreateInfo.Reconstructor;
    m_ReconstructionDisplayExposure = CreateInfo.ReconstructionDisplayExposure;
    m_ReconstructionExposureValue   = 0;
    m_SuperResolution               = CreateInfo.SuperResolution;
    auto* const  Device             = CreateInfo.Device;
    auto* const  Cache              = CreateInfo.Cache;
    const Uint32 Width = CreateInfo.Width, Height = CreateInfo.Height;
    const bool   bReconstruction  = m_Reconstructor == RTXPTRealtimeReconstructor::DLSSRR;
    const bool   bDisplaySize     = m_SuperResolution || bReconstruction;
    const Uint32 DisplayWidth     = bDisplaySize ? CreateInfo.DisplayWidth : Width;
    const Uint32 DisplayHeight    = bDisplaySize ? CreateInfo.DisplayHeight : Height;
    const bool   bReconstructorOk = m_Reconstructor == RTXPTRealtimeReconstructor::None ||
        (bReconstruction && m_Denoiser == RTXPTRealtimeDenoiser::None && !m_SuperResolution);
    if (!Device || !Cache || !CreateInfo.Constants || !Width || !Height || CreateInfo.DisplayFormat == TEX_FORMAT_UNKNOWN ||
        ComputeRTXPTLayerPlaneStride(Width, Height) > std::numeric_limits<Uint32>::max() ||
        DisplayWidth < Width || DisplayHeight < Height || !bReconstructorOk ||
        (!bDisplaySize && (CreateInfo.DisplayWidth != 0 || CreateInfo.DisplayHeight != 0)))
        return RTXPTErrorCode::InvalidArgument;
#if !RTXPT_HAS_RAY_RECONSTRUCTION
    //The library was built without the ray reconstruction interface (add_rtxpt_library).
    if (bReconstruction)
        return RTXPTErrorCode::RayReconstructionUnavailable;
#endif
    const bool bNative = m_Denoiser == RTXPTRealtimeDenoiser::NrdRelax;
    if (bNative && !RTXPTIsNrdAvailable())
        return RTXPTErrorCode::DenoiserUnavailable;
    RTXPTRenderTargetCreateInfo Targets;
    if (bNative && !SupportsNrdInputs(Device, Targets.Formats))
        return RTXPTErrorCode::DenoiserUnsupportedDevice;

    //Ray reconstruction's targets are those of RTXPTRayReconstructionInputs::MakeRenderTargetCreateInfo: the display
    //size is distinct exactly when the render size is smaller, and the NRD targets are not allocated.
    const bool bUpscaled                     = m_SuperResolution || (bReconstruction && (DisplayWidth != Width || DisplayHeight != Height));
    Targets.Dimensions                       = {Width, Height, DisplayWidth, DisplayHeight, bUpscaled};
    Targets.CreateRealtimeResources          = true;
    Targets.CreateRayReconstructionResources = bReconstruction;
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
    if (bReconstruction)
    {
        //RR reads the exposure of its scene-linear input from this texture (D4); R32F holds every display exposure a
        //view accepts. Render writes the exposure of each frame before Execute.
        const float       InitialExposure = 1.0f;
        TextureSubResData ExposureData{&InitialExposure, sizeof(InitialExposure)};
        TextureData       ExposureInit{&ExposureData, 1};
        TextureDesc       ExposureDesc;
        ExposureDesc.Name                 = "RTXPT ray reconstruction exposure";
        ExposureDesc.Type                 = RESOURCE_DIM_TEX_2D;
        ExposureDesc.Width                = 1;
        ExposureDesc.Height               = 1;
        ExposureDesc.Format               = TEX_FORMAT_R32_FLOAT;
        ExposureDesc.Usage                = USAGE_DEFAULT;
        ExposureDesc.BindFlags            = BIND_SHADER_RESOURCE;
        ExposureDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
        Device->CreateTexture(ExposureDesc, &ExposureInit, &m_ReconstructionExposure);
        if (!m_ReconstructionExposure)
            return RTXPTErrorCode::FrameTargetsFailed;
        m_ReconstructionExposureValue = InitialExposure;
    }
    auto*      Factory = Device->GetEngineFactory();
    const auto PassSet = bReconstruction ? RTXPTPostProcessPassSet::RealtimeRR :
                                           (bNative ? RTXPTPostProcessPassSet::RealtimeNative : RTXPTPostProcessPassSet::RealtimeRaw);
    if (!m_Guides.Initialize(Device, Factory, Cache, m_Constants, true) ||
        !m_Merge.Initialize(Device, Factory, Cache, m_Constants, true, PassSet) ||
        !m_PostProcess.Initialize(Device, Cache, CreateInfo.PSOCache, DisplayWidth, DisplayHeight, m_DisplayFormat))
        return RTXPTErrorCode::PostProcessSetupFailed;
    if (m_SuperResolution &&
        (!m_SuperResolutionInputs.Initialize(Device, Factory, Cache, m_Constants) || !m_SuperResolutionInputs.Resize(Device, Width, Height)))
        return RTXPTErrorCode::SuperResolutionInputsFailed;
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
 * Native denoises them first (Denoise); both produce render-size linear HDR. A SuperResolution pipeline upscales it to
 * the display size (Upscale). DLSSRR instead prepares RR's noisy radiance and guides from the layers and the denoised
 * specular hit distance of the guides, and RR reconstructs the display-size HDR (Reconstruct). The display post
 * processing (RTXPTViewPostProcess) meters that HDR for auto exposure, blooms a copy of it and tone maps it into the
 * host's explicit RTV at the display size. Only completion of the entire chain publishes valid output.
 */
RTXPTErrorCode RTXPTRealtimePipeline::Render(IDeviceContext* Context, RTXPTRayTracingPass& Rays, RTXPTLightsBaker& Lights, const SampleConstants& Input, const RTXPTToneMappingParameters& ToneParameters, const RTXPTBloomParameters& Bloom, RTXPTAutoExposure* AutoExposure, const RTXPTRealtimeHistoryAttribs& History, ITextureView* DisplayRTV, RTXPTGpuProfiler* Profiler)
{
    m_Stats.OutputValid = false;
    if (!m_Ready)
        return RTXPTErrorCode::NotInitialized;
    const bool   bReconstruction = m_Reconstructor == RTXPTRealtimeReconstructor::DLSSRR;
    const Uint32 Width = m_Targets.GetRenderWidth(), Height = m_Targets.GetRenderHeight();
    const Uint32 DisplayWidth = m_Targets.GetDisplayWidth(), DisplayHeight = m_Targets.GetDisplayHeight();
    if (!Context || !DisplayRTV || DisplayRTV->GetDesc().Format != m_DisplayFormat ||
        DisplayRTV->GetTexture()->GetDesc().Width != DisplayWidth || DisplayRTV->GetTexture()->GetDesc().Height != DisplayHeight ||
        Input.ptConsts.imageWidth != Width || Input.ptConsts.imageHeight != Height ||
        Input.ptConsts.NEEType > 1 || ToneParameters.AutoExposure || (m_SuperResolution && !History.Upscaler) ||
        (bReconstruction && !History.Reconstruction) || (AutoExposure && !AutoExposure->GetUsedExposure()))
        return RTXPTErrorCode::InvalidArgument;

    SampleConstants Constants = Input;
    auto&           PT        = Constants.ptConsts;
    PT.invSubSampleCount      = 1.0f;
    PT.useReSTIRDI = PT.useReSTIRGI = 0;
    PT.superResolutionActive        = m_SuperResolution ? 1u : 0u;
    PT.perPixelJitterAAScale        = 0;
    PT.DLSSRRBrightnessClampK       = 0;
    PT._activeDenoisingLayerCount   = kRTXPTDenoisingLayerCount;
    PT.maxDenoisingLayerVertexDepth = std::min(PT.bounceCount, kMaxLayerVertexDepth);
    PT.genericTSLineStride          = static_cast<Uint32>(ComputeRTXPTLayerLineStride(Width));
    PT.genericTSPlaneStride         = static_cast<Uint32>(ComputeRTXPTLayerPlaneStride(Width, Height));
    if (AutoExposure)
    {
        RTXPTToneMappingParameters TraceClamp = ToneParameters;
        TraceClamp.ExposureCompensation       = std::log2(RTXPTAutoExposureTraceClampExposure);
        PT.preExposedGrayLuminance            = m_PostProcess.GetToneMapping().ComputePreExposedGrayLuminance(TraceClamp, true);
    }
    else
    {
        PT.preExposedGrayLuminance = m_PostProcess.GetToneMapping().ComputePreExposedGrayLuminance(ToneParameters, true);
    }
    //Textures are sampled for the output resolution the upscaler or reconstruction produces.
    if (DisplayWidth != Width || DisplayHeight != Height)
        PT.texLODBias += RTXPTSuperResolutionInputs::ComputeTexLODBias(Width, Height, DisplayWidth, DisplayHeight);
    if (m_Denoiser == RTXPTRealtimeDenoiser::NrdRelax || bReconstruction)
    {
        //As RTXPTSample::UpdateFrameConstants converts the Sample's realtime firefly threshold.
        PT.denoisingEnabled                                = 1;
        PT.fireflyFilterThreshold                          = kNativeFireflyFilterThreshold * std::sqrt(PT.preExposedGrayLuminance) * kFireflyFilterThresholdScale;
        PT.denoiserRadianceClampK                          = kNativeDenoiserRadianceClampK;
        PT.denoisingLayersSuppressPrimaryIndirectSpecularK = kNativeSuppressPrimaryIndirectSpecK;
        PT.denoisingLayersAntiAliasingFallthrough          = kNativeAntiAliasingFallthrough;
        PT.allowPrimarySurfaceReplacement                  = kNativeAllowPrimarySurfaceReplacement;
        if (bReconstruction)
            PT.DLSSRRBrightnessClampK = kReconstructionBrightnessClampK * PT.preExposedGrayLuminance;
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
    if (bReconstruction)
    {
        const auto Code = Reconstruct(Context, Constants, ToneParameters, AutoExposure, History, Profiler);
        if (Code != RTXPTErrorCode::Ok)
            return Code;
    }
    else
    {
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
    }
    if (m_SuperResolution)
    {
        const auto Code = Upscale(Context, Constants, History, Profiler);
        if (Code != RTXPTErrorCode::Ok)
            return Code;
    }
    //Every Execute of the upscaler or reconstruction flushes and invalidates the context state; the display post
    //processing sets all of its own. It meters the HDR of the mode before Bloom, and leaves that HDR as the frame's
    //captures publish it.
    ITextureView* HDR      = m_SuperResolution || bReconstruction ? m_Targets.GetProcessedOutputColorSRV() : m_Targets.GetOutputColorSRV();
    const auto    PostCode = m_PostProcess.Apply(Context, HDR->GetTexture(), ToneParameters, Bloom, AutoExposure, DisplayRTV, Profiler);
    if (PostCode != RTXPTErrorCode::Ok)
        return PostCode;
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

/* Converts this frame's guides into the upscaler's render-size depth, motion and ignore-history mask with the uploaded
 * constants, so depth reconstructs the rays BUILD traced, then upscales the merged HDR into the display-size output.
 * The upscaler receives exposure 1 and no sharpening; display exposure stays with tone mapping. Its Execute
 * transitions every input and output.
 */
RTXPTErrorCode RTXPTRealtimePipeline::Upscale(IDeviceContext* Context, const SampleConstants& Constants, const RTXPTRealtimeHistoryAttribs& History, RTXPTGpuProfiler* Profiler)
{
    {
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::SuperResolutionInputs};
        if (!m_SuperResolutionInputs.Prepare(Context, m_Targets.GetDepthSRV(), m_Targets.GetScreenMotionVectorsSRV()))
            return RTXPTErrorCode::SuperResolutionInputsFailed;
    }
    ExecuteSuperResolutionAttribs Attribs;
    if (!RTXPTSuperResolutionInputs::MakeExecuteAttribs(Context, History.Upscaler->GetDesc(), m_SuperResolutionInputs,
                                                        m_Targets.GetOutputColorSRV(), m_Targets.GetProcessedOutputColorUAV(),
                                                        Constants, History.Reset, std::max(History.TimeDeltaSeconds, 0.0f), Attribs))
        return RTXPTErrorCode::InvalidArgument;
    {
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::SuperResolution};
        if (!History.Upscaler->Execute(Attribs))
            return RTXPTErrorCode::SuperResolutionExecuteFailed;
    }
    ++m_Stats.SuperResolutionExecutes;
    m_Stats.SuperResolutionResets += History.Reset ? 1u : 0u;
    return RTXPTErrorCode::Ok;
}

/* Prepares RR's render-size noisy radiance (OutputColor, clamped to DLSSRRBrightnessClampK) and guides from this
 * frame's layers with the uploaded constants, then RR reconstructs the display-size HDR (ProcessedOutputColor). The
 * color stays scene-linear: RR receives the exposure tone mapping applies through the exposure texture (D4), with
 * PreExposure and ExposureScale 1, so the output stays in the input's scale and tone mapping alone applies the display
 * exposure. Manual exposure is written into the pipeline's own texture; auto exposure hands RR the view's used exposure
 * of this frame, the texture tone mapping reads as well. The feature never runs its own auto exposure. The rays carry
 * RR's camera jitter alone (D5). Execute transitions every input and output.
 */
RTXPTErrorCode RTXPTRealtimePipeline::Reconstruct(IDeviceContext* Context, const SampleConstants& Constants, const RTXPTToneMappingParameters& ToneParameters, RTXPTAutoExposure* AutoExposure, const RTXPTRealtimeHistoryAttribs& History, RTXPTGpuProfiler* Profiler)
{
#if RTXPT_HAS_RAY_RECONSTRUCTION
    {
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::RRPrepare};
        if (!m_Merge.RunDLSSRRDenoiserPrepareInputs(Context, m_Targets))
            return RTXPTErrorCode::RayReconstructionInputsFailed;
    }
    ++m_Stats.RayReconstructionPrepares;
    //The validation control that hands RR exposure 1 applies to both modes.
    const bool  bAutoExposure = AutoExposure && m_ReconstructionDisplayExposure;
    const float Exposure      = !m_ReconstructionDisplayExposure ? 1.0f : bAutoExposure ? 0.0f : m_PostProcess.GetToneMapping().ComputeExposureScale(ToneParameters);
    if (!bAutoExposure && (!std::isfinite(Exposure) || !(Exposure > 0)))
        return RTXPTErrorCode::RayReconstructionInputsFailed;
    if (!bAutoExposure && Exposure != m_ReconstructionExposureValue)
    {
        Context->UpdateTexture(m_ReconstructionExposure, 0, 0, Box{0, 1, 0, 1}, TextureSubResData{&Exposure, sizeof(Exposure)},
                               RESOURCE_STATE_TRANSITION_MODE_TRANSITION, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        m_ReconstructionExposureValue = Exposure;
    }
    ITexture* ExposureTexture = bAutoExposure ? AutoExposure->GetUsedExposure() : m_ReconstructionExposure.RawPtr();
    //The forward sign MakeExecuteAttribs hands RR the depth basis with; a projection without one fails both.
    float ForwardSign = 0;
    if (!RTXPTRayReconstructionInputs::GetViewForwardSign(Constants.view.MatViewToClip, ForwardSign))
        return RTXPTErrorCode::RayReconstructionInputsFailed;
    ExecuteRayReconstructionAttribs Attribs;
    if (!RTXPTRayReconstructionInputs::MakeExecuteAttribsWithoutMicroJitter(Context, m_Targets, History.Reconstruction->GetDesc(), Constants, History.Reset,
                                                                            ExposureTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), Attribs))
        return RTXPTErrorCode::RayReconstructionInputsFailed;
    {
        RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::RR};
        if (!History.Reconstruction->Execute(Attribs))
            return RTXPTErrorCode::RayReconstructionExecuteFailed;
    }
    ++m_Stats.RayReconstructionExecutes;
    m_Stats.RayReconstructionResets += History.Reset ? 1u : 0u;
    m_Stats.RayReconstructionExposure     = Exposure;
    m_Stats.RayReconstructionAutoExposure = bAutoExposure;
    m_Stats.RayReconstructionForwardSign  = ForwardSign;
    m_ReconstructionExposureRead          = ExposureTexture;
    return RTXPTErrorCode::Ok;
#else
    //Initialize rejects DLSSRR when the library has no ray reconstruction interface.
    (void)Context, (void)Constants, (void)ToneParameters, (void)AutoExposure, (void)History, (void)Profiler;
    return RTXPTErrorCode::RayReconstructionUnavailable;
#endif
}
} // namespace Diligent
