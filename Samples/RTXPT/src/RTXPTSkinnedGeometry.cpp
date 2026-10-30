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

#include "RTXPTSkinnedGeometry.hpp"
#include "RTXPTShaderSource.hpp"
#include "RTXPTPipelineStateCache.hpp"

#include <cstring>

#include "DebugUtilities.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"

#include "AsyncPrefix.hpp"

namespace Diligent
{

void RTXPTSkinningPass::Reset()
{
    m_PSO.Release();
    m_SRB.Release();
    m_Constants.Release();
    m_Bound         = {};
    m_DispatchCount = 0;
}

bool RTXPTSkinningPass::Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache, IPipelineStateCache* pPSOCache)
{
    Reset();
    if (pDevice == nullptr || pEngineFactory == nullptr || pStateCache == nullptr)
        return false;

    BufferDesc ConstantsDesc;
    ConstantsDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    ConstantsDesc.Name                 = "RTXPT skinning constants";
    ConstantsDesc.Usage                = USAGE_DYNAMIC;
    ConstantsDesc.BindFlags            = BIND_UNIFORM_BUFFER;
    ConstantsDesc.CPUAccessFlags       = CPU_ACCESS_WRITE;
    ConstantsDesc.Size                 = sizeof(Constants);
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_Constants);
    if (!m_Constants)
        return false;

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    CreateRTXPTShaderSourceFactory(pEngineFactory, "shaders;shaders\\PathTracer", &pShaderSourceFactory);

    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.Desc.Name                  = "RTXPT skinned vertex build";
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = "PathTracer/SkinnedVertexBuild.csh";
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    RefCntAutoPtr<IShader> pCS;
    pStateCache->CreateShader(ShaderCI, &pCS);
    if (!pCS)
        return false;

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name                 = "RTXPT skinned vertex build PSO";
    PSOCreateInfo.PSODesc.PipelineType         = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.PSODesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    PSOCreateInfo.pCS                          = pCS;

    // The buffers are mutable: Bind creates a new resource binding when they change. None of them is USAGE_DYNAMIC.
    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout.AddVariable(SHADER_TYPE_COMPUTE, "cbSkinningConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC);
    for (const char* Name : {"t_SourceVertices", "t_SourceSkinData", "t_JointMatrices", "u_SkinnedVertices"})
        ResourceLayout.AddVariable(SHADER_TYPE_COMPUTE, Name, SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE, SHADER_VARIABLE_FLAG_NO_DYNAMIC_BUFFERS);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;
    PSOCreateInfo.pPSOCache              = SelectRTXPTPipelineStateCache(pDevice, PSOCreateInfo, pPSOCache);

    pStateCache->CreateComputePipelineState(PSOCreateInfo, &m_PSO);
    if (!m_PSO)
        return false;
    IShaderResourceVariable* pConstantsVar = m_PSO->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "cbSkinningConstants");
    if (pConstantsVar == nullptr)
    {
        m_PSO.Release();
        return false;
    }
    pConstantsVar->Set(m_Constants, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    return true;
}

bool RTXPTSkinningPass::Bind(const RTXPTSkinningBindings& Bindings)
{
    if (!IsReady() || Bindings.pSourceVertices == nullptr || Bindings.pSkinData == nullptr || Bindings.pJointMatrices == nullptr ||
        Bindings.pSkinnedVertices == nullptr)
        return false;
    if (m_SRB && std::memcmp(&Bindings, &m_Bound, sizeof(Bindings)) == 0)
        return true;

    m_SRB.Release();
    RefCntAutoPtr<IShaderResourceBinding> pSRB;
    m_PSO->CreateShaderResourceBinding(&pSRB, true);
    if (!pSRB)
        return false;
    const std::pair<const char*, IDeviceObject*> Resources[] = {
        {"t_SourceVertices", Bindings.pSourceVertices->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)},
        {"t_SourceSkinData", Bindings.pSkinData->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)},
        {"t_JointMatrices", Bindings.pJointMatrices->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)},
        {"u_SkinnedVertices", Bindings.pSkinnedVertices->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS)},
    };
    for (const auto& Resource : Resources)
    {
        IShaderResourceVariable* pVar = pSRB->GetVariableByName(SHADER_TYPE_COMPUTE, Resource.first);
        if (pVar == nullptr || Resource.second == nullptr)
            return false;
        pVar->Set(Resource.second);
    }
    m_SRB   = std::move(pSRB);
    m_Bound = Bindings;
    return true;
}

bool RTXPTSkinningPass::Dispatch(IDeviceContext* pContext, const RTXPTSkinningDispatch& Range)
{
    if (!m_SRB || pContext == nullptr || Range.VertexCount == 0)
        return false;
    {
        MapHelper<Constants> Mapped{pContext, m_Constants, MAP_WRITE, MAP_FLAG_DISCARD};
        if (!Mapped)
            return false;
        Mapped->Range = Range;
    }
    pContext->SetPipelineState(m_PSO);
    pContext->CommitShaderResources(m_SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DispatchComputeAttribs Attribs;
    Attribs.ThreadGroupCountX = (Range.VertexCount + 127u) / 128u;
    pContext->DispatchCompute(Attribs);
    ++m_DispatchCount;
    return true;
}

} // namespace Diligent
