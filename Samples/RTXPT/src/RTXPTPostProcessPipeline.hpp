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

#pragma once

#include <memory>

#include "DeviceContext.h"
#include "EngineFactory.h"
#include "PostFXContext.hpp"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "RTXPTAccumulationPass.hpp"
#include "RTXPTBloomPass.hpp"
#include "RTXPTPostProcessPass.hpp"
#include "RTXPTRenderTargets.hpp"
#include "RTXPTSuperResolutionPass.hpp"
#include "RTXPTRayReconstructionPass.hpp"
#include "RTXPTTemporalAAPass.hpp"
#include "RTXPTToneMappingPass.hpp"
#include "SwapChain.h"

namespace Diligent
{

struct IRenderStateCache;
struct IPipelineStateCache;
class RTXPTGpuProfiler;

struct RTXPTPostProcessPipelineStats
{
    bool Ready                       = false;
    bool ResourcesValid              = false;
    bool AccumulationStageReady      = false;
    bool SuperResolutionStageReady   = false;
    bool LastSuperResolutionActive   = false;
    bool TemporalAAStageReady        = false;
    bool LastTemporalAAActive        = false;
    bool LastRealtimeCopyExecuted    = false;
    bool HdrStageReady               = false;
    bool RealtimeMergeStageReady     = false;
    bool LastRealtimeFinalMergeReady = false;
    bool BloomStageReady             = false;
    bool ToneMappingStageReady       = false;
    bool LdrStageReady               = false;
};

class RTXPTPostProcessPipeline
{
public:
    void Reset();

    bool Initialize(IRenderDevice*       pDevice,
                    IEngineFactory*      pEngineFactory,
                    IRenderStateCache*   pStateCache,
                    IPipelineStateCache* pPSOCache,
                    ISwapChain*          pSwapChain,
                    IBuffer*             pFrameConstants,
                    bool                 ComputeSupported);

    bool ValidateRenderTargets(const RTXPTRenderTargets& RenderTargets);

    bool RunAccumulation(IDeviceContext*           pContext,
                         const RTXPTRenderTargets& RenderTargets,
                         Uint32                    SampleIndex,
                         bool                      ResetAccumulation);

    bool RunDenoiserPrepare(IDeviceContext*           pContext,
                            const RTXPTRenderTargets& RenderTargets,
                            RTXPTNrdMethod            Method,
                            Uint32                    PlaneIndex,
                            bool                      InitOutput);

    bool RunDenoiserFinalMerge(IDeviceContext*           pContext,
                               const RTXPTRenderTargets& RenderTargets,
                               RTXPTNrdMethod            Method,
                               Uint32                    PlaneIndex,
                               bool                      HasValidation);

    bool RunNoDenoiserFinalMerge(IDeviceContext*           pContext,
                                 const RTXPTRenderTargets& RenderTargets);

    RTXPTSuperResolutionFrameDesc ResolveSuperResolutionFrameDesc(const RTXPTSuperResolutionSettings& Settings,
                                                                  Uint32                              DisplayWidth,
                                                                  Uint32                              DisplayHeight,
                                                                  TEXTURE_FORMAT                      OutputFormat,
                                                                  bool                                ResetHistory,
                                                                  float                               TimeDeltaSeconds);

    bool RunSuperResolution(IDeviceContext*                      pContext,
                            const RTXPTRenderTargets&            RenderTargets,
                            const RTXPTSuperResolutionFrameDesc& FrameDesc,
                            float                                CameraNear,
                            float                                CameraFar,
                            float                                CameraFovAngleVert);

    float2 GetRealtimeTAAPixelJitter(Uint32 FrameIndex) const;

    bool                        PrepareRayReconstruction(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets, RTXPTGpuProfiler* Profiler);
    bool                        RunRayReconstruction(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets, const SampleConstants& FrameConstants, bool ResetHistory, RTXPTGpuProfiler* Profiler = nullptr, bool InputsPrepared = false);
    RTXPTRayReconstructionPass& GetRayReconstructionPass() { return m_RayReconstructionPass; }

    bool CopyRealtimeOutputToProcessed(IDeviceContext*           pContext,
                                       const RTXPTRenderTargets& RenderTargets);

    bool RunTemporalAA(IDeviceContext*                pContext,
                       const RTXPTRenderTargets&      RenderTargets,
                       const SampleConstants&         FrameConstants,
                       Uint32                         FrameIndex,
                       bool                           ResetHistory,
                       bool                           PreviousViewValid,
                       const RTXPTTemporalAASettings& Settings);

    bool RunRealtimeSuperResolution(IDeviceContext*                      pContext,
                                    const RTXPTRenderTargets&            RenderTargets,
                                    const RTXPTSuperResolutionFrameDesc& FrameDesc,
                                    float                                CameraNear,
                                    float                                CameraFar,
                                    float                                CameraFovAngleVert);

    bool RunPreToneMapping(IDeviceContext*             pContext,
                           const RTXPTRenderTargets&   RenderTargets,
                           const RTXPTBloomParameters& BloomParams);

    bool RunToneMapping(IDeviceContext*                   pContext,
                        const RTXPTRenderTargets&         RenderTargets,
                        const RTXPTToneMappingParameters& Params,
                        bool                              Enabled);

    float ComputePreExposedGrayLuminance(const RTXPTToneMappingParameters& Params, bool Enabled) const;

    bool                                 IsReady() const { return m_Stats.Ready; }
    const RTXPTPostProcessPipelineStats& GetStats() const { return m_Stats; }
    const RTXPTSuperResolutionPass&      GetSuperResolutionPass() const { return m_SuperResolutionPass; }
    const RTXPTTemporalAAPass&           GetTemporalAAPass() const { return m_TemporalAAPass; }

private:
    RTXPTPostProcessPipelineStats  m_Stats;
    RefCntAutoPtr<IRenderDevice>   m_Device;
    std::unique_ptr<PostFXContext> m_RealtimeCopyContext;
    RTXPTAccumulationPass          m_AccumulationPass;
    RTXPTPostProcessPass           m_PostProcessPass;
    RTXPTSuperResolutionPass       m_SuperResolutionPass;
    RTXPTRayReconstructionPass     m_RayReconstructionPass;
    RTXPTTemporalAAPass            m_TemporalAAPass;
    RTXPTBloomPass                 m_BloomPass;
    RTXPTToneMappingPass           m_ToneMappingPass;
};

} // namespace Diligent
