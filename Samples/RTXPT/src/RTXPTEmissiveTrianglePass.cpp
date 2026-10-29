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

#include "RTXPTEmissiveTrianglePass.hpp"
#include "RTXPTShaderSource.hpp"
#include "RTXPTPipelineStateCache.hpp"

#include "DebugUtilities.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"
#include "ShaderMacroHelper.hpp"

namespace Diligent
{

namespace
{

struct EmissiveTriangleBuildConstants
{
    Uint32 SubInstanceCount = 0;
    Uint32 Padding[3]       = {};
};
static_assert(sizeof(EmissiveTriangleBuildConstants) == 16, "EmissiveTriangleBuildConstants must match EmissiveTriangleBuild.hlsl");

} // namespace

void RTXPTEmissiveTrianglePass::Reset()
{
    m_PSO.Release();
    m_SRB.Release();
    m_IndexBufferView.Release();
    m_Constants.Release();
    m_Stats = {};
}

bool RTXPTEmissiveTrianglePass::Initialize(IRenderDevice* pDevice, const RTXPTEmissiveTrianglePassCreateInfo& CreateInfo)
{
    Reset();

    if (!CreateInfo.ComputeSupported)
    {
        DEV_ERROR("RTXPT emissive triangle pass requires compute shader support");
        return false;
    }

    // Sample emissive textures on the GPU (matching the closest-hit material bridge) when the scene carries
    // material textures. Bindless is mandatory, so the only factor-only case is a texture-less scene
    // (CreateInfo.MaterialTextureCount == 0), where the textured variant cannot be compiled.
    const bool UseTextures = CreateInfo.EnableMaterialTextures && CreateInfo.pMaterialTextures != nullptr && CreateInfo.MaterialTextureCount > 0;

    if (pDevice == nullptr || CreateInfo.pEngineFactory == nullptr || CreateInfo.pMaterialBuffer == nullptr || CreateInfo.pSubInstanceBuffer == nullptr ||
        CreateInfo.pSubInstanceTransformBuffer == nullptr || CreateInfo.pVertexBuffer == nullptr || CreateInfo.pSkinnedVertexBuffer == nullptr ||
        CreateInfo.pIndexBuffer == nullptr || CreateInfo.pEmissiveTriangleBuffer == nullptr)
    {
        DEV_ERROR("RTXPT emissive triangle pass requires all scene buffers");
        return false;
    }

    if (CreateInfo.IndexValueType != VT_UINT16 && CreateInfo.IndexValueType != VT_UINT32)
    {
        DEV_ERROR("RTXPT emissive triangle pass requires VT_UINT16 or VT_UINT32 indices");
        return false;
    }

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    CreateRTXPTShaderSourceFactory(CreateInfo.pEngineFactory, "shaders;shaders\\PathTracer", &pShaderSourceFactory);

    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.Desc.Name                  = "RTXPT emissive triangle build";
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = "PathTracer/EmissiveTriangleBuild.hlsl";
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    ShaderMacroHelper Macros;
    if (UseTextures)
    {
        Macros.Add("ENABLE_MATERIAL_TEXTURES", 1);
        Macros.Add("MATERIAL_TEXTURE_COUNT", static_cast<int>(CreateInfo.MaterialTextureCount));
    }
    ShaderCI.Macros = Macros;

    RefCntAutoPtr<IShader> pCS;
    CreateInfo.pStateCache->CreateShader(ShaderCI, &pCS);
    VERIFY(pCS, "Failed to create RTXPT emissive triangle build shader");
    if (!pCS)
        return false;

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = "RTXPT emissive triangle build PSO";
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.pCS                  = pCS;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout
        .AddVariable(SHADER_TYPE_COMPUTE, "g_EmissiveTriangleBuildConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_PTMaterialData", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_SubInstanceData", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_SubInstanceTransforms", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_VertexBuffer", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_SkinnedVertexBuffer", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_IndexBuffer", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_EmissiveTriangles", SHADER_RESOURCE_VARIABLE_TYPE_STATIC);
    if (UseTextures)
    {
        // The bindless material-texture array is bound per-build on the SRB (mutable); the sampler matches
        // the closest-hit material sampler (BSDFs/MaterialBridge.hlsli) so baked emissive
        // radiance is consistent with the BSDF-path emission.
        ResourceLayout.AddVariable(SHADER_TYPE_COMPUTE, "t_BindlessTextures", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE);

        const SamplerDesc MaterialSamplerDesc{
            FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR,
            TEXTURE_ADDRESS_WRAP, TEXTURE_ADDRESS_WRAP, TEXTURE_ADDRESS_WRAP};
        ResourceLayout.AddImmutableSampler(SHADER_TYPE_COMPUTE, "s_MaterialSampler", MaterialSamplerDesc);
    }
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    PSOCreateInfo.pPSOCache = SelectRTXPTPipelineStateCache(pDevice, PSOCreateInfo, CreateInfo.pPSOCache);
    CreateInfo.pStateCache->CreateComputePipelineState(PSOCreateInfo, &m_PSO);
    VERIFY(m_PSO, "Failed to create RTXPT emissive triangle build PSO");
    if (!m_PSO)
        return false;

    if (CreateInfo.IndexValueType != VT_UINT16 && CreateInfo.IndexValueType != VT_UINT32)
        return false;

    BufferViewDesc IndexViewDesc;
    IndexViewDesc.Name                 = "RTXPT emissive triangle index buffer SRV";
    IndexViewDesc.ViewType             = BUFFER_VIEW_SHADER_RESOURCE;
    IndexViewDesc.Format.ValueType     = CreateInfo.IndexValueType;
    IndexViewDesc.Format.NumComponents = 1;
    IndexViewDesc.Format.IsNormalized  = false;
    CreateInfo.pIndexBuffer->CreateView(IndexViewDesc, &m_IndexBufferView);
    VERIFY(m_IndexBufferView, "Failed to create RTXPT emissive triangle index buffer view");
    if (!m_IndexBufferView)
        return false;

    BufferDesc ConstantsDesc;
    ConstantsDesc.Name           = "RTXPT emissive triangle build constants";
    ConstantsDesc.Size           = sizeof(EmissiveTriangleBuildConstants);
    ConstantsDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage          = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_Constants);
    VERIFY(m_Constants, "Failed to create RTXPT emissive triangle build constants");
    if (!m_Constants)
        return false;

