/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#include "RTXPTPostProcessPipeline.hpp"

#include <algorithm>

#include "RTXPTGpuProfiler.hpp"

#include "DebugUtilities.hpp"
#include "RenderStateCache.h"

namespace Diligent
{

namespace
{

RTXPTDenoiserPostProcessAttribs MakeRealtimePostProcessAttribs(const RTXPTRenderTargets& RenderTargets)
{
    RTXPTDenoiserPostProcessAttribs Attribs;
    Attribs.pRenderTargets  = &RenderTargets;
    Attribs.pMergeOutputUAV = RenderTargets.GetOutputColorUAV();
    return Attribs;
}

} // namespace

void RTXPTPostProcessPipeline::Reset()
{
    m_RealtimeCopyContext.reset();
    m_AccumulationPass.Reset();
    m_PostProcessPass.Reset();
    m_SuperResolutionPass.Reset();
    m_RayReconstructionPass.Reset();
    m_TemporalAAPass.Reset();
    m_BloomPass.Reset();
    m_ToneMappingPass.Reset();
    m_Device.Release();
    m_Stats                = {};
    m_ToneMappingRTVFormat = TEX_FORMAT_UNKNOWN;
}

bool RTXPTPostProcessPipeline::Initialize(IRenderDevice* pDevice, const RTXPTPostProcessPipelineCreateInfo& CreateInfo)
{
    Reset();

    if (pDevice == nullptr || CreateInfo.pEngineFactory == nullptr || CreateInfo.ToneMappingRTVFormat == TEX_FORMAT_UNKNOWN || CreateInfo.pFrameConstants == nullptr)
    {
        DEV_ERROR("RTXPT post-process pipeline requires a device, engine factory, output RTV format, and frame constants buffer");
        return false;
    }

    if (!CreateInfo.ComputeSupported)
    {
        DEV_ERROR("RTXPT post-process pipeline requires compute shader support");
        return false;
    }

    m_Device               = pDevice;
    m_ToneMappingRTVFormat = CreateInfo.ToneMappingRTVFormat;
    m_RayReconstructionPass.Initialize(pDevice);

    PostFXContext::CreateInfo RealtimeCopyCI;
    RealtimeCopyCI.EnableAsyncCreation = false;
    RealtimeCopyCI.PackMatrixRowMajor  = true;
    m_RealtimeCopyContext              = std::make_unique<PostFXContext>(pDevice, RealtimeCopyCI);
    if (!m_RealtimeCopyContext)
    {
        DEV_ERROR("RTXPT realtime OutputColor copy context failed to initialize");
        return false;
    }

    m_Stats.SuperResolutionStageReady = m_SuperResolutionPass.Initialize(pDevice, CreateInfo.pStateCache);
    if (!m_Stats.SuperResolutionStageReady)
    {
        DEV_ERROR("RTXPT super-resolution pass failed to initialize");
        return false;
    }

    m_Stats.TemporalAAStageReady = m_TemporalAAPass.Initialize(pDevice, CreateInfo.pStateCache);
    if (!m_Stats.TemporalAAStageReady)
    {
        LOG_WARNING_MESSAGE("RTXPT temporal AA pass is unavailable: ", m_TemporalAAPass.GetStats().DisabledReason.c_str());
    }

    m_Stats.AccumulationStageReady = m_AccumulationPass.Initialize(pDevice, CreateInfo.pEngineFactory, CreateInfo.pStateCache, CreateInfo.pPSOCache, CreateInfo.ComputeSupported);
    if (!m_Stats.AccumulationStageReady)
    {
        DEV_ERROR("RTXPT accumulation pass failed to initialize");
        return false;
    }

    m_Stats.RealtimeMergeStageReady =
        m_PostProcessPass.Initialize(pDevice, CreateInfo.pEngineFactory, CreateInfo.pStateCache, CreateInfo.pFrameConstants, CreateInfo.ComputeSupported);
    if (!m_Stats.RealtimeMergeStageReady)
    {
        DEV_ERROR("RTXPT realtime post-process pass failed to initialize");
        return false;
    }

    m_Stats.ToneMappingStageReady =
        m_ToneMappingPass.Initialize(pDevice, CreateInfo.pEngineFactory, CreateInfo.pStateCache, CreateInfo.pPSOCache, CreateInfo.ToneMappingRTVFormat, CreateInfo.ComputeSupported);
    if (!m_Stats.ToneMappingStageReady)
    {
        DEV_ERROR("RTXPT tone mapping pass failed to initialize");
        return false;
    }

    m_Stats.BloomStageReady = m_BloomPass.Initialize(pDevice, CreateInfo.pEngineFactory, CreateInfo.pStateCache);
    if (!m_Stats.BloomStageReady)
    {
        DEV_ERROR("RTXPT bloom pass failed to initialize");
        return false;
    }

    m_Stats.Ready = true;
    return true;
}

bool RTXPTPostProcessPipeline::ValidateRenderTargets(const RTXPTRenderTargets& RenderTargets)
{
    const bool AccumulationResourcesValid =
        !RenderTargets.IsAccumulationActive() ||
        (RenderTargets.GetAccumulatedRadianceSRV() != nullptr && RenderTargets.GetAccumulatedRadianceUAV() != nullptr);
    const bool SuperResolutionResourcesValid =
        !RenderTargets.IsSuperResolutionActive() ||
        (RenderTargets.GetSuperResolutionInputColorSRV() != nullptr &&
         RenderTargets.GetTemporalFeedback1UAV() != nullptr &&
         RenderTargets.GetTemporalFeedback2UAV() != nullptr &&
         RenderTargets.GetCombinedHistoryClampRelaxUAV() != nullptr);
    const bool RealtimeResourcesValid =
        !RenderTargets.AreRealtimeRenderTargetsRequested() ||
        (RenderTargets.HasRealtimeRenderTargets() &&
         RenderTargets.GetCombinedHistoryClampRelaxUAV() != nullptr &&
         RenderTargets.GetAccumulationOutputUAV() != nullptr);

    m_Stats.ResourcesValid =
        RenderTargets.GetOutputColorSRV() != nullptr &&
        RenderTargets.GetOutputColorUAV() != nullptr &&
        AccumulationResourcesValid &&
        RenderTargets.GetAccumulationOutputUAV() != nullptr &&
        RenderTargets.GetDepthUAV() != nullptr &&
        RenderTargets.GetDepthSRV() != nullptr &&
        RenderTargets.GetScreenMotionVectorsUAV() != nullptr &&
        RenderTargets.GetScreenMotionVectorsSRV() != nullptr &&
        SuperResolutionResourcesValid &&
        RealtimeResourcesValid &&
        RenderTargets.GetProcessedOutputColorSRV() != nullptr &&
        RenderTargets.GetProcessedOutputColorUAV() != nullptr &&
        RenderTargets.GetProcessedOutputColorRTV() != nullptr &&
        RenderTargets.GetLdrColorSRV() != nullptr &&
        RenderTargets.GetLdrColorUAV() != nullptr &&
        RenderTargets.GetLdrColorRTV() != nullptr;

    if (!m_Stats.ResourcesValid)
        DEV_ERROR("RTXPT post-process render targets are incomplete");

    return m_Stats.ResourcesValid;
}

RTXPTSuperResolutionFrameDesc RTXPTPostProcessPipeline::ResolveSuperResolutionFrameDesc(const RTXPTSuperResolutionSettings& Settings,
                                                                                        Uint32                              DisplayWidth,
                                                                                        Uint32                              DisplayHeight,
                                                                                        TEXTURE_FORMAT                      OutputFormat,
                                                                                        bool                                ResetHistory,
                                                                                        float                               TimeDeltaSeconds)
{
    return m_SuperResolutionPass.ResolveFrameDesc(Settings, DisplayWidth, DisplayHeight, OutputFormat, ResetHistory, TimeDeltaSeconds);
}

bool RTXPTPostProcessPipeline::RunAccumulation(IDeviceContext*           pContext,
                                               const RTXPTRenderTargets& RenderTargets,
                                               Uint32                    SampleIndex,
                                               bool                      ResetAccumulation)
{
    if (!m_AccumulationPass.IsReady())
        return false;
    if (RenderTargets.GetAccumulatedRadianceSRV() == nullptr || RenderTargets.GetAccumulatedRadianceUAV() == nullptr)
    {
        DEV_ERROR("RTXPT accumulation pass requires accumulated radiance SRV and UAV");
        return false;
    }

    const Uint32 ClampedSampleIndex = std::max(SampleIndex, 1u);
    const float  BlendFactor        = ResetAccumulation ? 1.0f : 1.0f / static_cast<float>(ClampedSampleIndex);

    RTXPTAccumulationDispatch Dispatch;
    Dispatch.pInputColorSRV          = RenderTargets.GetOutputColorSRV();
    Dispatch.pAccumulatedRadianceUAV = RenderTargets.GetAccumulatedRadianceUAV();
    Dispatch.pProcessedOutputUAV     = RenderTargets.GetAccumulationOutputUAV();
    Dispatch.InputWidth              = RenderTargets.GetRenderWidth();
    Dispatch.InputHeight             = RenderTargets.GetRenderHeight();
    Dispatch.OutputWidth             = RenderTargets.GetRenderWidth();
    Dispatch.OutputHeight            = RenderTargets.GetRenderHeight();
    Dispatch.PixelOffset             = float2{0.0f, 0.0f};
    Dispatch.BlendFactor             = BlendFactor;

    const bool Executed            = m_AccumulationPass.Render(pContext, Dispatch);
    m_Stats.AccumulationStageReady = m_AccumulationPass.IsReady();
    return Executed;
}

bool RTXPTPostProcessPipeline::RunDenoiserPrepare(IDeviceContext*           pContext,
                                                  const RTXPTRenderTargets& RenderTargets,
                                                  RTXPTNrdMethod            Method,
                                                  Uint32                    PlaneIndex,
                                                  bool                      InitOutput)
{
    RTXPTDenoiserPostProcessAttribs Attribs = MakeRealtimePostProcessAttribs(RenderTargets);
    Attribs.Method                          = Method;
    Attribs.PlaneIndex                      = PlaneIndex;
    Attribs.InitOutput                      = InitOutput;
    Attribs.MiniConstants.params.x          = PlaneIndex;
    Attribs.MiniConstants.params.y          = InitOutput ? 1u : 0u;
    return m_PostProcessPass.RunDenoiserPrepare(pContext, Attribs);
}

bool RTXPTPostProcessPipeline::RunDenoiserFinalMerge(IDeviceContext*           pContext,
                                                     const RTXPTRenderTargets& RenderTargets,
                                                     RTXPTNrdMethod            Method,
                                                     Uint32                    PlaneIndex,
                                                     bool                      HasValidation)
{
    RTXPTDenoiserPostProcessAttribs Attribs = MakeRealtimePostProcessAttribs(RenderTargets);
    Attribs.Method                          = Method;
    Attribs.PlaneIndex                      = PlaneIndex;
    Attribs.HasValidation                   = HasValidation;
    Attribs.MiniConstants.params.x          = PlaneIndex;
    Attribs.MiniConstants.params.y          = HasValidation ? 1u : 0u;

    const bool Executed                 = m_PostProcessPass.RunDenoiserFinalMerge(pContext, Attribs);
    m_Stats.RealtimeMergeStageReady     = m_PostProcessPass.IsReady();
    m_Stats.LastRealtimeFinalMergeReady = Executed;
    if (!Executed)
        DEV_ERROR("RTXPT denoiser final merge failed");
    return Executed;
}

bool RTXPTPostProcessPipeline::RunNoDenoiserFinalMerge(IDeviceContext*           pContext,
                                                       const RTXPTRenderTargets& RenderTargets)
{
    RTXPTDenoiserPostProcessAttribs Attribs  = MakeRealtimePostProcessAttribs(RenderTargets);
    const bool                      Executed = m_PostProcessPass.RunNoDenoiserFinalMerge(pContext, Attribs);
    m_Stats.RealtimeMergeStageReady          = m_PostProcessPass.IsReady();
    m_Stats.LastRealtimeFinalMergeReady      = Executed;
    if (!Executed)
        DEV_ERROR("RTXPT no-denoiser final merge failed");
    return Executed;
}

bool RTXPTPostProcessPipeline::RunSuperResolution(IDeviceContext*                      pContext,
                                                  const RTXPTRenderTargets&            RenderTargets,
                                                  const RTXPTSuperResolutionFrameDesc& FrameDesc,
                                                  float                                CameraNear,
                                                  float                                CameraFar,
                                                  float                                CameraFovAngleVert)
{
    RTXPTSuperResolutionExecuteAttribs ExecuteAttribs;
    ExecuteAttribs.pRenderTargets     = &RenderTargets;
    ExecuteAttribs.pFrameDesc         = &FrameDesc;
    ExecuteAttribs.CameraNear         = CameraNear;
    ExecuteAttribs.CameraFar          = CameraFar;
    ExecuteAttribs.CameraFovAngleVert = CameraFovAngleVert;

    const bool  Executed = m_SuperResolutionPass.Execute(pContext, ExecuteAttribs);
    const auto& SRStats  = m_SuperResolutionPass.GetStats();

    m_Stats.SuperResolutionStageReady = !FrameDesc.Enabled || (Executed && SRStats.UpscalerReady);
    m_Stats.LastSuperResolutionActive = Executed && SRStats.LastExecute && FrameDesc.Enabled;
    if (!Executed && SRStats.DisabledReason.empty())
        DEV_ERROR("RTXPT temporal super-resolution pass failed");
    return Executed;
}

float2 RTXPTPostProcessPipeline::GetRealtimeTAAPixelJitter(Uint32 FrameIndex) const
{
    return RTXPTTemporalAAPass::ComputePixelJitter(FrameIndex);
}

bool RTXPTPostProcessPipeline::PrepareRayReconstruction(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets, RTXPTGpuProfiler* Profiler)
{
    m_Stats.LastRealtimeCopyExecuted  = false;
    m_Stats.LastTemporalAAActive      = false;
    m_Stats.LastSuperResolutionActive = false;
    if (Profiler)
        Profiler->Begin(pContext, RTXPTGpuProfiler::Stage::RRPrepare);
    const bool Prepared = m_PostProcessPass.RunDLSSRRDenoiserPrepareInputs(pContext, RenderTargets);
    if (Profiler)
        Profiler->End(pContext, RTXPTGpuProfiler::Stage::RRPrepare);
    if (!Prepared)
    {
        m_RayReconstructionPass.NotifyInputPreparationFailed();
        return false;
    }
    return true;
}

bool RTXPTPostProcessPipeline::RunRayReconstruction(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets, const SampleConstants& FrameConstants, bool ResetHistory, RTXPTGpuProfiler* Profiler, bool InputsPrepared)
{
    if (!InputsPrepared && !PrepareRayReconstruction(pContext, RenderTargets, Profiler)) return false;
    RTXPTGpuProfiler::Scope Timing{Profiler, pContext, RTXPTGpuProfiler::Stage::RR};
    return m_RayReconstructionPass.Execute(pContext, RenderTargets, FrameConstants, ResetHistory);
}

bool RTXPTPostProcessPipeline::CopyRealtimeOutputToProcessed(IDeviceContext*           pContext,
                                                             const RTXPTRenderTargets& RenderTargets)
{
    m_Stats.LastTemporalAAActive      = false;
    m_Stats.LastSuperResolutionActive = false;

    const auto FailCopy = [this](const char* Reason) {
        m_Stats.LastRealtimeCopyExecuted = false;
        DEV_ERROR("RTXPT realtime OutputColor copy failed: ", Reason);
        return false;
    };

    if (!m_RealtimeCopyContext)
        return FailCopy("realtime copy context is not initialized");
    if (!m_Device || pContext == nullptr)
        return FailCopy("copy requires a device and context");
    if (RenderTargets.GetOutputColorSRV() == nullptr || RenderTargets.GetProcessedOutputColorRTV() == nullptr)
        return FailCopy("copy requires OutputColor SRV and ProcessedOutputColor RTV");
    if (RenderTargets.GetRenderWidth() != RenderTargets.GetDisplayWidth() ||
        RenderTargets.GetRenderHeight() != RenderTargets.GetDisplayHeight())
        return FailCopy("copy requires render and display dimensions to match");

    PostFXContext::TextureOperationAttribs CopyAttribs;
    CopyAttribs.pDevice        = m_Device;
    CopyAttribs.pDeviceContext = pContext;
    m_RealtimeCopyContext->CopyTextureColor(CopyAttribs,
                                            RenderTargets.GetOutputColorSRV(),
                                            RenderTargets.GetProcessedOutputColorRTV());
    m_Stats.LastRealtimeCopyExecuted = true;
    return true;
}

bool RTXPTPostProcessPipeline::RunTemporalAA(IDeviceContext* pContext, const RTXPTTemporalAAFrameAttribs& FrameAttribs)
{
    m_Stats.LastRealtimeCopyExecuted  = false;
    m_Stats.LastSuperResolutionActive = false;

    RTXPTTemporalAAFrameAttribs Attribs = FrameAttribs;
    Attribs.pDevice                     = m_Device;
    Attribs.pDeviceContext              = pContext;

    const bool Executed          = m_TemporalAAPass.Execute(Attribs);
    m_Stats.TemporalAAStageReady = m_TemporalAAPass.IsReady();
    m_Stats.LastTemporalAAActive = Executed;
    if (!Executed)
        DEV_ERROR("RTXPT temporal AA failed: ", m_TemporalAAPass.GetStats().DisabledReason.c_str());
    return Executed;
}

bool RTXPTPostProcessPipeline::RunRealtimeSuperResolution(IDeviceContext*                      pContext,
                                                          const RTXPTRenderTargets&            RenderTargets,
                                                          const RTXPTSuperResolutionFrameDesc& FrameDesc,
                                                          float                                CameraNear,
                                                          float                                CameraFar,
                                                          float                                CameraFovAngleVert)
{
    m_Stats.LastRealtimeCopyExecuted = false;
    m_Stats.LastTemporalAAActive     = false;

    RTXPTSuperResolutionExecuteAttribs ExecuteAttribs;
    ExecuteAttribs.pRenderTargets     = &RenderTargets;
    ExecuteAttribs.pFrameDesc         = &FrameDesc;
    ExecuteAttribs.CameraNear         = CameraNear;
    ExecuteAttribs.CameraFar          = CameraFar;
    ExecuteAttribs.CameraFovAngleVert = CameraFovAngleVert;
    ExecuteAttribs.pColorSRV          = RenderTargets.GetOutputColorSRV();
    ExecuteAttribs.pOutputUAV         = RenderTargets.GetProcessedOutputColorUAV();

    const bool  Executed              = m_SuperResolutionPass.Execute(pContext, ExecuteAttribs);
    const auto& SRStats               = m_SuperResolutionPass.GetStats();
    m_Stats.SuperResolutionStageReady = !FrameDesc.Enabled || (Executed && SRStats.UpscalerReady);
    m_Stats.LastSuperResolutionActive = Executed && SRStats.LastExecute && FrameDesc.Enabled;
    if (!Executed && SRStats.DisabledReason.empty())
        DEV_ERROR("RTXPT realtime super-resolution pass failed");
    return Executed;
}

bool RTXPTPostProcessPipeline::RunPreToneMapping(IDeviceContext*             pContext,
                                                 const RTXPTRenderTargets&   RenderTargets,
                                                 const RTXPTBloomParameters& BloomParams)
{
    const bool BloomEnabled =
        BloomParams.Enabled &&
        BloomParams.Intensity > 0.0f &&
        BloomParams.Radius > 0.0f;
    if (BloomEnabled && !m_BloomPass.ResizeResources(m_Device, RenderTargets.GetDisplayWidth(), RenderTargets.GetDisplayHeight(), RenderTargets.GetProcessedOutputColorFormat()))
    {
        m_Stats.BloomStageReady = m_BloomPass.IsReady();
        DEV_ERROR("RTXPT bloom pass failed to resize resources");
        return false;
    }

    RTXPTBloomRenderAttribs BloomAttribs;
    BloomAttribs.pSourceSRV = RenderTargets.GetProcessedOutputColorSRV();
    BloomAttribs.pTargetRTV = RenderTargets.GetProcessedOutputColorRTV();
    BloomAttribs.Width      = RenderTargets.GetDisplayWidth();
    BloomAttribs.Height     = RenderTargets.GetDisplayHeight();
    BloomAttribs.Format     = RenderTargets.GetProcessedOutputColorFormat();
    BloomAttribs.Params     = BloomParams;

    const bool BloomExecuted = m_BloomPass.Render(pContext, BloomAttribs);
    m_Stats.BloomStageReady  = m_BloomPass.IsReady();
    if (!BloomExecuted)
    {
        DEV_ERROR("RTXPT bloom pass failed to render");
        return false;
    }

    return true;
}

bool RTXPTPostProcessPipeline::RunToneMapping(IDeviceContext*                   pContext,
                                              const RTXPTRenderTargets&         RenderTargets,
                                              const RTXPTToneMappingParameters& Params,
                                              bool                              Enabled)
{
    const auto* Target = RenderTargets.GetLdrColorRTV();
    if (!Target || Target->GetDesc().Format != m_ToneMappingRTVFormat)
    {
        DEV_ERROR("RTXPT tone mapping target does not match the initialized RTV format");
        return false;
    }
    if (!m_ToneMappingPass.ResizeResources(m_Device, RenderTargets.GetDisplayWidth(), RenderTargets.GetDisplayHeight(), RenderTargets.GetProcessedOutputColorFormat()))
    {
        m_Stats.ToneMappingStageReady = m_ToneMappingPass.IsReady();
        DEV_ERROR("RTXPT tone mapping pass failed to resize resources");
        return false;
    }

    RTXPTToneMappingRenderAttribs Attribs;
    Attribs.pSourceSRV = RenderTargets.GetProcessedOutputColorSRV();
    Attribs.pLdrRTV    = RenderTargets.GetLdrColorRTV();
    Attribs.Width      = RenderTargets.GetDisplayWidth();
    Attribs.Height     = RenderTargets.GetDisplayHeight();
    Attribs.Enabled    = Enabled;
    Attribs.pParams    = &Params;

    const bool Executed           = m_ToneMappingPass.Render(pContext, Attribs);
    m_Stats.ToneMappingStageReady = m_ToneMappingPass.IsReady();
    if (!Executed)
        DEV_ERROR("RTXPT tone mapping pass failed to render");
    return Executed;
}

float RTXPTPostProcessPipeline::ComputePreExposedGrayLuminance(const RTXPTToneMappingParameters& Params, bool Enabled) const
{
    return m_ToneMappingPass.ComputePreExposedGrayLuminance(Params, Enabled);
}

} // namespace Diligent
