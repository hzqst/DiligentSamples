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

#include "RTXPTLightsBakerPass.hpp"

#include "DebugUtilities.hpp"
#include "GraphicsTypesX.hpp"
#include "RenderStateCache.h"

#include "AsyncPrefix.hpp"

namespace Diligent
{

void RTXPTLightsBakerPass::Reset()
{
    m_PSO.Release();
    m_SRB.Release();
}

bool RTXPTLightsBakerPass::Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache, const char* Name, const char* EntryPoint)
{
    Reset();
    if (pDevice == nullptr || pEngineFactory == nullptr || EntryPoint == nullptr)
        return false;

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    pEngineFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders\\PathTracer", &pShaderSourceFactory);

    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.Desc.Name                  = Name != nullptr ? Name : "RTXPT LightsBaker pass";
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = "PathTracer/Lighting/LightsBaker.hlsl";
    ShaderCI.EntryPoint                 = EntryPoint;
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    RefCntAutoPtr<IShader> pCS;
    pStateCache->CreateShader(ShaderCI, &pCS);
    VERIFY(pCS, "Failed to create RTXPT LightsBaker shader");
    if (!pCS)
        return false;

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = ShaderCI.Desc.Name;
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.PSODesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    PSOCreateInfo.pCS                  = pCS;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout
        .AddVariable(SHADER_TYPE_COMPUTE, "t_LightingControl", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_LightProxyCounters", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_LightSamplingProxies", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_FeedbackTotalWeight", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_FeedbackCandidates", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_FeedbackTotalWeight", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_FeedbackCandidates", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_LocalSamplingBuffer", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    pStateCache->CreateComputePipelineState(PSOCreateInfo, &m_PSO);
    VERIFY(m_PSO, "Failed to create RTXPT LightsBaker PSO");
    if (!m_PSO)
        return false;

    m_PSO->CreateShaderResourceBinding(&m_SRB, true);
    VERIFY(m_SRB, "Failed to create RTXPT LightsBaker SRB");
    return m_SRB != nullptr;
}

bool RTXPTLightsBakerPass::Bind(IBuffer* pControl, IBuffer* pProxyCounters, IBuffer* pProxyIndices, IBuffer* pLocalSamplingBuffer, ITextureView* pFeedbackTotalWeightSRV, ITextureView* pFeedbackCandidatesSRV, ITextureView* pFeedbackTotalWeightUAV, ITextureView* pFeedbackCandidatesUAV)
{
    if (!m_SRB || pControl == nullptr)
        return false;

    auto SetVariable = [this](const char* Name, IDeviceObject* pObject) {
        IShaderResourceVariable* pVar = m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name);
        if (pVar == nullptr)
            return true;
        if (pObject == nullptr)
            return false;
        pVar->Set(pObject);
        return true;
    };

    return SetVariable("t_LightingControl", pControl->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetVariable("t_LightProxyCounters", pProxyCounters != nullptr ? pProxyCounters->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE) : nullptr) &&
        SetVariable("t_LightSamplingProxies", pProxyIndices != nullptr ? pProxyIndices->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE) : nullptr) &&
        SetVariable("t_FeedbackTotalWeight", pFeedbackTotalWeightSRV) &&
        SetVariable("t_FeedbackCandidates", pFeedbackCandidatesSRV) &&
        SetVariable("u_FeedbackTotalWeight", pFeedbackTotalWeightUAV) &&
        SetVariable("u_FeedbackCandidates", pFeedbackCandidatesUAV) &&
        SetVariable("u_LocalSamplingBuffer", pLocalSamplingBuffer != nullptr ? pLocalSamplingBuffer->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS) : nullptr);
}

bool RTXPTLightsBakerPass::Dispatch(IDeviceContext* pContext, Uint32 ThreadGroupsX, Uint32 ThreadGroupsY)
{
    if (!m_PSO || !m_SRB || pContext == nullptr || ThreadGroupsX == 0 || ThreadGroupsY == 0)
        return false;

    pContext->SetPipelineState(m_PSO);
    pContext->CommitShaderResources(m_SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DispatchComputeAttribs DispatchAttribs;
    DispatchAttribs.ThreadGroupCountX = ThreadGroupsX;
    DispatchAttribs.ThreadGroupCountY = ThreadGroupsY;
    DispatchAttribs.ThreadGroupCountZ = 1;
    pContext->DispatchCompute(DispatchAttribs);
    return true;
}

} // namespace Diligent
