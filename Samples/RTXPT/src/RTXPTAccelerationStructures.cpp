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

#include "RTXPTAccelerationStructures.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "DebugUtilities.hpp"
#include "GraphicsAccessories.hpp"
#include "MapHelper.hpp"
#include "RTXPTMaterials.hpp"

#include "AsyncPrefix.hpp"

namespace Diligent
{

namespace
{

constexpr Uint32 MaxTLASCustomID         = (1u << 24) - 1u;
constexpr Uint32 MaxSubInstanceDataCount = MaxTLASCustomID + 1u;
constexpr Uint32 kRTXPTHitGroupStride    = 2; // Keep in sync with RTXPT_HIT_GROUP_STRIDE in PathTracer/Config.h.

struct PositionLayout
{
    bool       Valid          = false;
    Uint8      BufferId       = 0;
    VALUE_TYPE ValueType      = VT_UNDEFINED;
    Uint8      ComponentCount = 0;
    Uint32     RelativeOffset = 0;
};

PositionLayout FindPositionLayout(const GLTF::Model& Model)
{
    for (Uint32 Attr = 0; Attr < Model.GetNumVertexAttributes(); ++Attr)
    {
        const GLTF::VertexAttributeDesc& Desc = Model.GetVertexAttribute(Attr);
        if (Desc.Name != nullptr && std::strcmp(Desc.Name, GLTF::PositionAttributeName) == 0)
        {
            PositionLayout Layout;
            Layout.Valid          = true;
            Layout.BufferId       = Desc.BufferId;
            Layout.ValueType      = Desc.ValueType;
            Layout.ComponentCount = Desc.NumComponents;
            Layout.RelativeOffset = Desc.RelativeOffset == ~0U ? 0 : Desc.RelativeOffset;
            return Layout;
        }
    }
    return {};
}

Uint32 GetVertexStride(const GLTF::Model& Model, Uint8 BufferId)
{
    Uint32 Stride = 0;
    for (Uint32 Attr = 0; Attr < Model.GetNumVertexAttributes(); ++Attr)
    {
        const GLTF::VertexAttributeDesc& Desc = Model.GetVertexAttribute(Attr);
        if (Desc.BufferId == BufferId)
            Stride = std::max(Stride, Desc.RelativeOffset + GetValueSize(Desc.ValueType) * Desc.NumComponents);
    }
    return Stride;
}

bool HasBindFlag(const IBuffer* pBuffer, BIND_FLAGS Flag)
{
    return pBuffer != nullptr && (pBuffer->GetDesc().BindFlags & Flag) != 0;
}

InstanceMatrix ToInstanceMatrix(const float4x4& Transform)
{
    InstanceMatrix Matrix;
    Matrix.SetRotation(&Transform._11, 4);
    Matrix.SetTranslation(Transform._41, Transform._42, Transform._43);
    return Matrix;
}

} // namespace

void RTXPTAccelerationStructures::Reset()
{
    m_GPUTLAS = {};
    m_BLASRecords.clear();
    m_TLAS.Release();
    m_BLASScratch.Release();
    m_TLASScratch.Release();
    m_InstanceBuffer.Release();
    m_SubInstanceBuffer.Release();
    m_SubInstanceTransformBuffer.Release();
    m_SubInstanceTransforms.clear();
    m_InstanceNames.clear();
    m_TLASInstances.clear();
    m_Stats = {};
}

bool RTXPTAccelerationStructures::BuildScene(IRenderDevice* pDevice, IDeviceContext* pContext, const RTXPTAccelerationStructuresBuildAttribs& BuildAttribs)
{
    Reset();
    m_Stats.RayTracingSupported = BuildAttribs.RayTracingSupported;

    if (pDevice == nullptr || pContext == nullptr || BuildAttribs.pSceneData == nullptr)
    {
        DEV_ERROR("RTXPT acceleration structure build requires a device, device context and scene data");
        return false;
    }
    const RTXPTSceneGraphData& SceneData = *BuildAttribs.pSceneData;

    if (!BuildAttribs.RayTracingSupported)
    {
        DEV_ERROR("RTXPT acceleration structure build requires ray tracing support");
        return false;
    }

    if (SceneData.ModelAssets.empty() || SceneData.ModelInstances.empty())
    {
        // Empty scenes deliberately remain unbuilt; the sample presents its clear fallback.
        return false;
    }

    if (BuildAttribs.IndexType != VT_UINT16 && BuildAttribs.IndexType != VT_UINT32)
    {
        LOG_ERROR_MESSAGE("RTXPT index buffer has an unsupported index size");
        return false;
    }
    const Uint32 IndexSize = GetValueSize(BuildAttribs.IndexType);

    const bool     SkinnedGeometryReady  = BuildAttribs.pSkinnedGeometry != nullptr && BuildAttribs.pSkinnedGeometry->IsReady();
    const Uint32   SkinningDispatchCount = BuildAttribs.pSkinnedGeometry != nullptr ? BuildAttribs.pSkinnedGeometry->GetStats().DispatchCount : 0;
    IBuffer* const pSkinnedVertexBuffer  = SkinnedGeometryReady ?
        BuildAttribs.pSkinnedGeometry->GetSkinnedVertexBuffer() :
        nullptr;

    std::vector<SubInstanceData> SubInstances;
    m_TLASInstances.reserve(SceneData.GraphNodes.size());
    m_InstanceNames.reserve(SceneData.GraphNodes.size());
    SubInstances.reserve(SceneData.GraphNodes.size());
    m_SubInstanceTransforms.clear();
    m_SubInstanceTransforms.reserve(SceneData.GraphNodes.size());

    Uint32 SubInstanceBase        = 0;
    Uint32 EmissiveTriangleOffset = 0;
    for (Uint32 InstanceId = 0; InstanceId < SceneData.ModelInstances.size(); ++InstanceId)
    {
        const RTXPTModelInstance& Instance = SceneData.ModelInstances[InstanceId];
        if (Instance.ModelAssetId >= SceneData.ModelAssets.size())
        {
            LOG_ERROR_MESSAGE("RTXPT scene instance references an invalid model asset");
            return false;
        }

        const RTXPTModelAsset& Asset = SceneData.ModelAssets[Instance.ModelAssetId];
        if (!Asset.Model || Asset.SceneIndex >= Asset.Model->Scenes.size())
        {
            LOG_ERROR_MESSAGE("RTXPT acceleration structure build found an invalid model asset");
            return false;
        }

        const GLTF::Model&   Model    = *Asset.Model;
        const PositionLayout Position = FindPositionLayout(Model);
        if (!Position.Valid || Position.BufferId != 0 || Position.ValueType != VT_FLOAT32 || Position.ComponentCount != 3)
        {
            LOG_ERROR_MESSAGE("RTXPT BLAS build requires float3 POSITION vertex data in vertex buffer 0");
            return false;
        }

        std::vector<Uint8> MaterialNeedsAnyHit(Model.Materials.size(), Uint8{0});
        std::vector<Uint8> MaterialAlphaTested(Model.Materials.size(), Uint8{0});
        std::vector<Uint8> MaterialExcludedFromNEE(Model.Materials.size(), Uint8{0});
        std::vector<Uint8> MaterialEmissiveAreaLight(Model.Materials.size(), Uint8{0});
        for (Uint32 MatIdx = 0; MatIdx < Model.Materials.size(); ++MatIdx)
        {
            const GLTF::Material&         Material            = Model.Materials[MatIdx];
            const RTXPTMaterialExtension* pExtension          = RTXPTGetMaterialExtension(SceneData, Asset, MatIdx);
            const bool                    HasBaseColorTexture = RTXPTMaterialHasBaseColorTexture(Model, Material, pExtension);
            const bool                    IsAlphaTested       = RTXPTMaterialIsAlphaTested(Material, pExtension, HasBaseColorTexture);
            const bool                    IsExcludedFromNEE   = RTXPTMaterialIsExcludedFromNEE(pExtension);
            const bool                    NeedsAnyHit         = RTXPTMaterialNeedsAnyHit(Material, pExtension, HasBaseColorTexture);
            MaterialAlphaTested[MatIdx]                       = IsAlphaTested ? Uint8{1} : Uint8{0};
            MaterialExcludedFromNEE[MatIdx]                   = IsExcludedFromNEE ? Uint8{1} : Uint8{0};
            MaterialNeedsAnyHit[MatIdx]                       = NeedsAnyHit ? Uint8{1} : Uint8{0};
            MaterialEmissiveAreaLight[MatIdx]                 = RTXPTMaterialIsEmissiveAreaLight(Material, pExtension) ? Uint8{1} : Uint8{0};
        }

        IBuffer* pVertexBuffer = Model.GetVertexBuffer(Position.BufferId, pDevice, pContext);
        if (!HasBindFlag(pVertexBuffer, BIND_RAY_TRACING))
        {
            LOG_ERROR_MESSAGE("RTXPT POSITION vertex buffer is missing BIND_RAY_TRACING");
            return false;
        }

        IBuffer* pIndexBuffer = Model.GetIndexBuffer(pDevice, pContext);
        if (pIndexBuffer != nullptr && !HasBindFlag(pIndexBuffer, BIND_RAY_TRACING))
        {
            LOG_ERROR_MESSAGE("RTXPT index buffer is missing BIND_RAY_TRACING");
            return false;
        }

        const GLTF::Scene& Scene        = Model.Scenes[Asset.SceneIndex];
        const Uint32       VertexStride = GetVertexStride(Model, Position.BufferId);
        const Uint32       BaseVertex   = Model.GetBaseVertex();
        const Uint32       FirstIndex   = pIndexBuffer != nullptr ? Model.GetFirstIndexLocation() : 0;

        if (VertexStride == 0)
        {
            LOG_ERROR_MESSAGE("RTXPT POSITION vertex buffer has an invalid stride");
            return false;
        }

        const Uint64 ModelVertexOffset = Uint64{BaseVertex} * Uint64{VertexStride} + Uint64{Position.RelativeOffset};
        if (ModelVertexOffset >= pVertexBuffer->GetDesc().Size)
        {
            LOG_ERROR_MESSAGE("RTXPT POSITION vertex buffer is too small for the model base vertex");
            return false;
        }
        const Uint64 ModelVertexCount64 = (pVertexBuffer->GetDesc().Size - ModelVertexOffset) / VertexStride;
        if (ModelVertexCount64 == 0 || ModelVertexCount64 > Uint64{std::numeric_limits<Uint32>::max()})
        {
            LOG_ERROR_MESSAGE("RTXPT POSITION vertex buffer has an invalid model vertex count");
            return false;
        }
        const Uint32 ModelVertexCount = static_cast<Uint32>(ModelVertexCount64);

        const GLTF::ModelTransforms& Transforms = Instance.Transforms.NodeGlobalMatrices.empty() ?
            Asset.StaticTransforms :
            Instance.Transforms;

        for (const GLTF::Node* pNode : Scene.LinearNodes)
        {
            if (pNode == nullptr || pNode->pMesh == nullptr)
                continue;

            if (pNode->Index < 0 || static_cast<size_t>(pNode->Index) >= Transforms.NodeGlobalMatrices.size())
                continue;

            const RTXPTSkinnedSceneNodeGeometry* pSkinnedNode =
                BuildAttribs.pSkinnedGeometry != nullptr ?
                BuildAttribs.pSkinnedGeometry->FindNode(Instance.ModelAssetId, InstanceId, pNode) :
                nullptr;
            if (pNode->pSkin != nullptr)
            {
                if (BuildAttribs.pSkinnedGeometry == nullptr)
                {
                    LOG_ERROR_MESSAGE("RTXPT skinned geometry requires current-frame skinned geometry");
                    return false;
                }

                if (pSkinnedNode == nullptr)
                {
                    LOG_ERROR_MESSAGE("RTXPT skinned geometry is missing a node record");
                    return false;
                }

                if (!SkinnedGeometryReady || SkinningDispatchCount == 0)
                {
                    LOG_ERROR_MESSAGE("RTXPT skinned geometry is not ready for BLAS build");
                    return false;
                }
            }
            const bool IsSkinnedNode =
                SkinnedGeometryReady && pSkinnedNode != nullptr && pSkinnedVertexBuffer != nullptr;
            IBuffer*     pNodeVertexBuffer     = IsSkinnedNode ? pSkinnedVertexBuffer : pVertexBuffer;
            const Uint32 NodeVertexStride      = IsSkinnedNode ? static_cast<Uint32>(sizeof(RTXPTGeometryVertex)) : VertexStride;
            const Uint64 NodeModelVertexOffset = IsSkinnedNode ?
                Uint64{pSkinnedNode->VertexBase} * sizeof(RTXPTGeometryVertex) :
                ModelVertexOffset;
            const Uint32 NodeModelVertexCount = IsSkinnedNode ? pSkinnedNode->VertexCount : ModelVertexCount;

            std::vector<std::string>           GeometryNames;
            std::vector<BLASTriangleDesc>      TriangleDescs;
            std::vector<BLASBuildTriangleData> TriangleData;

            GeometryNames.reserve(pNode->pMesh->Primitives.size());
            TriangleDescs.reserve(pNode->pMesh->Primitives.size());
            TriangleData.reserve(pNode->pMesh->Primitives.size());

            Uint32 PrimitiveIndex = 0;
            for (const GLTF::Primitive& Primitive : pNode->pMesh->Primitives)
            {
                if (Primitive.VertexCount == 0 && Primitive.IndexCount == 0)
                    continue;

                const std::string NodeName = pNode->Name.empty() ? "RTXPTGeometry" : pNode->Name;
                GeometryNames.emplace_back(Instance.Name + "_" + NodeName + "_" + std::to_string(PrimitiveIndex));

                const bool   IsIndexed                 = Primitive.HasIndices();
                const Uint32 TriangleCount             = IsIndexed ? Primitive.IndexCount / 3u : Primitive.VertexCount / 3u;
                const bool   GeometryEmissiveAreaLight = Primitive.MaterialId < MaterialEmissiveAreaLight.size() &&
                    MaterialEmissiveAreaLight[Primitive.MaterialId] != 0;

                SubInstanceData SubEntry;
                SubEntry.MaterialID = Primitive.MaterialId < Asset.MaterialRemap.size() ?
                    Asset.MaterialRemap[Primitive.MaterialId] :
                    0;
                SubEntry.VertexCount            = Primitive.VertexCount;
                SubEntry.EmissiveTriangleOffset = EmissiveTriangleOffset;
                if (IsSkinnedNode)
                {
                    SubEntry.Flags |= kSubInstanceFlag_Skinned;
                    SubEntry.VertexOffset = pSkinnedNode->VertexBase + Primitive.FirstVertex;
                }
                else
                {
                    SubEntry.VertexOffset = Asset.GlobalVertexBase + BaseVertex + Primitive.FirstVertex;
                }
                if (IsIndexed)
                {
                    SubEntry.Flags |= kSubInstanceFlag_Indexed;
                    SubEntry.IndexOffset = Asset.GlobalIndexBase + FirstIndex + Primitive.FirstIndex;
                    SubEntry.IndexCount  = Primitive.IndexCount;
                }
                SubInstances.emplace_back(SubEntry);
                m_SubInstanceTransforms.emplace_back(Transforms.NodeGlobalMatrices[pNode->Index]);
                if (GeometryEmissiveAreaLight)
                {
                    if (Uint64{EmissiveTriangleOffset} + Uint64{TriangleCount} > Uint64{std::numeric_limits<Uint32>::max()})
                    {
                        LOG_ERROR_MESSAGE("RTXPT emissive triangle offset exceeds Uint32");
                        return false;
                    }
                    EmissiveTriangleOffset += TriangleCount;
                }

                // The GLTF builder bakes VertexStart into indexed primitives and resets FirstVertex to 0.
                // BLAS indexed geometry therefore needs the whole model vertex range, not Primitive.VertexCount.
                const Uint64 PrimitiveVertexOff = IsIndexed ?
                    NodeModelVertexOffset :
                    (IsSkinnedNode ?
                         (Uint64{pSkinnedNode->VertexBase + Primitive.FirstVertex} * sizeof(RTXPTGeometryVertex)) :
                         (Uint64{BaseVertex + Primitive.FirstVertex} * Uint64{VertexStride} + Uint64{Position.RelativeOffset}));
                const Uint32 PrimitiveVertexCnt = IsIndexed ? NodeModelVertexCount : Primitive.VertexCount;

                BLASTriangleDesc TriangleDesc;
                TriangleDesc.GeometryName         = GeometryNames.back().c_str();
                TriangleDesc.MaxVertexCount       = PrimitiveVertexCnt;
                TriangleDesc.VertexValueType      = Position.ValueType;
                TriangleDesc.VertexComponentCount = Position.ComponentCount;
                TriangleDesc.MaxPrimitiveCount    = TriangleCount;
                TriangleDesc.IndexType            = IsIndexed ? BuildAttribs.IndexType : VT_UNDEFINED;

                BLASBuildTriangleData BuildData;
                BuildData.GeometryName         = TriangleDesc.GeometryName;
                BuildData.pVertexBuffer        = pNodeVertexBuffer;
                BuildData.VertexOffset         = PrimitiveVertexOff;
                BuildData.VertexStride         = NodeVertexStride;
                BuildData.VertexCount          = PrimitiveVertexCnt;
                BuildData.VertexValueType      = Position.ValueType;
                BuildData.VertexComponentCount = Position.ComponentCount;
                BuildData.PrimitiveCount       = TriangleDesc.MaxPrimitiveCount;
                // Alpha-tested and ExcludeFromNEE geometry require programmable candidate filtering.
                // Everything else stays opaque to skip any-hit entirely.
                const bool GeometryNeedsAnyHit = Primitive.MaterialId < MaterialNeedsAnyHit.size() &&
                    MaterialNeedsAnyHit[Primitive.MaterialId] != 0;
                BuildData.Flags                = GeometryNeedsAnyHit ? RAYTRACING_GEOMETRY_FLAG_NONE : RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                const bool GeometryAlphaTested = Primitive.MaterialId < MaterialAlphaTested.size() &&
                    MaterialAlphaTested[Primitive.MaterialId] != 0;
                if (GeometryAlphaTested)
                    ++m_Stats.AlphaTestedGeometryCount;
                const bool GeometryExcludedFromNEE = Primitive.MaterialId < MaterialExcludedFromNEE.size() &&
                    MaterialExcludedFromNEE[Primitive.MaterialId] != 0;
                if (GeometryExcludedFromNEE)
                    ++m_Stats.ExcludedFromNEEGeometryCount;

                if (IsIndexed)
                {
                    BuildData.pIndexBuffer = pIndexBuffer;
                    BuildData.IndexOffset  = (FirstIndex + Primitive.FirstIndex) * IndexSize;
                    BuildData.IndexType    = BuildAttribs.IndexType;
                }

                TriangleDescs.emplace_back(TriangleDesc);
                TriangleData.emplace_back(BuildData);
                ++PrimitiveIndex;
            }

            if (TriangleDescs.empty())
                continue;

            BLASRecord        Record;
            const std::string NodeName   = pNode->Name.empty() ? "node" : pNode->Name;
            Record.Name                  = "RTXPT BLAS " + Instance.Name + " " + NodeName + " node_" + std::to_string(pNode->Index);
            Record.GeometryCount         = static_cast<Uint32>(TriangleDescs.size());
            Record.Dynamic               = IsSkinnedNode;
            Record.VertexBuffer          = pNodeVertexBuffer;
            Record.IndexBuffer           = pIndexBuffer;
            Record.ModelAssetId          = Instance.ModelAssetId;
            Record.ModelInstanceId       = InstanceId;
            Record.pNode                 = pNode;
            Record.InstanceIndex         = static_cast<Uint32>(m_TLASInstances.size());
            Record.SubInstanceBase       = SubInstanceBase;
            Record.SkinningDispatchCount = IsSkinnedNode ? SkinningDispatchCount : 0;
            Record.GeometryNames         = std::move(GeometryNames);
            Record.TriangleData          = TriangleData;
            for (size_t i = 0; i < Record.TriangleData.size(); ++i)
            {
                const char* GeometryName            = Record.GeometryNames[i].c_str();
                TriangleDescs[i].GeometryName       = GeometryName;
                Record.TriangleData[i].GeometryName = GeometryName;
            }

            if (Uint64{SubInstanceBase} + Uint64{Record.GeometryCount} > Uint64{MaxSubInstanceDataCount})
            {
                LOG_ERROR_MESSAGE("RTXPT sub-instance table exceeds the 24-bit TLAS CustomId range");
                return false;
            }

            BottomLevelASDesc BLASDesc;
            BLASDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
            BLASDesc.Name                 = Record.Name.c_str();
            BLASDesc.Flags                = IsSkinnedNode ? RAYTRACING_BUILD_AS_ALLOW_UPDATE : RAYTRACING_BUILD_AS_NONE;
            BLASDesc.pTriangles           = TriangleDescs.data();
            BLASDesc.TriangleCount        = static_cast<Uint32>(TriangleDescs.size());
            pDevice->CreateBLAS(BLASDesc, &Record.BLAS);

            VERIFY(Record.BLAS, "Failed to create RTXPT BLAS");
            if (!Record.BLAS)
                return false;

            const Uint64 ScratchSize = std::max(Record.BLAS->GetScratchBufferSizes().Build,
                                                Record.BLAS->GetScratchBufferSizes().Update);
            m_Stats.BLASScratchSize  = std::max(m_Stats.BLASScratchSize, ScratchSize);

            if (!m_BLASScratch || m_BLASScratch->GetDesc().Size < ScratchSize)
            {
                BufferDesc ScratchDesc;
                ScratchDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
                ScratchDesc.Name                 = "RTXPT BLAS scratch buffer";
                ScratchDesc.Usage                = USAGE_DEFAULT;
                ScratchDesc.BindFlags            = BIND_RAY_TRACING;
                ScratchDesc.Size                 = ScratchSize;
                m_BLASScratch.Release();
                pDevice->CreateBuffer(ScratchDesc, nullptr, &m_BLASScratch);
                VERIFY(m_BLASScratch, "Failed to create RTXPT BLAS scratch buffer");
                if (!m_BLASScratch)
                    return false;
            }

            BuildBLASAttribs BLASAttribs;
            BLASAttribs.pBLAS                       = Record.BLAS;
            BLASAttribs.BLASTransitionMode          = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            BLASAttribs.GeometryTransitionMode      = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            BLASAttribs.pTriangleData               = Record.TriangleData.data();
            BLASAttribs.TriangleDataCount           = static_cast<Uint32>(Record.TriangleData.size());
            BLASAttribs.pScratchBuffer              = m_BLASScratch;
            BLASAttribs.ScratchBufferTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            pContext->BuildBLAS(BLASAttribs);

            m_InstanceNames.emplace_back(Record.Name);

            TLASBuildInstanceData TLASInstance;
            TLASInstance.InstanceName = m_InstanceNames.back().c_str();
            TLASInstance.pBLAS        = Record.BLAS;
            TLASInstance.Transform    = ToInstanceMatrix(Transforms.NodeGlobalMatrices[pNode->Index]);
            TLASInstance.CustomId     = SubInstanceBase;
            TLASInstance.Flags        = RAYTRACING_INSTANCE_NONE;
            TLASInstance.Mask         = 0xFF;
            m_TLASInstances.emplace_back(TLASInstance);

            SubInstanceBase += Record.GeometryCount;
            m_Stats.GeometryCount += Record.GeometryCount;
            m_BLASRecords.emplace_back(std::move(Record));
        }
    }

    if (m_TLASInstances.empty())
    {
        // A scene may contain cameras/lights only. Never retain the previous TLAS.
        return false;
    }

    for (size_t i = 0; i < m_TLASInstances.size(); ++i)
        m_TLASInstances[i].InstanceName = m_InstanceNames[i].c_str();

    TopLevelASDesc TLASDesc;
    TLASDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    TLASDesc.Name                 = "RTXPT TLAS";
    TLASDesc.MaxInstanceCount     = static_cast<Uint32>(m_TLASInstances.size());
    TLASDesc.Flags                = RAYTRACING_BUILD_AS_ALLOW_UPDATE;
    pDevice->CreateTLAS(TLASDesc, &m_TLAS);

    VERIFY(m_TLAS, "Failed to create RTXPT TLAS");
    if (!m_TLAS)
        return false;

    const Uint64 TLASBuildScratchSize  = m_TLAS->GetScratchBufferSizes().Build;
    const Uint64 TLASUpdateScratchSize = m_TLAS->GetScratchBufferSizes().Update;
    m_Stats.TLASScratchSize            = std::max(TLASBuildScratchSize, TLASUpdateScratchSize);

    BufferDesc InstanceBufferDesc;
    InstanceBufferDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    InstanceBufferDesc.Name                 = "RTXPT TLAS instance buffer";
    InstanceBufferDesc.Usage                = USAGE_DEFAULT;
    InstanceBufferDesc.BindFlags            = BIND_RAY_TRACING | BIND_UNORDERED_ACCESS;
    InstanceBufferDesc.Mode                 = BUFFER_MODE_RAW;
    InstanceBufferDesc.Size                 = Uint64{TLAS_INSTANCE_DATA_SIZE} * Uint64{m_TLASInstances.size()};
    pDevice->CreateBuffer(InstanceBufferDesc, nullptr, &m_InstanceBuffer);
    VERIFY(m_InstanceBuffer, "Failed to create RTXPT TLAS instance buffer");
    if (!m_InstanceBuffer)
        return false;

    BufferDesc TLASScratchDesc;
    TLASScratchDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    TLASScratchDesc.Name                 = "RTXPT TLAS scratch buffer";
    TLASScratchDesc.Usage                = USAGE_DEFAULT;
    TLASScratchDesc.BindFlags            = BIND_RAY_TRACING;
    TLASScratchDesc.Size                 = m_Stats.TLASScratchSize;
    pDevice->CreateBuffer(TLASScratchDesc, nullptr, &m_TLASScratch);
    VERIFY(m_TLASScratch, "Failed to create RTXPT TLAS scratch buffer");
    if (!m_TLASScratch)
        return false;

    BuildTLASAttribs TLASAttribs;
    TLASAttribs.pTLAS                        = m_TLAS;
    TLASAttribs.TLASTransitionMode           = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    TLASAttribs.BLASTransitionMode           = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    TLASAttribs.pInstances                   = m_TLASInstances.data();
    TLASAttribs.InstanceCount                = static_cast<Uint32>(m_TLASInstances.size());
    TLASAttribs.pInstanceBuffer              = m_InstanceBuffer;
    TLASAttribs.InstanceBufferTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    TLASAttribs.HitGroupStride               = kRTXPTHitGroupStride;
    TLASAttribs.BindingMode                  = HIT_GROUP_BINDING_MODE_PER_GEOMETRY;
    TLASAttribs.pScratchBuffer               = m_TLASScratch;
    TLASAttribs.ScratchBufferTransitionMode  = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    pContext->BuildTLAS(TLASAttribs);

    if (SubInstances.empty())
    {
        // Always upload at least one dummy entry so the bridge buffer can be bound unconditionally.
        SubInstances.push_back(SubInstanceData{});
        m_SubInstanceTransforms.push_back(float4x4::Identity());
    }

    BufferDesc SubInstanceDesc;
    SubInstanceDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    SubInstanceDesc.Name                 = "RTXPT sub-instance buffer";
    SubInstanceDesc.Usage                = USAGE_IMMUTABLE;
    SubInstanceDesc.BindFlags            = BIND_SHADER_RESOURCE;
    SubInstanceDesc.Mode                 = BUFFER_MODE_STRUCTURED;
    SubInstanceDesc.ElementByteStride    = sizeof(SubInstanceData);
    SubInstanceDesc.Size                 = sizeof(SubInstanceData) * SubInstances.size();

    BufferData SubInstanceData{SubInstances.data(), SubInstanceDesc.Size};
    pDevice->CreateBuffer(SubInstanceDesc, &SubInstanceData, &m_SubInstanceBuffer);
    VERIFY(m_SubInstanceBuffer, "Failed to create RTXPT sub-instance buffer");
    if (!m_SubInstanceBuffer)
        return false;

    BufferDesc TransformDesc;
    TransformDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    TransformDesc.Name                 = "RTXPT sub-instance transform buffer";
    TransformDesc.Usage                = USAGE_DYNAMIC;
    TransformDesc.BindFlags            = BIND_SHADER_RESOURCE;
    TransformDesc.CPUAccessFlags       = CPU_ACCESS_WRITE;
    TransformDesc.Mode                 = BUFFER_MODE_STRUCTURED;
    TransformDesc.ElementByteStride    = sizeof(float4x4);
    TransformDesc.Size                 = Uint64{m_SubInstanceTransforms.size()} * sizeof(float4x4);
    pDevice->CreateBuffer(TransformDesc, nullptr, &m_SubInstanceTransformBuffer);
    VERIFY(m_SubInstanceTransformBuffer, "Failed to create RTXPT sub-instance transform buffer");
    if (!m_SubInstanceTransformBuffer)
        return false;

    if (!UploadSubInstanceTransforms(pContext)) return false;

    if (BuildAttribs.pEngineFactory != nullptr)
    {
        std::vector<GPUTLAS::InstanceData> Instances;
        for (const auto& Inst : m_TLASInstances)
            Instances.push_back({Inst.CustomId, Inst.CustomId, Inst.Mask});
        GPUTLAS::CreateInfo GPUTLASCI;
        GPUTLASCI.Factory        = BuildAttribs.pEngineFactory;
        GPUTLASCI.InstanceBuffer = m_InstanceBuffer;
        GPUTLASCI.Instances      = Instances.data();
        GPUTLASCI.Count          = static_cast<Uint32>(Instances.size());
        m_Stats.GPUTLASActive    = m_GPUTLAS.Initialize(pDevice, pContext, GPUTLASCI);
        if (m_Stats.GPUTLASActive)
        {
            m_Stats.GPUInstanceDataSize          = m_GPUTLAS.GetMemorySize();
            TLASAttribs.InstanceBufferIsPrebuilt = m_GPUTLAS.Prepare(pContext, m_SubInstanceTransformBuffer, m_InstanceBuffer);
            TLASAttribs.Update                   = true;
            pContext->BuildTLAS(TLASAttribs);
        }
        else
        {
            m_GPUTLAS = {};
            LOG_WARNING_MESSAGE("GPU TLAS initialization unavailable; using CPU instance preparation");
        }
    }

    m_Stats.BLASCount        = static_cast<Uint32>(m_BLASRecords.size());
    m_Stats.InstanceCount    = static_cast<Uint32>(m_TLASInstances.size());
    m_Stats.SubInstanceCount = static_cast<Uint32>(SubInstances.size());
    m_Stats.Built            = true;
    return true;
}

bool RTXPTAccelerationStructures::UploadSubInstanceTransforms(IDeviceContext* Context)
{
    if (!m_SubInstanceTransformBuffer || m_SubInstanceTransforms.empty()) return false;
    // Dynamic allocations belong to the consuming context and current frame.
    MapHelper<float4x4> Mapped{Context, m_SubInstanceTransformBuffer, MAP_WRITE, MAP_FLAG_DISCARD};
    if (!Mapped) return false;
    std::copy(m_SubInstanceTransforms.begin(), m_SubInstanceTransforms.end(), static_cast<float4x4*>(Mapped));
    return true;
}

bool RTXPTAccelerationStructures::UpdateTLAS(IDeviceContext* pContext, const RTXPTSceneGraphData& SceneData)
{
    if (!m_TLAS || !m_InstanceBuffer || !m_TLASScratch || m_TLASInstances.empty())
    {
        DEV_ERROR("RTXPT dynamic TLAS update requires built TLAS resources");
        return false;
    }

    for (const BLASRecord& Record : m_BLASRecords)
    {
        if (Record.pNode == nullptr || Record.pNode->Index < 0 ||
            Record.InstanceIndex >= m_TLASInstances.size() ||
            Record.InstanceIndex >= m_InstanceNames.size())
        {
            DEV_ERROR("RTXPT dynamic TLAS update has invalid instance data");
            return false;
        }

        if (Record.ModelInstanceId >= SceneData.ModelInstances.size())
        {
            DEV_ERROR("RTXPT dynamic TLAS update has an invalid model instance id");
            return false;
        }

        const RTXPTModelInstance& Instance = SceneData.ModelInstances[Record.ModelInstanceId];
        if (Instance.ModelAssetId >= SceneData.ModelAssets.size())
        {
            DEV_ERROR("RTXPT dynamic TLAS update has an invalid model asset id");
            return false;
        }

        const RTXPTModelAsset&       Asset      = SceneData.ModelAssets[Instance.ModelAssetId];
        const GLTF::ModelTransforms& Transforms = Instance.Transforms.NodeGlobalMatrices.empty() ?
            Asset.StaticTransforms :
            Instance.Transforms;

        const Uint32 NodeIndex = static_cast<Uint32>(Record.pNode->Index);
        if (NodeIndex >= Transforms.NodeGlobalMatrices.size())
        {
            DEV_ERROR("RTXPT dynamic TLAS update is missing node transforms");
            return false;
        }

        const float4x4& WorldTransform = Transforms.NodeGlobalMatrices[NodeIndex];
        if (!m_Stats.GPUTLASActive)
            m_TLASInstances[Record.InstanceIndex].Transform = ToInstanceMatrix(WorldTransform);
        m_TLASInstances[Record.InstanceIndex].InstanceName = m_InstanceNames[Record.InstanceIndex].c_str();

        if (Uint64{Record.SubInstanceBase} + Uint64{Record.GeometryCount} > m_SubInstanceTransforms.size())
        {
            DEV_ERROR("RTXPT dynamic TLAS update has invalid sub-instance transform data");
            return false;
        }
        for (Uint32 i = 0; i < Record.GeometryCount; ++i)
            m_SubInstanceTransforms[Record.SubInstanceBase + i] = WorldTransform;
    }

    if (!m_SubInstanceTransformBuffer || m_SubInstanceTransforms.empty())
    {
        DEV_ERROR("RTXPT dynamic TLAS update requires a sub-instance transform buffer");
        return false;
    }
    if (!UploadSubInstanceTransforms(pContext)) return false;

    BuildTLASAttribs TLASAttribs;
    TLASAttribs.pTLAS                        = m_TLAS;
    TLASAttribs.TLASTransitionMode           = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    TLASAttribs.BLASTransitionMode           = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    TLASAttribs.pInstances                   = m_TLASInstances.data();
    TLASAttribs.InstanceCount                = static_cast<Uint32>(m_TLASInstances.size());
    TLASAttribs.pInstanceBuffer              = m_InstanceBuffer;
    TLASAttribs.InstanceBufferTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    TLASAttribs.HitGroupStride               = kRTXPTHitGroupStride;
    TLASAttribs.BindingMode                  = HIT_GROUP_BINDING_MODE_PER_GEOMETRY;
    TLASAttribs.pScratchBuffer               = m_TLASScratch;
    TLASAttribs.ScratchBufferTransitionMode  = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    TLASAttribs.Update                       = true;
    TLASAttribs.InstanceBufferIsPrebuilt     = m_Stats.GPUTLASActive && m_GPUTLAS.Prepare(pContext, m_SubInstanceTransformBuffer, m_InstanceBuffer);
    pContext->BuildTLAS(TLASAttribs);
    return true;
}

bool RTXPTAccelerationStructures::UpdateDynamicBLAS(IDeviceContext*                  pContext,
                                                    const RTXPTSceneGraphData&       SceneData,
                                                    const RTXPTSkinnedSceneGeometry& SkinnedGeometry)
{
    if (pContext == nullptr)
    {
        DEV_ERROR("RTXPT dynamic BLAS update requires a device context");
        return false;
    }

    if (!IsBuilt())
    {
        DEV_ERROR("RTXPT dynamic BLAS update requires built acceleration structures");
        return false;
    }

    if (!SkinnedGeometry.IsReady())
    {
        DEV_ERROR("RTXPT skinned geometry is not ready for dynamic BLAS update");
        return false;
    }

    if (SkinnedGeometry.HasSkinnedGeometry() && SkinnedGeometry.GetStats().DispatchCount == 0)
    {
        DEV_ERROR("RTXPT skinned geometry has not been dispatched for this frame");
        return false;
    }

    const Uint32 SkinningDispatchCount = SkinnedGeometry.GetStats().DispatchCount;

    bool Updated = false;
    for (BLASRecord& Record : m_BLASRecords)
    {
        if (!Record.Dynamic || !Record.BLAS)
            continue;

        if (Record.VertexBuffer.RawPtr() != SkinnedGeometry.GetSkinnedVertexBuffer())
        {
            DEV_ERROR("RTXPT dynamic BLAS was built with a different skinned vertex buffer");
            return false;
        }

        if (Record.SkinningDispatchCount == SkinningDispatchCount)
        {
            DEV_ERROR("RTXPT dynamic BLAS update requires a newer skinned geometry dispatch");
            return false;
        }

        BuildBLASAttribs BLASAttribs;
        BLASAttribs.pBLAS                       = Record.BLAS;
        BLASAttribs.BLASTransitionMode          = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        BLASAttribs.GeometryTransitionMode      = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        BLASAttribs.pTriangleData               = Record.TriangleData.data();
        BLASAttribs.TriangleDataCount           = static_cast<Uint32>(Record.TriangleData.size());
        BLASAttribs.pScratchBuffer              = m_BLASScratch;
        BLASAttribs.ScratchBufferTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        BLASAttribs.Update                      = true;
        pContext->BuildBLAS(BLASAttribs);
        Record.SkinningDispatchCount = SkinningDispatchCount;
        Updated                      = true;
    }

    return Updated && UpdateTLAS(pContext, SceneData);
}

} // namespace Diligent
