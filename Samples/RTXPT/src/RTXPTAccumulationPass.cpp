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

#include "RTXPTAccumulationPass.hpp"
#include "RTXPTShaderSource.hpp"
#include "RTXPTPipelineStateCache.hpp"

#include "DebugUtilities.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"

#include "GraphicsTypesX.hpp"

namespace Diligent
{

namespace
{

struct RTXPTAccumulationConstants
{
    float2 OutputSize          = float2{0.0f, 0.0f};
    float2 InputSize           = float2{0.0f, 0.0f};
    float2 InputTextureSizeInv = float2{0.0f, 0.0f};
    float2 PixelOffset         = float2{0.0f, 0.0f};
    float  BlendFactor         = 1.0f;
    float3 _Padding0           = float3{0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(RTXPTAccumulationConstants) == 48, "RTXPTAccumulationConstants must match RTXPTAccumulation.csh");

} // namespace

void RTXPTAccumulationPass::Reset()
{
    m_PSO.Release();
    m_SRB.Release();
    m_Constants.Release();
    m_LinearSampler.Release();
    m_Stats = {};
}

bool RTXPTAccumulationPass::Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache, IPipelineStateCache* pPSOCache, bool ComputeSupported)
{
    Reset();

    if (!ComputeSupported)
    {
        DEV_ERROR("RTXPT accumulation pass requires compute shader support");
        return false;
    }

    if (pDevice == nullptr)
    {
        DEV_ERROR("RTXPT accumulation pass requires a render device");
        return false;
    }

    if (pEngineFactory == nullptr)
    {
        DEV_ERROR("RTXPT accumulation pass requires an engine factory");
        return false;
    }

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    CreateRTXPTShaderSourceFactory(pEngineFactory, "shaders;shaders\\PostProcessing", &pShaderSourceFactory);

    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.Desc.Name                  = "RTXPT accumulation";
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = "PostProcessing/RTXPTAccumulation.csh";
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    RefCntAutoPtr<IShader> pCS;
    pStateCache->CreateShader(ShaderCI, &pCS);
    VERIFY(pCS, "Failed to create RTXPT accumulation shader");
    if (!pCS)
        return false;

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = "RTXPT accumulation PSO";
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.pCS                  = pCS;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout
        .AddVariable(SHADER_TYPE_COMPUTE, "g_AccumulationConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_InputColor", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_AccumulatedColor", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_OutputColor", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "s_LinearSampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    PSOCreateInfo.pPSOCache = SelectRTXPTPipelineStateCache(pDevice, PSOCreateInfo, pPSOCache);
    pStateCache->CreateComputePipelineState(PSOCreateInfo, &m_PSO);
    VERIFY(m_PSO, "Failed to create RTXPT accumulation PSO");
    if (!m_PSO)
        return false;

    BufferDesc ConstantsDesc;
    ConstantsDesc.Name           = "RTXPT accumulation constants";
    ConstantsDesc.Size           = sizeof(RTXPTAccumulationConstants);
    ConstantsDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage          = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_Constants);
    VERIFY(m_Constants, "Failed to create RTXPT accumulation constants");
    if (!m_Constants)
        return false;

    SamplerDesc LinearClamp;
    LinearClamp.Name      = "RTXPT accumulation linear sampler";
    LinearClamp.MinFilter = FILTER_TYPE_LINEAR;
    LinearClamp.MagFilter = FILTER_TYPE_LINEAR;
    LinearClamp.MipFilter = FILTER_TYPE_LINEAR;
    LinearClamp.AddressU  = TEXTURE_ADDRESS_CLAMP;
    LinearClamp.AddressV  = TEXTURE_ADDRESS_CLAMP;
    LinearClamp.AddressW  = TEXTURE_ADDRESS_CLAMP;
    pDevice->CreateSampler(LinearClamp, &m_LinearSampler);
    VERIFY(m_LinearSampler, "Failed to create RTXPT accumulation linear sampler");
    if (!m_LinearSampler)
        return false;

    auto SetStatic = [this](const char* Name, IDeviceObject* pObject) {
        if (pObject == nullptr)
        {
            DEV_ERROR("RTXPT accumulation pass static resource is null: ", Name);
            return false;
        }

        IShaderResourceVariable* pVar = m_PSO->GetStaticVariableByName(SHADER_TYPE_COMPUTE, Name);
        if (pVar == nullptr)
        {
            UNEXPECTED("RTXPT accumulation pass static shader variable is missing: ", Name);
            return false;
        }

        pVar->Set(pObject, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
        return true;
    };

    const bool Bound =
        SetStatic("g_AccumulationConstants", m_Constants) &&
        SetStatic("s_LinearSampler", m_LinearSampler);
    VERIFY(Bound, "Failed to bind RTXPT accumulation static resources");
    if (!Bound)
        return false;

    m_PSO->CreateShaderResourceBinding(&m_SRB, true);
    VERIFY(m_SRB, "Failed to create RTXPT accumulation SRB");
    if (!m_SRB)
        return false;

    m_Stats.Ready = true;
    return true;
}

bool RTXPTAccumulationPass::Render(IDeviceContext* pContext, const RTXPTAccumulationDispatch& Dispatch)
{
    m_Stats.LastDispatchExecuted = false;

    if (!IsReady() || pContext == nullptr ||
        Dispatch.pInputColorSRV == nullptr ||
        Dispatch.pAccumulatedRadianceUAV == nullptr ||
        Dispatch.pProcessedOutputUAV == nullptr ||
        Dispatch.InputWidth == 0 ||
        Dispatch.InputHeight == 0 ||
        Dispatch.OutputWidth == 0 ||
        Dispatch.OutputHeight == 0)
        return false;

    RTXPTAccumulationConstants Constants;
    Constants.OutputSize          = float2{static_cast<float>(Dispatch.OutputWidth), static_cast<float>(Dispatch.OutputHeight)};
    Constants.InputSize           = float2{static_cast<float>(Dispatch.InputWidth), static_cast<float>(Dispatch.InputHeight)};
    Constants.InputTextureSizeInv = float2{1.0f / static_cast<float>(Dispatch.InputWidth), 1.0f / static_cast<float>(Dispatch.InputHeight)};
    Constants.PixelOffset         = Dispatch.PixelOffset;
    Constants.BlendFactor         = Dispatch.BlendFactor;

    {
        MapHelper<RTXPTAccumulationConstants> Mapped{pContext, m_Constants, MAP_WRITE, MAP_FLAG_DISCARD};
        VERIFY(Mapped, "Failed to map RTXPT accumulation constants");
        if (!Mapped)
            return false;

        *Mapped = Constants;
    }

    auto SetVariable = [this](const char* Name, IDeviceObject* pObject) {
        if (pObject == nullptr)
            return false;

        IShaderResourceVariable* pVar = m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name);
        if (pVar == nullptr)
        {
            UNEXPECTED("RTXPT accumulation pass dynamic shader variable is missing: ", Name);
            return false;
        }

        pVar->Set(pObject);
        return true;
    };

    const bool Bound =
        SetVariable("t_InputColor", Dispatch.pInputColorSRV) &&
        SetVariable("u_AccumulatedColor", Dispatch.pAccumulatedRadianceUAV) &&
        SetVariable("u_OutputColor", Dispatch.pProcessedOutputUAV);
    VERIFY(Bound, "Failed to bind RTXPT accumulation dynamic resources");
    if (!Bound)
        return false;

    pContext->SetPipelineState(m_PSO);
    pContext->CommitShaderResources(m_SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DispatchComputeAttribs DispatchAttribs;
    DispatchAttribs.ThreadGroupCountX = (Dispatch.OutputWidth + 7u) / 8u;
    DispatchAttribs.ThreadGroupCountY = (Dispatch.OutputHeight + 7u) / 8u;
    DispatchAttribs.ThreadGroupCountZ = 1;
    pContext->DispatchCompute(DispatchAttribs);

    m_Stats.LastDispatchExecuted = true;
    ++m_Stats.DispatchCount;
    return true;
}

} // namespace Diligent
