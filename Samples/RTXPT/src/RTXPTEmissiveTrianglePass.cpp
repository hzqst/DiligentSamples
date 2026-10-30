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
#include "AsyncPrefix.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

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
    m_IndexBuffer.Release();
    m_Constants.Release();
    m_Bound = {};
    m_BoundTextures.clear();
    m_IndexValueType = VT_UNDEFINED;
    m_UseTextures    = false;
    m_Stats          = {};
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
    // material textures. Bindless is mandatory, so the only factor-only case is a texture-less table
    // (CreateInfo.MaterialTextureCount == 0), where the textured variant cannot be compiled.
    const bool UseTextures = CreateInfo.EnableMaterialTextures && CreateInfo.MaterialTextureCount > 0;

    if (pDevice == nullptr || CreateInfo.pEngineFactory == nullptr)
    {
        DEV_ERROR("RTXPT emissive triangle pass requires a device and an engine factory");
        return false;
    }

    if (CreateInfo.IndexValueType != VT_UINT16 && CreateInfo.IndexValueType != VT_UINT32)
    {
        DEV_ERROR("RTXPT emissive triangle pass requires VT_UINT16 or VT_UINT32 indices");
        return false;
    }
    m_IndexValueType = CreateInfo.IndexValueType;
    m_UseTextures    = UseTextures;

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
    PSOCreateInfo.PSODesc.Name                 = "RTXPT emissive triangle build PSO";
    PSOCreateInfo.PSODesc.PipelineType         = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.PSODesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    PSOCreateInfo.pCS                          = pCS;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    // Scene resources are mutable: Bind creates a new resource binding when they change. None of their buffers is
    // USAGE_DYNAMIC, so they need no dynamic offsets.
    ResourceLayout.AddVariable(SHADER_TYPE_COMPUTE, "g_EmissiveTriangleBuildConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC);
    for (const char* Name : {"t_PTMaterialData", "t_SubInstanceData", "t_SubInstanceTransforms", "t_VertexBuffer", "t_SkinnedVertexBuffer", "u_EmissiveTriangles"})
        ResourceLayout.AddVariable(SHADER_TYPE_COMPUTE, Name, SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE, SHADER_VARIABLE_FLAG_NO_DYNAMIC_BUFFERS);
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

    BufferDesc ConstantsDesc;
    ConstantsDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    ConstantsDesc.Name                 = "RTXPT emissive triangle build constants";
    ConstantsDesc.Size                 = sizeof(EmissiveTriangleBuildConstants);
    ConstantsDesc.BindFlags            = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage                = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags       = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_Constants);
    VERIFY(m_Constants, "Failed to create RTXPT emissive triangle build constants");
    if (!m_Constants)
        return false;

    IShaderResourceVariable* pConstantsVar = m_PSO->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "g_EmissiveTriangleBuildConstants");
    if (pConstantsVar == nullptr)
    {
        UNEXPECTED("RTXPT emissive triangle pass constants are missing");
        return false;
    }
    pConstantsVar->Set(m_Constants, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);

    m_Stats.Ready = true;
    return true;
}

bool RTXPTEmissiveTrianglePass::Bind(const RTXPTEmissiveTriangleBindings& Bindings)
{
    if (!IsReady())
        return false;
    if (Bindings.pMaterialBuffer == nullptr || Bindings.pSubInstanceBuffer == nullptr || Bindings.pSubInstanceTransformBuffer == nullptr ||
        Bindings.pVertexBuffer == nullptr || Bindings.pSkinnedVertexBuffer == nullptr || Bindings.pIndexBuffer == nullptr ||
        Bindings.pEmissiveTriangleBuffer == nullptr)
    {
        DEV_ERROR("RTXPT emissive triangle pass requires all scene buffers");
        return false;
    }
    const Uint32 TextureCount = m_UseTextures ? Bindings.MaterialTextureCount : 0;
    if (m_UseTextures && Bindings.pMaterialTextures == nullptr)
    {
        DEV_ERROR("RTXPT emissive triangle pass requires the material textures it was compiled for");
        return false;
    }

    RTXPTEmissiveTriangleBindings Key = Bindings;
    Key.pMaterialTextures             = nullptr;
    if (m_SRB && std::memcmp(&Key, &m_Bound, sizeof(Key)) == 0 && m_BoundTextures.size() == TextureCount &&
        std::equal(m_BoundTextures.begin(), m_BoundTextures.end(), Bindings.pMaterialTextures))
        return true;

    if (m_IndexBuffer != Bindings.pIndexBuffer)
    {
        m_IndexBufferView.Release();
        m_IndexBuffer = Bindings.pIndexBuffer;
        BufferViewDesc IndexViewDesc;
        IndexViewDesc.Name                 = "RTXPT emissive triangle index buffer SRV";
        IndexViewDesc.ViewType             = BUFFER_VIEW_SHADER_RESOURCE;
        IndexViewDesc.Format.ValueType     = m_IndexValueType;
        IndexViewDesc.Format.NumComponents = 1;
        IndexViewDesc.Format.IsNormalized  = false;
        Bindings.pIndexBuffer->CreateView(IndexViewDesc, &m_IndexBufferView);
        if (!m_IndexBufferView)
        {
            m_IndexBuffer.Release();
            DEV_ERROR("Failed to create RTXPT emissive triangle index buffer view");
            return false;
        }
    }

    m_SRB.Release();
    RefCntAutoPtr<IShaderResourceBinding> pSRB;
    m_PSO->CreateShaderResourceBinding(&pSRB, true);
    if (!pSRB)
    {
        DEV_ERROR("Failed to create RTXPT emissive triangle build SRB");
        return false;
    }

    const std::pair<const char*, IDeviceObject*> Resources[] = {
        {"t_PTMaterialData", Bindings.pMaterialBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)},
        {"t_SubInstanceData", Bindings.pSubInstanceBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)},
        {"t_SubInstanceTransforms", Bindings.pSubInstanceTransformBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)},
        {"t_VertexBuffer", Bindings.pVertexBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)},
        {"t_SkinnedVertexBuffer", Bindings.pSkinnedVertexBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)},
        {"t_IndexBuffer", m_IndexBufferView},
        {"u_EmissiveTriangles", Bindings.pEmissiveTriangleBuffer->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS)},
    };
    for (const auto& Resource : Resources)
    {
        IShaderResourceVariable* pVar = pSRB->GetVariableByName(SHADER_TYPE_COMPUTE, Resource.first);
        if (pVar == nullptr || Resource.second == nullptr)
        {
            UNEXPECTED("RTXPT emissive triangle pass resource is missing: ", Resource.first);
            return false;
        }
        pVar->Set(Resource.second);
    }

    if (m_UseTextures)
    {
        IShaderResourceVariable* pTexVar = pSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_BindlessTextures");
        if (pTexVar == nullptr)
        {
            UNEXPECTED("RTXPT emissive triangle pass material texture array binding is missing");
            return false;
        }
        pTexVar->SetArray(Bindings.pMaterialTextures, 0, Bindings.MaterialTextureCount);
    }

    m_SRB   = std::move(pSRB);
    m_Bound = Key;
    m_BoundTextures.assign(Bindings.pMaterialTextures, Bindings.pMaterialTextures + TextureCount);
    return true;
}

bool RTXPTEmissiveTrianglePass::Dispatch(IDeviceContext* pContext, Uint32 SubInstanceCount)
{
    m_Stats.LastDispatchExecuted = false;

    if (!IsReady() || !m_SRB || pContext == nullptr || SubInstanceCount == 0)
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
