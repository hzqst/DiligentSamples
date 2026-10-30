/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "RTXPTSceneImporter.hpp"

#include <algorithm>

#include "DebugUtilities.hpp"
#include "RTXPTLights.hpp"
#include "RTXPTMaterials.hpp"
#include "RTXPTScene.hpp"

namespace Diligent
{

namespace
{

const GLTF::ModelTransforms& GetInstanceTransforms(const RTXPTModelAsset& Asset, const RTXPTModelInstance& Instance)
{
    return Instance.Transforms.NodeGlobalMatrices.empty() ? Asset.StaticTransforms : Instance.Transforms;
}

// The geometry of a model: all its vertices, indices and meshes. With pSkin, the vertices are skinned by the joints of
// pSkin, whose influences are vertex buffer 1 (JOINTS_0 + WEIGHTS_0, indexing the joints of the skin).
RTXPTErrorCode RegisterModelGeometry(IRenderDevice* pDevice, IDeviceContext* pContext, const RTXPTModelAsset& Asset, const GLTF::Skin* pSkin, RTXPTSceneRuntime& Runtime, RTXPTGeometryHandle& Handle, std::vector<Int32>& MeshIndices)
{
    const GLTF::Model& Model         = *Asset.Model;
    IBuffer*           pVertexBuffer = Model.GetVertexBuffer(0, pDevice, pContext);
    IBuffer*           pIndexBuffer  = Model.GetIndexBuffer(pDevice, pContext);
    if (pVertexBuffer == nullptr)
    {
        LOG_ERROR_MESSAGE("RTXPT model has no vertex buffer: ", Asset.ModelName);
        return RTXPTErrorCode::ModelBuffersInvalid;
    }
    const Uint32 BaseVertex       = Model.GetBaseVertex();
    const Uint64 ModelVertexCount = pVertexBuffer->GetDesc().Size / sizeof(RTXPTGeometryVertex);
    const Uint32 FirstIndex       = pIndexBuffer != nullptr ? Model.GetFirstIndexLocation() : 0u;
    const Uint64 ModelIndexCount  = pIndexBuffer != nullptr ? pIndexBuffer->GetDesc().Size / sizeof(Uint32) : 0u;
    if (ModelVertexCount <= BaseVertex || ModelIndexCount < FirstIndex)
    {
        LOG_ERROR_MESSAGE("RTXPT model buffers are smaller than their base offsets: ", Asset.ModelName);
        return RTXPTErrorCode::ModelBuffersInvalid;
    }

    // One geometry mesh per glTF mesh with triangles; nodes that share a mesh share its BLAS.
    std::vector<std::vector<RTXPTGeometryPrimitiveDesc>> Primitives;
    MeshIndices.assign(Model.Meshes.size(), -1);
    const Uint32 SlotCount = std::max<Uint32>(static_cast<Uint32>(Model.Materials.size()), 1u);
    for (size_t MeshIdx = 0; MeshIdx < Model.Meshes.size(); ++MeshIdx)
    {
        std::vector<RTXPTGeometryPrimitiveDesc> MeshPrimitives;
        for (const GLTF::Primitive& Primitive : Model.Meshes[MeshIdx].Primitives)
        {
            if (Primitive.VertexCount == 0 && Primitive.IndexCount == 0)
                continue;
            RTXPTGeometryPrimitiveDesc Desc;
            // The glTF builder bakes VertexStart into indexed primitives; their indices address the model vertices.
            if (Primitive.HasIndices())
            {
                Desc.FirstIndex = Primitive.FirstIndex;
                Desc.IndexCount = Primitive.IndexCount;
            }
            else
            {
                Desc.FirstVertex = Primitive.FirstVertex;
                Desc.VertexCount = Primitive.VertexCount;
            }
            Desc.MaterialSlot = Primitive.MaterialId < SlotCount ? Primitive.MaterialId : 0u;
            MeshPrimitives.push_back(Desc);
        }
        if (MeshPrimitives.empty())
            continue;
        MeshIndices[MeshIdx] = static_cast<Int32>(Primitives.size());
        Primitives.push_back(std::move(MeshPrimitives));
    }
    if (Primitives.empty())
        return RTXPTErrorCode::Ok;

    std::vector<RTXPTGeometryMeshDesc> Meshes(Primitives.size());
    for (size_t i = 0; i < Primitives.size(); ++i)
        Meshes[i] = {Primitives[i].data(), static_cast<Uint32>(Primitives[i].size())};

    RTXPTGeometryDesc Desc;
    Desc.Name                = Asset.ModelName.c_str();
    Desc.VertexCount         = static_cast<Uint32>(std::min<Uint64>(ModelVertexCount - BaseVertex, ~Uint32{0}));
    Desc.IndexCount          = static_cast<Uint32>(std::min<Uint64>(ModelIndexCount - FirstIndex, ~Uint32{0}));
    Desc.pSourceVertexBuffer = pVertexBuffer;
    Desc.SourceVertexOffset  = Uint64{BaseVertex} * sizeof(RTXPTGeometryVertex);
    Desc.pSourceIndexBuffer  = Desc.IndexCount != 0 ? pIndexBuffer : nullptr;
    Desc.SourceIndexOffset   = Uint64{FirstIndex} * sizeof(Uint32);
    Desc.pMeshes             = Meshes.data();
    Desc.MeshCount           = static_cast<Uint32>(Meshes.size());
    Desc.MaterialSlotCount   = SlotCount;
    if (pSkin != nullptr)
    {
        IBuffer* pSkinBuffer = Model.GetVertexBufferCount() > 1 ? Model.GetVertexBuffer(1, pDevice, pContext) : nullptr;
        if (pSkinBuffer == nullptr)
        {
            LOG_ERROR_MESSAGE("RTXPT skinned model has no skin buffer: ", Asset.ModelName);
            return RTXPTErrorCode::ModelBuffersInvalid;
        }
        Desc.pSourceSkinBuffer = pSkinBuffer;
        Desc.SourceSkinOffset  = Uint64{BaseVertex} * sizeof(RTXPTSkinVertex);
        Desc.JointCount        = static_cast<Uint32>(pSkin->Joints.size());
    }
    const RTXPTSceneResult Result = Runtime.RegisterGeometry(Desc, Handle);
    if (Result != RTXPTSceneResult::Ok)
    {
        LOG_ERROR_MESSAGE("RTXPT geometry of ", Asset.ModelName, " was rejected: ", GetRTXPTSceneResultString(Result));
        return RTXPTErrorCode::GeometryRejected;
    }
    return RTXPTErrorCode::Ok;
}

// The material records index the shared bindless table of RTXPTMaterials; a runtime material lists its own views.
RTXPTErrorCode RegisterMaterial(const RTXPTMaterials& Materials, Uint32 Row, RTXPTSceneRuntime& Runtime, RTXPTMaterialHandle& Handle)
{
    MaterialPTData                   Data = Materials.GetMaterialData()[Row];
    std::vector<ITextureView*>       Views;
    const std::pair<Uint32, Uint32*> TextureRefs[] = {
        {kMaterialFlag_HasBaseColorTexture, &Data.baseColorTextureIndex},
        {kMaterialFlag_HasEmissiveTexture, &Data.emissiveTextureIndex},
        {kMaterialFlag_HasMetallicRoughnessTexture, &Data.metallicRoughnessTextureIndex},
        {kMaterialFlag_HasNormalTexture, &Data.normalTextureIndex},
        {kMaterialFlag_HasTransmissionTexture, &Data.transmissionTextureIndex},
        {kMaterialFlag_HasThicknessTexture, &Data.thicknessTextureIndex},
    };
    for (const auto& Ref : TextureRefs)
    {
        if ((Data.flags & Ref.first) == 0u)
        {
            *Ref.second = 0;
            continue;
        }
        ITextureView* pView = Materials.GetTextureView(*Ref.second);
        if (pView == nullptr)
        {
            LOG_ERROR_MESSAGE("RTXPT material ", Row, " references a missing texture");
            return RTXPTErrorCode::MaterialTextureMissing;
        }
        const auto It = std::find(Views.begin(), Views.end(), pView);
        *Ref.second   = static_cast<Uint32>(It - Views.begin());
        if (It == Views.end())
            Views.push_back(pView);
    }

    const std::string Name = "material " + std::to_string(Row);
    RTXPTMaterialDesc Desc;
    Desc.Name              = Name.c_str();
    Desc.Data              = Data;
    Desc.ppTextures        = Views.data();
    Desc.TextureCount      = static_cast<Uint32>(Views.size());
    Desc.EmissionMagnitude = std::max(Data.emissiveFactor.x, std::max(Data.emissiveFactor.y, Data.emissiveFactor.z));
    if (!(Desc.EmissionMagnitude >= 0.0f))
        Desc.EmissionMagnitude = 0.0f;
    const RTXPTSceneResult Result = Runtime.RegisterMaterial(Desc, Handle);
    if (Result != RTXPTSceneResult::Ok)
    {
        LOG_ERROR_MESSAGE("RTXPT ", Name, " was rejected: ", GetRTXPTSceneResultString(Result));
        return RTXPTErrorCode::MaterialRejected;
    }
    return RTXPTErrorCode::Ok;
}

} // namespace

RTXPTErrorCode ImportRTXPTScene(IRenderDevice*        pDevice,
                                IDeviceContext*       pContext,
                                const RTXPTScene&     Scene,
                                const RTXPTMaterials& Materials,
                                RTXPTSceneRuntime&    Runtime,
                                RTXPTImportedScene&   Imported)
{
    Imported = {};
    if (!Runtime.IsUpdating())
        return RTXPTErrorCode::UpdateNotOpen;
    if (Scene.GetVertexStride0() != sizeof(RTXPTGeometryVertex) || Scene.GetIndexType() != VT_UINT32)
        return RTXPTErrorCode::UnsupportedVertexLayout;

    const RTXPTSceneGraphData& Graph = Scene.GetSceneGraphData();
    auto                       Fail  = [&](RTXPTErrorCode Code) {
        ReleaseRTXPTImportedScene(Runtime, Imported);
        Imported.Instances.clear();
        return Code;
    };

    Imported.Geometries.resize(Graph.ModelAssets.size());
    Imported.MeshIndices.resize(Graph.ModelAssets.size());
    Imported.SkinnedGeometries.resize(Graph.ModelAssets.size());
    for (size_t AssetIdx = 0; AssetIdx < Graph.ModelAssets.size(); ++AssetIdx)
    {
        const RTXPTModelAsset& Asset = Graph.ModelAssets[AssetIdx];
        if (Asset.Model)
        {
            const RTXPTErrorCode Code = RegisterModelGeometry(pDevice, pContext, Asset, nullptr, Runtime, Imported.Geometries[AssetIdx], Imported.MeshIndices[AssetIdx]);
            if (Code != RTXPTErrorCode::Ok)
                return Fail(Code);
        }
        Imported.SkinnedGeometries[AssetIdx].resize(Asset.Model ? Asset.Model->Skins.size() : 0u);
    }

    const Uint32 MaterialCount = static_cast<Uint32>(Materials.GetMaterialData().size());
    Imported.Materials.resize(MaterialCount);
    for (Uint32 Row = 0; Row < MaterialCount; ++Row)
    {
        const RTXPTErrorCode Code = RegisterMaterial(Materials, Row, Runtime, Imported.Materials[Row]);
        if (Code != RTXPTErrorCode::Ok)
            return Fail(Code);
    }

    std::vector<RTXPTMaterialHandle> Binding;
    for (RTXPTSceneId InstanceId = 0; InstanceId < Graph.ModelInstances.size(); ++InstanceId)
    {
        const RTXPTModelInstance& Instance = Graph.ModelInstances[InstanceId];
        if (Instance.ModelAssetId >= Graph.ModelAssets.size() || !Imported.Geometries[Instance.ModelAssetId].IsValid())
            continue;
        const RTXPTModelAsset& Asset = Graph.ModelAssets[Instance.ModelAssetId];
        const GLTF::Model&     Model = *Asset.Model;
        if (Asset.SceneIndex >= Model.Scenes.size())
            continue;

        // Material slot i of the geometry is glTF material i of the model.
        Binding.assign(std::max<size_t>(Model.Materials.size(), 1u), RTXPTMaterialHandle{});
        for (size_t Slot = 0; Slot < Binding.size(); ++Slot)
        {
            const Uint32 Row = Slot < Asset.MaterialRemap.size() ? Asset.MaterialRemap[Slot] : 0u;
            if (Row >= Imported.Materials.size())
            {
                LOG_ERROR_MESSAGE("RTXPT model ", Asset.ModelName, " references a missing material");
                return Fail(RTXPTErrorCode::MaterialReferenceMissing);
            }
            Binding[Slot] = Imported.Materials[Row];
        }

        const GLTF::ModelTransforms& Transforms = GetInstanceTransforms(Asset, Instance);
        for (const GLTF::Node* pNode : Model.Scenes[Asset.SceneIndex].LinearNodes)
        {
            if (pNode == nullptr || pNode->pMesh == nullptr || pNode->Index < 0 ||
                static_cast<size_t>(pNode->Index) >= Transforms.NodeGlobalMatrices.size())
                continue;
            const Int32 MeshIndex = Imported.MeshIndices[Instance.ModelAssetId][pNode->pMesh - Model.Meshes.data()];
            if (MeshIndex < 0)
                continue;
            RTXPTGeometryHandle Geometry = Imported.Geometries[Instance.ModelAssetId];
            const float4x4*     pPose    = nullptr;
            Uint32              Joints   = 0;
            if (pNode->pSkin != nullptr)
            {
                // Skinned nodes share one skinned geometry per skin of the model.
                const size_t         SkinIndex = static_cast<size_t>(pNode->pSkin - Model.Skins.data());
                RTXPTGeometryHandle& Skinned   = Imported.SkinnedGeometries[Instance.ModelAssetId][SkinIndex];
                std::vector<Int32>   SkinnedMeshIndices;
                if (!Skinned.IsValid())
                {
                    const RTXPTErrorCode Code = RegisterModelGeometry(pDevice, pContext, Asset, pNode->pSkin, Runtime, Skinned, SkinnedMeshIndices);
                    if (Code != RTXPTErrorCode::Ok)
                        return Fail(Code);
                }
                Joints = static_cast<Uint32>(pNode->pSkin->Joints.size());
                if (pNode->SkinTransformsIndex < 0 || static_cast<size_t>(pNode->SkinTransformsIndex) >= Transforms.Skins.size() ||
                    Transforms.Skins[pNode->SkinTransformsIndex].JointMatrices.size() < Joints)
                {
                    LOG_ERROR_MESSAGE("RTXPT skinned node is missing its joint matrices: ", Asset.ModelName);
                    return Fail(RTXPTErrorCode::JointMatricesMissing);
                }
                Geometry = Skinned;
                pPose    = Transforms.Skins[pNode->SkinTransformsIndex].JointMatrices.data();
            }

            // Degenerate node matrices (for example zero scale) hide their nodes.
            const float4x4&   Transform = Transforms.NodeGlobalMatrices[pNode->Index];
            RTXPTInstanceDesc Desc;
            Desc.Geometry        = Geometry;
            Desc.pPose           = pPose;
            Desc.PoseMatrixCount = Joints;
            Desc.MeshIndex       = static_cast<Uint32>(MeshIndex);
            Desc.Enabled         = IsValidRTXPTInstanceTransform(Transform);
            Desc.Transform       = Desc.Enabled ? Transform : float4x4::Identity();
            Desc.pMaterials      = Binding.data();
            Desc.MaterialCount   = static_cast<Uint32>(Binding.size());
            Desc.UserKey         = (Uint64{Instance.GraphNodeId} << 32u) | static_cast<Uint32>(pNode->Index);

            RTXPTImportedSceneInstance Created;
            Created.ModelInstanceId       = InstanceId;
            Created.NodeIndex             = pNode->Index;
            Created.SkinTransformsIndex   = pNode->pSkin != nullptr ? pNode->SkinTransformsIndex : -1;
            Created.JointCount            = Joints;
            const RTXPTSceneResult Result = Runtime.CreateInstance(Desc, Created.Handle);
            if (Result != RTXPTSceneResult::Ok)
            {
                LOG_ERROR_MESSAGE("RTXPT instance of ", Asset.ModelName, " was rejected: ", GetRTXPTSceneResultString(Result));
                return Fail(RTXPTErrorCode::InstanceRejected);
            }
            Imported.Instances.push_back(Created);
        }
    }

    std::vector<RTXPTSceneLight> Lights;
    BuildRTXPTSceneLights(Graph, Lights);
    const RTXPTSceneResult Result = Runtime.SetLights(Lights.data(), static_cast<Uint32>(Lights.size()));
    if (Result != RTXPTSceneResult::Ok)
    {
        LOG_ERROR_MESSAGE("RTXPT scene lights were rejected: ", GetRTXPTSceneResultString(Result));
        return Fail(RTXPTErrorCode::LightsRejected);
    }
    return RTXPTErrorCode::Ok;
}

RTXPTErrorCode UpdateRTXPTImportedSceneTransforms(const RTXPTScene& Scene, const RTXPTImportedScene& Imported, RTXPTSceneRuntime& Runtime)
{
    const RTXPTSceneGraphData& Graph = Scene.GetSceneGraphData();
    for (const RTXPTImportedSceneInstance& Instance : Imported.Instances)
    {
        const RTXPTModelInstance&    ModelInstance = Graph.ModelInstances[Instance.ModelInstanceId];
        const GLTF::ModelTransforms& Transforms    = GetInstanceTransforms(Graph.ModelAssets[ModelInstance.ModelAssetId], ModelInstance);
        const float4x4&              Transform     = Transforms.NodeGlobalMatrices[Instance.NodeIndex];
        const bool                   Visible       = IsValidRTXPTInstanceTransform(Transform);
        RTXPTSceneResult             Result        = Runtime.SetInstanceEnabled(Instance.Handle, Visible);
        if (Result == RTXPTSceneResult::Ok && Visible)
            Result = Runtime.SetInstanceTransform(Instance.Handle, Transform);
        if (Result == RTXPTSceneResult::Ok && Instance.SkinTransformsIndex >= 0)
        {
            const std::vector<float4x4>& Pose = Transforms.Skins[Instance.SkinTransformsIndex].JointMatrices;
            Result                            = Runtime.SetInstancePose(Instance.Handle, Pose.data(), Instance.JointCount);
        }
        if (Result != RTXPTSceneResult::Ok)
        {
            LOG_ERROR_MESSAGE("RTXPT instance transform update failed: ", GetRTXPTSceneResultString(Result));
            return RTXPTErrorCode::TransformUpdateFailed;
        }
    }
    return RTXPTErrorCode::Ok;
}

void ReleaseRTXPTImportedScene(RTXPTSceneRuntime& Runtime, RTXPTImportedScene& Imported)
{
    for (const RTXPTGeometryHandle& Handle : Imported.Geometries)
    {
        if (Handle.IsValid())
            Runtime.ReleaseGeometry(Handle);
    }
    for (const std::vector<RTXPTGeometryHandle>& Skinned : Imported.SkinnedGeometries)
    {
        for (const RTXPTGeometryHandle& Handle : Skinned)
        {
            if (Handle.IsValid())
                Runtime.ReleaseGeometry(Handle);
        }
    }
    for (const RTXPTMaterialHandle& Handle : Imported.Materials)
    {
        if (Handle.IsValid())
            Runtime.ReleaseMaterial(Handle);
    }
    Imported.Geometries.clear();
    Imported.SkinnedGeometries.clear();
    Imported.MeshIndices.clear();
    Imported.Materials.clear();
}

} // namespace Diligent
