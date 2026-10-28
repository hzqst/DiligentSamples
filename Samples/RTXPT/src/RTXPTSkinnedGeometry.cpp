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

#include <algorithm>

#include "DebugUtilities.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"

#include "AsyncPrefix.hpp"

namespace Diligent
{

void RTXPTSkinnedSceneGeometry::Reset()
{
    m_Nodes.clear();
    m_JointMatrices.clear();
    m_AssetBindings.clear();
    m_SkinnedVertexBuffer.Release();
    m_JointMatrixBuffer.Release();
    m_SkinningConstantsCB.Release();
    m_PSO.Release();
    m_Stats = {};
}

const RTXPTSkinnedSceneNodeGeometry* RTXPTSkinnedSceneGeometry::FindNode(RTXPTSceneId      ModelAssetId,
                                                                         RTXPTSceneId      ModelInstanceId,
                                                                         const GLTF::Node* pNode) const
{
    for (const RTXPTSkinnedSceneNodeGeometry& Node : m_Nodes)
    {
        if (Node.ModelAssetId == ModelAssetId &&
            Node.ModelInstanceId == ModelInstanceId &&
            Node.pNode == pNode)
        {
            return &Node;
        }
    }
    return nullptr;
}

bool RTXPTSkinnedSceneGeometry::BuildNodeTable(const RTXPTSceneGraphData& SceneData)
{
    Uint32 VertexBase = 0;
    Uint32 JointBase  = 0;

    for (Uint32 InstanceId = 0; InstanceId < SceneData.ModelInstances.size(); ++InstanceId)
    {
        const RTXPTModelInstance& Instance = SceneData.ModelInstances[InstanceId];
        if (Instance.ModelAssetId >= SceneData.ModelAssets.size())
        {
            LOG_ERROR_MESSAGE("RTXPT skinned geometry has an invalid model asset id");
            return false;
        }

        const RTXPTModelAsset& Asset = SceneData.ModelAssets[Instance.ModelAssetId];
        if (!Asset.Model || Asset.SceneIndex >= Asset.Model->Scenes.size())
        {
            LOG_ERROR_MESSAGE("RTXPT skinned geometry has an invalid model asset");
            return false;
        }

        const GLTF::Scene& Scene            = Asset.Model->Scenes[Asset.SceneIndex];
        const Uint32       SourceVertexBase = Asset.Model->GetBaseVertex();
        const IBuffer*     pVertexBuffer    = Asset.Model->GetVertexBufferCount() > 0 ? Asset.Model->GetVertexBuffer(0) : nullptr;
        const Uint64       SourceOffset     = Uint64{SourceVertexBase} * sizeof(RTXPTGeometryVertex);
        const Uint32       ModelVertexCount = pVertexBuffer != nullptr && pVertexBuffer->GetDesc().Size > SourceOffset ?
            static_cast<Uint32>((pVertexBuffer->GetDesc().Size - SourceOffset) / sizeof(RTXPTGeometryVertex)) :
            0;

        bool InstanceHasSkin = false;
        for (const GLTF::Node* pNode : Scene.LinearNodes)
        {
            if (pNode == nullptr || pNode->pMesh == nullptr || pNode->pSkin == nullptr)
                continue;

            if (Asset.Model->GetVertexBufferCount() <= 1 || ModelVertexCount == 0)
            {
                LOG_ERROR_MESSAGE("RTXPT skinned geometry requires source vertex and skin buffers");
                return false;
            }

            RTXPTSkinnedSceneNodeGeometry Node;
            Node.ModelAssetId     = Instance.ModelAssetId;
            Node.ModelInstanceId  = InstanceId;
            Node.pNode            = pNode;
            Node.SourceVertexBase = SourceVertexBase;
            Node.VertexBase       = VertexBase;
            Node.VertexCount      = ModelVertexCount;
            Node.JointBase        = JointBase;
            Node.JointCount       = static_cast<Uint32>(pNode->pSkin->Joints.size());
            m_Nodes.push_back(Node);

            VertexBase += Node.VertexCount;
            JointBase += Node.JointCount;
            InstanceHasSkin = true;
        }

        if (InstanceHasSkin)
            ++m_Stats.SkinnedInstanceCount;
    }

    m_Stats.SkinningJobCount   = static_cast<Uint32>(m_Nodes.size());
    m_Stats.SkinnedVertexCount = VertexBase;
    m_Stats.JointMatrixCount   = JointBase;
    return true;
}

bool RTXPTSkinnedSceneGeometry::CreateBuffers(IRenderDevice* pDevice)
{
    const Uint32 VertexCountForBuffer = std::max<Uint32>(m_Stats.SkinnedVertexCount, 1);

    BufferDesc VertexDesc;
    VertexDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    VertexDesc.Name                 = "RTXPT current skinned vertex arena";
    VertexDesc.Usage                = USAGE_DEFAULT;
    VertexDesc.BindFlags            = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS | BIND_RAY_TRACING;
    VertexDesc.Mode                 = BUFFER_MODE_STRUCTURED;
    VertexDesc.ElementByteStride    = sizeof(RTXPTGeometryVertex);
    VertexDesc.Size                 = Uint64{VertexCountForBuffer} * sizeof(RTXPTGeometryVertex);
    pDevice->CreateBuffer(VertexDesc, nullptr, &m_SkinnedVertexBuffer);
    VERIFY(m_SkinnedVertexBuffer, "Failed to create RTXPT skinned vertex arena");
    if (!m_SkinnedVertexBuffer)
        return false;

    if (m_Nodes.empty())
        return true;

    BufferDesc JointDesc;
    JointDesc.Name              = "RTXPT skinned joint matrices";
    JointDesc.Usage             = USAGE_DYNAMIC;
    JointDesc.BindFlags         = BIND_SHADER_RESOURCE;
    JointDesc.CPUAccessFlags    = CPU_ACCESS_WRITE;
    JointDesc.Mode              = BUFFER_MODE_STRUCTURED;
    JointDesc.ElementByteStride = sizeof(float4x4);
    JointDesc.Size              = Uint64{std::max<Uint32>(m_Stats.JointMatrixCount, 1)} * sizeof(float4x4);
    pDevice->CreateBuffer(JointDesc, nullptr, &m_JointMatrixBuffer);
    VERIFY(m_JointMatrixBuffer, "Failed to create RTXPT skinned joint matrix buffer");
    if (!m_JointMatrixBuffer)
        return false;

    BufferDesc ConstantsDesc;
    ConstantsDesc.Name           = "RTXPT skinning constants";
    ConstantsDesc.Usage          = USAGE_DYNAMIC;
    ConstantsDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    ConstantsDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    ConstantsDesc.Size           = sizeof(SkinningConstants);
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_SkinningConstantsCB);
    VERIFY(m_SkinningConstantsCB, "Failed to create RTXPT skinning constants");
    return m_SkinningConstantsCB != nullptr;
}