    auto SetStatic = [&](const char* Name, IDeviceObject* pObject) {
        if (pObject == nullptr)
        {
            DEV_ERROR("RTXPT emissive triangle pass static resource is null: ", Name);
            return false;
        }

        IShaderResourceVariable* pVar = m_PSO->GetStaticVariableByName(SHADER_TYPE_COMPUTE, Name);
        if (pVar == nullptr)
        {
            UNEXPECTED("RTXPT emissive triangle pass static shader variable is missing: ", Name);
            return false;
        }

        pVar->Set(pObject, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
        return true;
    };

    const bool Bound =
        SetStatic("g_EmissiveTriangleBuildConstants", m_Constants) &&
        SetStatic("t_PTMaterialData", CreateInfo.pMaterialBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetStatic("t_SubInstanceData", CreateInfo.pSubInstanceBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetStatic("t_SubInstanceTransforms", CreateInfo.pSubInstanceTransformBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetStatic("t_VertexBuffer", CreateInfo.pVertexBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetStatic("t_SkinnedVertexBuffer", CreateInfo.pSkinnedVertexBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetStatic("t_IndexBuffer", m_IndexBufferView) &&
        SetStatic("u_EmissiveTriangles", CreateInfo.pEmissiveTriangleBuffer->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    VERIFY(Bound, "Failed to bind RTXPT emissive triangle build static resources");
    if (!Bound)
        return false;

    m_PSO->CreateShaderResourceBinding(&m_SRB, true);
    VERIFY(m_SRB, "Failed to create RTXPT emissive triangle build SRB");
    if (!m_SRB)
        return false;

    if (UseTextures)
    {
        IShaderResourceVariable* pTexVar = m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_BindlessTextures");
        if (pTexVar == nullptr)
        {
            UNEXPECTED("RTXPT emissive triangle pass material texture array binding is missing");
            return false;
        }
        pTexVar->SetArray(CreateInfo.pMaterialTextures, 0, CreateInfo.MaterialTextureCount);
    }

    m_Stats.Ready = true;
    return true;
}

bool RTXPTEmissiveTrianglePass::Dispatch(IDeviceContext* pContext, Uint32 SubInstanceCount)
{
    m_Stats.LastDispatchExecuted = false;

    if (!IsReady() || pContext == nullptr || SubInstanceCount == 0)
        return false;

    MapHelper<EmissiveTriangleBuildConstants> MappedConstants{pContext, m_Constants, MAP_WRITE, MAP_FLAG_DISCARD};
    if (MappedConstants == nullptr)
    {
        DEV_ERROR("Failed to map RTXPT emissive triangle build constants");
        return false;
    }
    MappedConstants->SubInstanceCount = SubInstanceCount;

    pContext->SetPipelineState(m_PSO);
    pContext->CommitShaderResources(m_SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DispatchComputeAttribs DispatchAttribs;
    DispatchAttribs.ThreadGroupCountX = (SubInstanceCount + 63u) / 64u;
    DispatchAttribs.ThreadGroupCountY = 1;
    DispatchAttribs.ThreadGroupCountZ = 1;
    pContext->DispatchCompute(DispatchAttribs);

    m_Stats.LastDispatchExecuted = true;
    ++m_Stats.DispatchCount;
    return true;
}

} // namespace Diligent
