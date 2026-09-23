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

#include "Buffer.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "BasicMath.hpp"

namespace Diligent
{

struct IRenderStateCache;

// Drives the GPU per-triangle, power-proportional light sampling proxy build (shaders/PathTracer/Lighting/
// LightProxyBuild.hlsl). Replaces the CPU "emissive bucket" proxy build with RTXPT-fork's per-light model:
// every analytic light and every emissive triangle is weighted by its power and gets sampling proxies in
// proportion. Owns the compute PSOs/SRBs, constants and indirect jobs; the per-light/proxy storage and
// the control buffer are owned by RTXPTLightsBaker and passed into Build().
class RTXPTLightProxyBuildPass
{
public:
    void Reset();

    bool Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache);

    bool IsReady() const { return m_Ready; }

    // Runs Reset -> ComputeWeights -> ComputeProxyCounts -> ScatterProxies with UAV barriers between passes.
    // pControl must be a DEFAULT (UAV) buffer; the pass writes WeightsSumUINT/SamplingProxyCount/ProxyBuildTaskCount.
    bool Build(IDeviceContext* pContext,
               IBuffer*        pControl,
               IBuffer*        pLights,
               IBuffer*        pLightsEx,
               IBuffer*        pFeedbackCounts,
               IBuffer*        pLightWeights,
               IBuffer*        pLightProxyCounters,
               IBuffer*        pLightSamplingProxies,
               Uint32          TotalLightCount,
               Uint32          AnalyticLightCount,
               Uint32          ProxyBudget,
               Uint32          ImportanceSamplingType,
               IBuffer*        pHistoryRemapCurrentToPast = nullptr,
               const float4*   pFrustumPlanes             = nullptr);

private:
    bool CreatePSO(IRenderStateCache* pStateCache, IShader* pCS, const char* Name, RefCntAutoPtr<IPipelineState>& PSO, RefCntAutoPtr<IShaderResourceBinding>& SRB);
    bool BindResources(IShaderResourceBinding* pSRB,
                       IBuffer*                pControl,
                       IBuffer*                pLights,
                       IBuffer*                pLightsEx,
                       IBuffer*                pFeedbackCounts,
                       IBuffer*                pLightWeights,
                       IBuffer*                pLightProxyCounters,
                       IBuffer*                pLightSamplingProxies);

    RefCntAutoPtr<IPipelineState>         m_ResetPSO;
    RefCntAutoPtr<IPipelineState>         m_WeightsPSO;
    RefCntAutoPtr<IPipelineState>         m_CountsPSO;
    RefCntAutoPtr<IPipelineState>         m_ScatterPSO;
    RefCntAutoPtr<IPipelineState>         m_FinalizePSO;
    RefCntAutoPtr<IShaderResourceBinding> m_ResetSRB;
    RefCntAutoPtr<IShaderResourceBinding> m_WeightsSRB;
    RefCntAutoPtr<IShaderResourceBinding> m_CountsSRB;
    RefCntAutoPtr<IShaderResourceBinding> m_ScatterSRB;
    RefCntAutoPtr<IShaderResourceBinding> m_FinalizeSRB;
    RefCntAutoPtr<IBuffer>                m_Constants;
    RefCntAutoPtr<IBuffer>                m_ProxyJobs;
    RefCntAutoPtr<IBuffer>                m_ProxyDispatchArgs;
    RefCntAutoPtr<IBuffer>                m_HistoricLightWeights;
    IBuffer*                              m_HistoryRemap        = nullptr;
    Uint32                                m_HistoricWeightCount = 0;
    RefCntAutoPtr<IRenderDevice>          m_Device;
    bool                                  m_Ready = false;
};

} // namespace Diligent