bool RTXPTSkinnedSceneGeometry::CreatePipeline(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache)
{
    if (m_Nodes.empty())
        return true;

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    pEngineFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders\\PathTracer", &pShaderSourceFactory);

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
    VERIFY(pCS, "Failed to create RTXPT skinned vertex build shader");
    if (!pCS)
        return false;

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = "RTXPT skinned vertex build PSO";
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.pCS                  = pCS;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout
        .AddVariable(SHADER_TYPE_COMPUTE, "cbSkinningConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_SourceVertices", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_SourceSkinData", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_JointMatrices", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_SkinnedVertices", SHADER_RESOURCE_VARIABLE_TYPE_STATIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    pStateCache->CreateComputePipelineState(PSOCreateInfo, &m_PSO);
    VERIFY(m_PSO, "Failed to create RTXPT skinned vertex build PSO");
    if (!m_PSO)
        return false;

    auto SetStatic = [&](const char* Name, IDeviceObject* pObject) {
        IShaderResourceVariable* pVar = m_PSO->GetStaticVariableByName(SHADER_TYPE_COMPUTE, Name);
        if (pVar == nullptr || pObject == nullptr)
            return false;
        pVar->Set(pObject, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
        return true;
    };

    const bool Bound =
        SetStatic("cbSkinningConstants", m_SkinningConstantsCB) &&
        SetStatic("t_JointMatrices", m_JointMatrixBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE)) &&
        SetStatic("u_SkinnedVertices", m_SkinnedVertexBuffer->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    VERIFY(Bound, "Failed to bind RTXPT skinned vertex build static resources");
    return Bound;
}

bool RTXPTSkinnedSceneGeometry::CreateAssetBindings(IRenderDevice* pDevice, const RTXPTSceneGraphData& SceneData)
{
    (void)pDevice;
    m_AssetBindings.assign(SceneData.ModelAssets.size(), {});
    for (Uint32 AssetId = 0; AssetId < SceneData.ModelAssets.size(); ++AssetId)
    {
        const RTXPTModelAsset& Asset = SceneData.ModelAssets[AssetId];
        if (!Asset.Model)
            continue;

        bool AssetHasSkinnedNode = false;
        for (const RTXPTSkinnedSceneNodeGeometry& Node : m_Nodes)
        {
            if (Node.ModelAssetId == AssetId)
            {
                AssetHasSkinnedNode = true;
                break;
            }
        }
        if (!AssetHasSkinnedNode)
            continue;

        if (Asset.Model->GetVertexBufferCount() <= 1)
        {
            LOG_ERROR_MESSAGE("RTXPT skinned scene asset is missing source vertex or skin buffers");
            return false;
        }

        RTXPTSkinnedSceneAssetBinding& Binding = m_AssetBindings[AssetId];
        Binding.ModelAssetId                   = AssetId;
        Binding.pSourceVertexBuffer            = Asset.Model->GetVertexBuffer(0);
        Binding.pSourceSkinBuffer              = Asset.Model->GetVertexBuffer(1);
        if (Binding.pSourceVertexBuffer == nullptr || Binding.pSourceSkinBuffer == nullptr)
        {
            LOG_ERROR_MESSAGE("RTXPT skinned scene asset is missing source vertex or skin buffers");
            return false;
        }

        m_PSO->CreateShaderResourceBinding(&Binding.pSRB, true);
        VERIFY(Binding.pSRB, "Failed to create RTXPT skinned vertex build SRB");
        if (!Binding.pSRB)
            return false;

        IShaderResourceVariable* pSourceVertices = Binding.pSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_SourceVertices");
        IShaderResourceVariable* pSourceSkinData = Binding.pSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_SourceSkinData");
        if (pSourceVertices == nullptr || pSourceSkinData == nullptr)
        {
            UNEXPECTED("Failed to find RTXPT skinned source buffer variables");
            return false;
        }

        pSourceVertices->Set(Binding.pSourceVertexBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        pSourceSkinData->Set(Binding.pSourceSkinBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
    }

    return true;
}

bool RTXPTSkinnedSceneGeometry::Initialize(IRenderDevice*             pDevice,
                                           IEngineFactory*            pEngineFactory,
                                           IRenderStateCache*         pStateCache,
                                           const RTXPTSceneGraphData& SceneData,
                                           bool                       ComputeSupported)
{
    Reset();

    if (pDevice == nullptr || pEngineFactory == nullptr)
    {
        DEV_ERROR("RTXPT skinned geometry requires a device and engine factory");
        return false;
    }

    if (!BuildNodeTable(SceneData))
        return false;

    if (!CreateBuffers(pDevice))
        return false;

    if (m_Nodes.empty())
    {
        m_Stats.Ready = true;
        return true;
    }

    if (!ComputeSupported)
    {
        DEV_ERROR("RTXPT skinned geometry requires compute shader support");
        return false;
    }

    if (!CreatePipeline(pDevice, pEngineFactory, pStateCache))
        return false;

    if (!CreateAssetBindings(pDevice, SceneData))
        return false;

    m_Stats.Ready = true;
    return true;
}

bool RTXPTSkinnedSceneGeometry::UploadJointMatrices(IDeviceContext* pContext, const RTXPTSceneGraphData& SceneData)
{
    m_JointMatrices.assign(std::max<Uint32>(m_Stats.JointMatrixCount, 1), float4x4::Identity());

    for (const RTXPTSkinnedSceneNodeGeometry& Node : m_Nodes)
    {
        if (Node.ModelInstanceId >= SceneData.ModelInstances.size() ||
            Node.pNode == nullptr ||
            Node.pNode->SkinTransformsIndex < 0)
        {
            LOG_ERROR_MESSAGE("RTXPT skinned node is missing skin transform data");
            return false;
        }

        const RTXPTModelInstance&    Instance   = SceneData.ModelInstances[Node.ModelInstanceId];
        const GLTF::ModelTransforms& Transforms = Instance.Transforms;
        const Uint32                 SkinIndex  = static_cast<Uint32>(Node.pNode->SkinTransformsIndex);
        if (SkinIndex >= Transforms.Skins.size())
        {
            LOG_ERROR_MESSAGE("RTXPT skinned node is missing skin transform data");
            return false;
        }

        const std::vector<float4x4>& Source = Transforms.Skins[SkinIndex].JointMatrices;
        if (Source.size() < Node.JointCount)
        {
            LOG_ERROR_MESSAGE("RTXPT skinned node is missing skin transform data");
            return false;
        }

        for (Uint32 i = 0; i < Node.JointCount; ++i)
            m_JointMatrices[Node.JointBase + i] = Source[i];
    }

    MapHelper<float4x4> Mapped{pContext, m_JointMatrixBuffer, MAP_WRITE, MAP_FLAG_DISCARD};
    VERIFY(Mapped, "Failed to map RTXPT skinned joint matrix buffer");
    if (!Mapped)
        return false;

    std::copy(m_JointMatrices.begin(), m_JointMatrices.end(), static_cast<float4x4*>(Mapped));
    return true;
}

bool RTXPTSkinnedSceneGeometry::Update(IDeviceContext* pContext, const RTXPTSceneGraphData& SceneData)
{
    m_Stats.LastDispatchExecuted = false;

    if (!IsReady())
        return false;

    if (m_Nodes.empty())
        return true;

    if (pContext == nullptr)
    {
        DEV_ERROR("RTXPT skinned geometry requires a device context");
        return false;
    }

    if (!UploadJointMatrices(pContext, SceneData))
        return false;

    pContext->SetPipelineState(m_PSO);

    for (const RTXPTSkinnedSceneNodeGeometry& Node : m_Nodes)
    {
        if (Node.ModelAssetId >= m_AssetBindings.size() || !m_AssetBindings[Node.ModelAssetId].pSRB)
        {
            LOG_ERROR_MESSAGE("RTXPT skinned geometry is missing an asset binding");
            return false;
        }

        SkinningConstants Constants;
        Constants.SourceVertexBase = Node.SourceVertexBase;
        Constants.DestVertexBase   = Node.VertexBase;
        Constants.JointBase        = Node.JointBase;
        Constants.VertexCount      = Node.VertexCount;

        {
            MapHelper<SkinningConstants> Mapped{pContext, m_SkinningConstantsCB, MAP_WRITE, MAP_FLAG_DISCARD};
            VERIFY(Mapped, "Failed to map RTXPT skinning constants");
            if (!Mapped)
                return false;

            *Mapped = Constants;
        }

        pContext->CommitShaderResources(m_AssetBindings[Node.ModelAssetId].pSRB,
                                        RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        DispatchComputeAttribs DispatchAttribs;
        DispatchAttribs.ThreadGroupCountX = (Node.VertexCount + 127u) / 128u;
        DispatchAttribs.ThreadGroupCountY = 1;
        DispatchAttribs.ThreadGroupCountZ = 1;
        pContext->DispatchCompute(DispatchAttribs);
    }

    m_Stats.LastDispatchExecuted = true;
    ++m_Stats.DispatchCount;
    return true;
}

} // namespace Diligent
