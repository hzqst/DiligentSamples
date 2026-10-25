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

#include "RTXPTLightProxyBuildPass.hpp"

#include "DebugUtilities.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"
#include <algorithm>

namespace Diligent
{

namespace
{

struct LightProxyBuildConstantsCPU
{
    Uint32 TotalLightCount        = 0;
    Uint32 AnalyticLightCount     = 0;
    Uint32 ProxyBudget            = 0;
    Uint32 ImportanceSamplingType = 1;
    Uint32 HistoricWeightCount    = 0;
    Uint32 HasHistoryRemap        = 0;
    Uint32 HasFrustumPlanes       = 0;
    Uint32 Padding                = 0;
    float4 FrustumPlanes[5]       = {};
    float3 CameraPosition         = {};
    Uint32 HasCameraPosition      = 0;
};
static_assert(sizeof(LightProxyBuildConstantsCPU) == 128, "LightProxyBuildConstants must match LightProxyBuild.hlsl");

constexpr Uint32 kProxyBuildThreads = 256u;

void InsertBufferUAVBarrier(IDeviceContext* pContext, IBuffer* pBuffer)
{
    if (pContext == nullptr || pBuffer == nullptr)
        return;

    StateTransitionDesc Barrier{pBuffer,
                                RESOURCE_STATE_UNORDERED_ACCESS,
                                RESOURCE_STATE_UNORDERED_ACCESS,
                                STATE_TRANSITION_FLAG_UPDATE_STATE};
    pContext->TransitionResourceState(Barrier);
}

} // namespace

void RTXPTLightProxyBuildPass::Reset()
{
    m_ResetPSO.Release();
    m_WeightsPSO.Release();
    m_ClustersPSO.Release();
    m_CountsPSO.Release();
    m_ScatterPSO.Release();
    m_FinalizePSO.Release();
    m_ResetSRB.Release();
    m_WeightsSRB.Release();
    m_ClustersSRB.Release();
    m_CountsSRB.Release();
    m_ScatterSRB.Release();
    m_FinalizeSRB.Release();
    m_Constants.Release();
    m_ProxyJobs.Release();
    m_ProxyDispatchArgs.Release();
    m_HistoricLightWeights.Release();
    m_HistoryRemap        = nullptr;
    m_HistoricWeightCount = 0;
    m_Device.Release();
    m_Ready = false;
}

bool RTXPTLightProxyBuildPass::CreatePSO(IRenderStateCache*                     pStateCache,
                                         IShader*                               pCS,
                                         const char*                            Name,
                                         RefCntAutoPtr<IPipelineState>&         PSO,
                                         RefCntAutoPtr<IShaderResourceBinding>& SRB)
{
    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = Name;
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.pCS                  = pCS;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    ResourceLayout
        .AddVariable(SHADER_TYPE_COMPUTE, "g_LightProxyBuildConstants", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_LightingControl", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_Lights", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_LightsEx", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_LightWeights", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_LightProxyCounters", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_LightSamplingProxies", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    if (pStateCache) pStateCache->CreateComputePipelineState(PSOCreateInfo, &PSO);
    else
        m_Device->CreateComputePipelineState(PSOCreateInfo, &PSO);
    if (!PSO)
    {
        UNEXPECTED("Failed to create RTXPT light proxy build PSO: ", Name);
        return false;
    }

    PSO->CreateShaderResourceBinding(&SRB, true);
    if (!SRB)
    {
        UNEXPECTED("Failed to create RTXPT light proxy build SRB: ", Name);
        return false;
    }
    return true;
}

bool RTXPTLightProxyBuildPass::Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache)
{
    Reset();
    if (pDevice == nullptr || pEngineFactory == nullptr)
        return false;
    m_Device = pDevice;

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    pEngineFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders\\PathTracer", &pShaderSourceFactory);

    auto CreateCS = [&](const char* EntryPoint, const char* Name) -> RefCntAutoPtr<IShader> {
        ShaderCreateInfo ShaderCI;
        ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
        ShaderCI.Desc.Name                  = Name;
        ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
        ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
        ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
        ShaderCI.FilePath                   = "PathTracer/Lighting/LightProxyBuild.hlsl";
        ShaderCI.EntryPoint                 = EntryPoint;
        ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

        RefCntAutoPtr<IShader> pCS;
        if (pStateCache) pStateCache->CreateShader(ShaderCI, &pCS);
        else
            pDevice->CreateShader(ShaderCI, &pCS);
        return pCS;
    };

    RefCntAutoPtr<IShader> pResetCS    = CreateCS("ResetProxyBuildCS", "RTXPT light proxy reset");
    RefCntAutoPtr<IShader> pWeightsCS  = CreateCS("ComputeWeightsCS", "RTXPT light proxy weights");
    RefCntAutoPtr<IShader> pClustersCS = CreateCS("ProcessClustersCS", "RTXPT light clusters");
    RefCntAutoPtr<IShader> pCountsCS   = CreateCS("ComputeProxyCountsCS", "RTXPT light proxy counts");
    RefCntAutoPtr<IShader> pScatterCS  = CreateCS("ScatterProxiesCS", "RTXPT light proxy scatter");
    RefCntAutoPtr<IShader> pFinalizeCS = CreateCS("FinalizeProxyJobsCS", "RTXPT light proxy jobs");
    if (!pResetCS || !pWeightsCS || !pClustersCS || !pCountsCS || !pScatterCS || !pFinalizeCS)
    {
        UNEXPECTED("Failed to create RTXPT light proxy build shaders");
        return false;
    }

    BufferDesc ConstantsDesc;
    ConstantsDesc.Name           = "RTXPT light proxy build constants";
    ConstantsDesc.Size           = sizeof(LightProxyBuildConstantsCPU);
    ConstantsDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage          = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_Constants);
    if (!m_Constants)
    {
        UNEXPECTED("Failed to create RTXPT light proxy build constants buffer");
        return false;
    }

