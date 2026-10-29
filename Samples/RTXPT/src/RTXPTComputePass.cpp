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

#include "RTXPTComputePass.hpp"
#include "RTXPTShaderSource.hpp"
#include "DebugUtilities.hpp"
#include "RenderStateCache.h"

#include "GraphicsTypesX.hpp"

namespace Diligent
{

void RTXPTComputePass::Reset()
{
    m_PSO.Release();
    m_SRB.Release();
    m_Stats = {};
    m_Name.clear();
}

bool RTXPTComputePass::Initialize(IRenderDevice* pDevice, const RTXPTComputePassCreateInfo& CreateInfo)
{
    Reset();
    m_Name = CreateInfo.PassName != nullptr ? CreateInfo.PassName : "RTXPT compute pass";

    if (!CreateInfo.ComputeSupported)
    {
        DEV_ERROR(m_Name, " requires compute shader support");
        return false;
    }

    if (pDevice == nullptr || CreateInfo.pEngineFactory == nullptr)
    {
        DEV_ERROR(m_Name, " requires a render device and engine factory");
        return false;
    }

    if (CreateInfo.pFrameConstants == nullptr)
    {
        DEV_ERROR(m_Name, " requires frame constants");
        return false;
    }

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    CreateRTXPTShaderSourceFactory(CreateInfo.pEngineFactory, "shaders", &pShaderSourceFactory);

    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.Desc.Name                  = m_Name.c_str();
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = CreateInfo.ShaderFilePath;
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    RefCntAutoPtr<IShader> pCS;
    CreateInfo.pStateCache->CreateShader(ShaderCI, &pCS);
    VERIFY(pCS, "Failed to create ", m_Name, " shader");
    if (!pCS)
        return false;

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = m_Name.c_str();
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.pCS                  = pCS;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout
        .AddVariable(SHADER_TYPE_COMPUTE, "g_Const", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_InputColor", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_Output", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    CreateInfo.pStateCache->CreateComputePipelineState(PSOCreateInfo, &m_PSO);
    VERIFY(m_PSO, "Failed to create ", m_Name, " PSO");
    if (!m_PSO)
        return false;

    m_PSO->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "g_Const")->Set(CreateInfo.pFrameConstants, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    m_PSO->CreateShaderResourceBinding(&m_SRB, true);
    VERIFY(m_SRB, "Failed to create ", m_Name, " SRB");
    if (!m_SRB)
        return false;

    // TODO(RTXPT-Port Phase 5.5): Restore RTXDI DI/GI and denoising-guide compute chains; R3 LightsBaker feedback uses RTXPTLightsBakerPass.
    m_Stats.Ready = true;
    return true;
}

bool RTXPTComputePass::Dispatch(IDeviceContext* pContext, ITextureView* pInputSRV, ITextureView* pOutputUAV, Uint32 Width, Uint32 Height)
{
    m_Stats.LastDispatchExecuted = false;

    if (!IsReady() || pInputSRV == nullptr || pOutputUAV == nullptr || Width == 0 || Height == 0)
        return false;

    m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_InputColor")->Set(pInputSRV);
    m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Output")->Set(pOutputUAV);

    pContext->SetPipelineState(m_PSO);
    pContext->CommitShaderResources(m_SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DispatchComputeAttribs DispatchAttribs;
    DispatchAttribs.ThreadGroupCountX = (Width + 7) / 8;
    DispatchAttribs.ThreadGroupCountY = (Height + 7) / 8;
    DispatchAttribs.ThreadGroupCountZ = 1;
    pContext->DispatchCompute(DispatchAttribs);

    m_Stats.LastDispatchExecuted = true;
    ++m_Stats.DispatchCount;
    return true;
}

} // namespace Diligent