    const bool Ok =
        CreatePSO(pStateCache, pResetCS, "RTXPT light proxy reset PSO", m_ResetPSO, m_ResetSRB) &&
        CreatePSO(pStateCache, pWeightsCS, "RTXPT light proxy weights PSO", m_WeightsPSO, m_WeightsSRB) &&
        CreatePSO(pStateCache, pClustersCS, "RTXPT light clusters PSO", m_ClustersPSO, m_ClustersSRB) &&
        CreatePSO(pStateCache, pCountsCS, "RTXPT light proxy counts PSO", m_CountsPSO, m_CountsSRB) &&
        CreatePSO(pStateCache, pFinalizeCS, "RTXPT light proxy jobs PSO", m_FinalizePSO, m_FinalizeSRB) &&
        CreatePSO(pStateCache, pScatterCS, "RTXPT light proxy scatter PSO", m_ScatterPSO, m_ScatterSRB);

    m_Ready = Ok;
    return Ok;
}

bool RTXPTLightProxyBuildPass::BindResources(IShaderResourceBinding* pSRB,
                                             IBuffer*                pControl,
                                             IBuffer*                pLights,
                                             IBuffer*                pLightsEx,
                                             IBuffer*                pFeedbackCounts,
                                             IBuffer*                pLightWeights,
                                             IBuffer*                pLightProxyCounters,
                                             IBuffer*                pLightSamplingProxies)
{
    if (pSRB == nullptr)
        return false;

    // Tolerant binding: each entry point uses a subset of the resources, so a missing variable is expected.
    auto SetVar = [&](const char* Name, IDeviceObject* pObject) {
        IShaderResourceVariable* pVar = pSRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name);
        if (pVar == nullptr)
            return true;
        if (pObject == nullptr)
            return false;
        pVar->Set(pObject);
        return true;
    };

    return SetVar("g_LightProxyBuildConstants", m_Constants) &&
        (pSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Lights") == nullptr || SetVar("u_Lights", pLights->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS))) &&
        (pSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_LightsEx") == nullptr || SetVar("u_LightsEx", pLightsEx->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS))) &&
        SetVar("u_LightingControl", pControl->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS)) &&
        SetVar("t_Lights", pLights->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetVar("t_LightsEx", pLightsEx->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetVar("u_ProxyJobs", m_ProxyJobs->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS)) &&
        SetVar("u_ProxyDispatchArgs", m_ProxyDispatchArgs->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS)) &&
        SetVar("t_FeedbackCounts", pFeedbackCounts->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetVar("t_HistoryRemapCurrentToPast", (m_HistoryRemap ? m_HistoryRemap : pFeedbackCounts)->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetVar("t_HistoricLightWeights", m_HistoricLightWeights->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetVar("u_LightWeights", pLightWeights->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS)) &&
        SetVar("u_LightProxyCounters", pLightProxyCounters->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS)) &&
        SetVar("u_LightSamplingProxies", pLightSamplingProxies->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
}

bool RTXPTLightProxyBuildPass::Build(IDeviceContext* pContext,
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
                                     IBuffer*        pHistoryRemapCurrentToPast,
                                     const float4*   pFrustumPlanes,
                                     Uint32          ClusterCount,
                                     const float3*   pCameraPosition)
{
    if (!m_Ready || pContext == nullptr || TotalLightCount == 0u)
        return false;
    if (pControl == nullptr || pLights == nullptr || pLightsEx == nullptr ||
        pLightWeights == nullptr || pLightProxyCounters == nullptr || pLightSamplingProxies == nullptr)
        return false;

    m_HistoryRemap = pHistoryRemapCurrentToPast;
    if (!m_HistoricLightWeights)
    {
        BufferDesc Desc;
        Desc.Name              = "RTXPT historic light weights";
        Desc.Size              = sizeof(float);
        Desc.Mode              = BUFFER_MODE_STRUCTURED;
        Desc.ElementByteStride = sizeof(float);
        Desc.BindFlags         = BIND_SHADER_RESOURCE;
        m_Device->CreateBuffer(Desc, nullptr, &m_HistoricLightWeights);
        if (!m_HistoricLightWeights) return false;
    }
    {
        MapHelper<LightProxyBuildConstantsCPU> Mapped{pContext, m_Constants, MAP_WRITE, MAP_FLAG_DISCARD};
        if (Mapped == nullptr)
            return false;
        Mapped->TotalLightCount        = TotalLightCount;
        Mapped->AnalyticLightCount     = AnalyticLightCount;
        Mapped->ProxyBudget            = ProxyBudget;
        Mapped->ImportanceSamplingType = ImportanceSamplingType;
        Mapped->HistoricWeightCount    = m_HistoricWeightCount;
        Mapped->HasHistoryRemap        = m_HistoryRemap ? 1u : 0u;
        Mapped->HasFrustumPlanes       = pFrustumPlanes ? 1u : 0u;
        Mapped->HasCameraPosition      = pCameraPosition ? 1u : 0u;
        Mapped->CameraPosition         = pCameraPosition ? *pCameraPosition : float3{};
        if (pFrustumPlanes) std::copy(pFrustumPlanes, pFrustumPlanes + 5, Mapped->FrustumPlanes);
    }

    const Uint64 JobsSize = (Uint64{TotalLightCount} + (Uint64{ProxyBudget} + 255u) / 256u) * 4u * sizeof(Uint32);
    if (!m_ProxyJobs || m_ProxyJobs->GetDesc().Size < JobsSize)
    {
        m_ProxyJobs.Release();
        BufferDesc Desc;
        Desc.Name              = "RTXPT light proxy jobs";
        Desc.Size              = std::max(JobsSize, Uint64{16});
        Desc.Mode              = BUFFER_MODE_STRUCTURED;
        Desc.ElementByteStride = 4 * sizeof(Uint32);
        Desc.BindFlags         = BIND_UNORDERED_ACCESS;
        m_Device->CreateBuffer(Desc, nullptr, &m_ProxyJobs);
    }
    if (!m_ProxyDispatchArgs)
    {
        BufferDesc Desc;
        Desc.Name              = "RTXPT light proxy indirect dispatch";
        Desc.Size              = 3 * sizeof(Uint32);
        Desc.Mode              = BUFFER_MODE_STRUCTURED;
        Desc.ElementByteStride = sizeof(Uint32);
        Desc.BindFlags         = BIND_UNORDERED_ACCESS | BIND_INDIRECT_DRAW_ARGS;
        m_Device->CreateBuffer(Desc, nullptr, &m_ProxyDispatchArgs);
    }
    if (!m_ProxyJobs || !m_ProxyDispatchArgs) return false;

    if (!BindResources(m_FinalizeSRB, pControl, pLights, pLightsEx, pFeedbackCounts, pLightWeights, pLightProxyCounters, pLightSamplingProxies) ||
        !BindResources(m_ResetSRB, pControl, pLights, pLightsEx, pFeedbackCounts, pLightWeights, pLightProxyCounters, pLightSamplingProxies) ||
        !BindResources(m_WeightsSRB, pControl, pLights, pLightsEx, pFeedbackCounts, pLightWeights, pLightProxyCounters, pLightSamplingProxies) ||
        !BindResources(m_CountsSRB, pControl, pLights, pLightsEx, pFeedbackCounts, pLightWeights, pLightProxyCounters, pLightSamplingProxies) ||
        !BindResources(m_ScatterSRB, pControl, pLights, pLightsEx, pFeedbackCounts, pLightWeights, pLightProxyCounters, pLightSamplingProxies))
    {
        UNEXPECTED("Failed to bind RTXPT light proxy build resources");
        return false;
    }

    const Uint32 LightGroups = (TotalLightCount + kProxyBuildThreads - 1u) / kProxyBuildThreads;

    DispatchComputeAttribs SingleGroup;
    SingleGroup.ThreadGroupCountX = 1;
    SingleGroup.ThreadGroupCountY = 1;
    SingleGroup.ThreadGroupCountZ = 1;

    DispatchComputeAttribs LightDispatch;
    LightDispatch.ThreadGroupCountX = LightGroups;
    LightDispatch.ThreadGroupCountY = 1;
    LightDispatch.ThreadGroupCountZ = 1;

    // Reset the GPU-computed control scalars (weight sum, proxy count, scatter cursor).
    pContext->SetPipelineState(m_ResetPSO);
    pContext->CommitShaderResources(m_ResetSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->DispatchCompute(SingleGroup);
    InsertBufferUAVBarrier(pContext, pControl);
    InsertBufferUAVBarrier(pContext, m_ProxyDispatchArgs);

    // Per-light weights + total weight sum (single group: each thread reduces a contiguous block of lights).
    pContext->SetPipelineState(m_WeightsPSO);
    pContext->CommitShaderResources(m_WeightsSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->DispatchCompute(SingleGroup);
    InsertBufferUAVBarrier(pContext, pControl);
    InsertBufferUAVBarrier(pContext, pLightWeights);

    if (ClusterCount != 0u)
    {
        if (!BindResources(m_ClustersSRB, pControl, pLights, pLightsEx, pFeedbackCounts, pLightWeights, pLightProxyCounters, pLightSamplingProxies)) return false;
        pContext->SetPipelineState(m_ClustersPSO);
        pContext->CommitShaderResources(m_ClustersSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        DispatchComputeAttribs ClustersDispatch;
        ClustersDispatch.ThreadGroupCountX = (ClusterCount + kProxyBuildThreads - 1u) / kProxyBuildThreads;
        pContext->DispatchCompute(ClustersDispatch);
        InsertBufferUAVBarrier(pContext, pLights);
        InsertBufferUAVBarrier(pContext, pLightsEx);
        InsertBufferUAVBarrier(pContext, pLightWeights);
    }

    // Per-light proxy counts (proportional to weight) + atomic total proxy count.
    pContext->SetPipelineState(m_CountsPSO);
    pContext->CommitShaderResources(m_CountsSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->DispatchCompute(LightDispatch);
    InsertBufferUAVBarrier(pContext, pControl);
    InsertBufferUAVBarrier(pContext, pLightProxyCounters);
    InsertBufferUAVBarrier(pContext, m_ProxyJobs);

    pContext->SetPipelineState(m_FinalizePSO);
    pContext->CommitShaderResources(m_FinalizeSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->DispatchCompute(SingleGroup);

    // Scatter each light's index into its claimed proxy-table range.
    pContext->SetPipelineState(m_ScatterPSO);
    pContext->CommitShaderResources(m_ScatterSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DispatchComputeIndirectAttribs Indirect;
    Indirect.pAttribsBuffer                   = m_ProxyDispatchArgs;
    Indirect.AttribsBufferStateTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    pContext->DispatchComputeIndirect(Indirect);
    InsertBufferUAVBarrier(pContext, pLightSamplingProxies);

    // Preserve the boosted weight (not the temporally mixed proxy weight), matching
    // upstream's ping-pong weights. Copy after all reads of the preceding frame.
    const Uint64 WeightBytes = Uint64{TotalLightCount} * sizeof(float);
    if (m_HistoricLightWeights->GetDesc().Size < WeightBytes)
    {
        m_HistoricLightWeights.Release();
        BufferDesc Desc;
        Desc.Name              = "RTXPT historic light weights";
        Desc.Size              = WeightBytes;
        Desc.Mode              = BUFFER_MODE_STRUCTURED;
        Desc.ElementByteStride = sizeof(float);
        Desc.BindFlags         = BIND_SHADER_RESOURCE;
        m_Device->CreateBuffer(Desc, nullptr, &m_HistoricLightWeights);
        if (!m_HistoricLightWeights) return false;
    }
    pContext->CopyBuffer(pLightWeights, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                         m_HistoricLightWeights, 0, WeightBytes, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    m_HistoricWeightCount = TotalLightCount;

    return true;
}

} // namespace Diligent
