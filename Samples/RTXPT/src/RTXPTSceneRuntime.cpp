/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "RTXPTSceneRuntime.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace Diligent
{

namespace
{

// Bounds that keep every count and offset of the frame builder within Uint32 arithmetic.
constexpr Uint32 kMaxGeometryVertices = 1u << 28;
constexpr Uint32 kMaxGeometryIndices  = 1u << 30;
constexpr Uint32 kMaxMeshPrimitives   = 1u << 16;
constexpr Uint32 kMaxMaterialSlots    = 1u << 16;
constexpr Uint32 kMaxJoints           = 1u << 16;
constexpr Uint32 kMaxMaterialTextures = 64;
constexpr Uint32 kMaxInstances        = 1u << 22;
constexpr Uint32 kMaxLights           = 1u << 20;

bool IsFinite(float Value)
{
    return std::isfinite(Value);
}

bool IsFinite(const float4x4& Matrix)
{
    for (Uint32 Row = 0; Row < 4; ++Row)
        for (Uint32 Column = 0; Column < 4; ++Column)
            if (!IsFinite(Matrix.m[Row][Column]))
                return false;
    return true;
}

bool IsFinite(const RTXPTGeometryVertex& Vertex)
{
    return IsFinite(Vertex.position.x) && IsFinite(Vertex.position.y) && IsFinite(Vertex.position.z) &&
        IsFinite(Vertex.normal.x) && IsFinite(Vertex.normal.y) && IsFinite(Vertex.normal.z) &&
        IsFinite(Vertex.texCoord0.x) && IsFinite(Vertex.texCoord0.y);
}

bool IsValidSkinVertex(const RTXPTSkinVertex& Vertex, Uint32 JointCount)
{
    for (Uint32 i = 0; i < 4; ++i)
    {
        const float Joint  = Vertex.joints[i];
        const float Weight = Vertex.weights[i];
        if (!IsFinite(Joint) || !IsFinite(Weight) || Weight < 0.0f)
            return false;
        // Zero weights are skipped by the skinning shader, so their joints are never read.
        if (Weight > 0.0f && (Joint < 0.0f || Joint + 0.5f >= static_cast<float>(JointCount)))
            return false;
    }
    return true;
}

bool IsSameMatrix(const float4x4& A, const float4x4& B)
{
    return std::memcmp(&A, &B, sizeof(float4x4)) == 0;
}

// Whether a GPU source range of Count elements of Stride bytes lies in pBuffer.
bool IsValidSourceRange(const IBuffer* pBuffer, Uint64 Offset, Uint32 Count, Uint32 Stride)
{
    if (pBuffer == nullptr || Offset % sizeof(Uint32) != 0)
        return false;
    const Uint64 Size = pBuffer->GetDesc().Size;
    return Offset <= Size && Uint64{Count} * Stride <= Size - Offset;
}

template <typename HandleType>
HandleType MakeHandle(Uint32 Slot, Uint32 Generation)
{
    HandleType Handle;
    Handle.Slot       = Slot;
    Handle.Generation = Generation;
    return Handle;
}

Uint32 NextGeneration(Uint32 Generation)
{
    return Generation == std::numeric_limits<Uint32>::max() ? 1u : Generation + 1u;
}

// Slots of shared objects addressed by handles. A freed slot keeps the generation of its last occupant, so the next
// occupant gets a different one.
template <typename T>
struct RTXPTSlotTable
{
    struct Entry
    {
        std::shared_ptr<T> Object;
        Uint32             Generation = 0;
    };
    std::vector<Entry>  Entries;
    std::vector<Uint32> FreeSlots;

    RTXPTSceneHandle Add(std::shared_ptr<T> Object)
    {
        Uint32 Slot = 0;
        if (!FreeSlots.empty())
        {
            Slot = FreeSlots.back();
            FreeSlots.pop_back();
        }
        else
        {
            Slot = static_cast<Uint32>(Entries.size());
            Entries.emplace_back();
        }
        Entry& E     = Entries[Slot];
        E.Object     = std::move(Object);
        E.Generation = NextGeneration(E.Generation);
        return MakeHandle<RTXPTSceneHandle>(Slot, E.Generation);
    }

    const std::shared_ptr<T>* Find(const RTXPTSceneHandle& Handle) const
    {
        if (!Handle.IsValid() || Handle.Slot >= Entries.size())
            return nullptr;
        const Entry& E = Entries[Handle.Slot];
        return E.Object && E.Generation == Handle.Generation ? &E.Object : nullptr;
    }

    bool Remove(const RTXPTSceneHandle& Handle)
    {
        if (Find(Handle) == nullptr)
            return false;
        FreeSlots.push_back(Handle.Slot);
        Entries[Handle.Slot].Object.reset();
        return true;
    }

    Uint32 Size() const { return static_cast<Uint32>(Entries.size() - FreeSlots.size()); }
};

} // namespace

const char* GetRTXPTSceneResultString(RTXPTSceneResult Result)
{
    switch (Result)
    {
        case RTXPTSceneResult::Ok: return "ok";
        case RTXPTSceneResult::InvalidArgument: return "invalid argument";
        case RTXPTSceneResult::InvalidHandle: return "invalid handle";
        case RTXPTSceneResult::InvalidPose: return "invalid pose";
        case RTXPTSceneResult::InvalidState: return "invalid state";
        case RTXPTSceneResult::Unsupported: return "unsupported";
        case RTXPTSceneResult::LimitExceeded: return "limit exceeded";
        case RTXPTSceneResult::OutOfMemory: return "out of memory";
    }
    return "unknown result";
}

const char* GetRTXPTErrorCodeString(RTXPTErrorCode Code)
{
    switch (Code)
    {
        case RTXPTErrorCode::Ok: return "ok";
        case RTXPTErrorCode::InvalidArgument: return "a required argument is missing or not ready";
        case RTXPTErrorCode::OutOfMemory: return "out of memory";
        case RTXPTErrorCode::NotInitialized: return "it is not initialized";
        case RTXPTErrorCode::UpdateNotOpen: return "the scene import requires an open runtime update";
        case RTXPTErrorCode::UnsupportedVertexLayout: return "the scene import requires 32-byte vertices and 32-bit indices";
        case RTXPTErrorCode::ModelBuffersInvalid: return "a model has no vertex or skin buffer, or buffers smaller than their base offsets";
        case RTXPTErrorCode::GeometryRejected: return "the runtime scene rejected a model geometry";
        case RTXPTErrorCode::MaterialTextureMissing: return "a material references a missing texture";
        case RTXPTErrorCode::MaterialRejected: return "the runtime scene rejected a material";
        case RTXPTErrorCode::MaterialReferenceMissing: return "a model references a missing material";
        case RTXPTErrorCode::JointMatricesMissing: return "a skinned node is missing its joint matrices";
        case RTXPTErrorCode::InstanceRejected: return "the runtime scene rejected an instance";
        case RTXPTErrorCode::LightsRejected: return "the runtime scene rejected the scene lights";
        case RTXPTErrorCode::TransformUpdateFailed: return "an instance transform update failed";
        case RTXPTErrorCode::DeviceFeaturesMissing: return "RTXPT requires ray tracing, bindless resources, compute and standalone RT shaders";
        case RTXPTErrorCode::SkinnedGeometryUnsupported: return "the embedded renderer supports static geometry only";
        case RTXPTErrorCode::UpdateFailed: return "the scene import could not be opened or committed";
        case RTXPTErrorCode::EnvironmentFailed: return "the scene environment failed to load or bake";
        case RTXPTErrorCode::PlaceholderTextureFailed: return "the placeholder material texture could not be created";
        case RTXPTErrorCode::SceneBuffersFailed: return "the scene buffers could not be created";
        case RTXPTErrorCode::ArenaAllocationFailed: return "a geometry arena allocation failed";
        case RTXPTErrorCode::BindlessTableExceeded: return "material textures exceed the bindless table";
        case RTXPTErrorCode::SkinningPassFailed: return "the skinning pass could not be compiled";
        case RTXPTErrorCode::JointPaletteFailed: return "the joint palette could not be uploaded";
        case RTXPTErrorCode::SkinningBindingsFailed: return "the skinning resources could not be bound";
        case RTXPTErrorCode::SkinningDispatchFailed: return "a skinning dispatch failed";
        case RTXPTErrorCode::BLASBuildFailed: return "a BLAS build failed";
        case RTXPTErrorCode::SubInstanceLimitExceeded: return "sub-instances exceed the 24-bit TLAS CustomId range";
        case RTXPTErrorCode::EmissiveTriangleLimitExceeded: return "the emissive triangle count exceeds Uint32";
        case RTXPTErrorCode::SceneTablesFailed: return "the scene tables or TLAS could not be created";
        case RTXPTErrorCode::LightBufferFailed: return "the light buffer could not be created";
        case RTXPTErrorCode::EmissiveTriangleBufferFailed: return "the emissive triangle buffer could not be created";
        case RTXPTErrorCode::EmissivePassFailed: return "the emissive triangle pass could not be compiled";
        case RTXPTErrorCode::EmissiveBuildFailed: return "the emissive triangle build failed";
        case RTXPTErrorCode::FrameTargetsFailed: return "the frame targets could not be created";
        case RTXPTErrorCode::LightSamplingSetupFailed: return "the light sampling preparation failed";
        case RTXPTErrorCode::RayTracingSetupFailed: return "the ray tracing shaders or bindings failed";
        case RTXPTErrorCode::PostProcessSetupFailed: return "the accumulation or tone mapping initialization failed";
        case RTXPTErrorCode::FrameNotPrepared: return "the scene frame is not prepared";
        case RTXPTErrorCode::SceneBindingsFailed: return "the light sampling or scene bindings failed";
        case RTXPTErrorCode::ConstantsUpdateFailed: return "the frame constants could not be updated";
        case RTXPTErrorCode::TraceFailed: return "the light sampling or path tracing dispatch failed";
        case RTXPTErrorCode::AccumulationFailed: return "the accumulation failed";
        case RTXPTErrorCode::ToneMappingFailed: return "the tone mapping failed";
        case RTXPTErrorCode::UnsupportedViewConfiguration: return "embedded views require a supported mode, equal render/display sizes and an sRGB RGBA8 output RTV";
        case RTXPTErrorCode::GuidesFailed: return "realtime guides failed";
        case RTXPTErrorCode::RealtimeMergeFailed: return "realtime layer merge failed";
    }
    return "unknown error";
}

bool IsValidRTXPTInstanceTransform(const float4x4& Transform)
{
    if (!IsFinite(Transform) || Transform._14 != 0.0f || Transform._24 != 0.0f || Transform._34 != 0.0f || Transform._44 != 1.0f)
        return false;
    const double Determinant =
        double{Transform._11} * (double{Transform._22} * Transform._33 - double{Transform._23} * Transform._32) -
        double{Transform._12} * (double{Transform._21} * Transform._33 - double{Transform._23} * Transform._31) +
        double{Transform._13} * (double{Transform._21} * Transform._32 - double{Transform._22} * Transform._31);
    return std::isfinite(Determinant) && Determinant != 0.0;
}

struct RTXPTSceneRuntime::Impl
{
    // The instance table of a scene version, indexed by slot. A freed slot keeps its last generation.
    struct SceneState
    {
        std::vector<std::shared_ptr<const RTXPTSceneInstanceState>> Instances;
        std::vector<Uint32>                                         Generations;
        std::vector<Uint32>                                         FreeSlots;
        std::shared_ptr<const std::vector<RTXPTSceneLight>>         Lights;
        Uint64                                                      LightRevision = 0;
        Uint32                                                      LiveCount     = 0;
    };

    RTXPTSlotTable<const RTXPTGeometryAsset> Geometries;
    RTXPTSlotTable<const RTXPTMaterialAsset> Materials;
    SceneState                               Committed;
    SceneState                               Pending;
    RTXPTSceneSnapshotPtr                    Snapshot;
    bool                                     bUpdating       = false;
    bool                                     bContentChanged = false;
    Uint64                                   NextAssetId     = 1;
    Uint64                                   NextRevision    = 1;
    Uint64                                   NextSnapshotId  = 1;
    Uint64                                   ContentRevision = 0;

    template <typename F>
    static RTXPTSceneResult Guard(F&& Function)
    {
        try
        {
            return Function();
        }
        catch (const std::bad_alloc&)
        {
            return RTXPTSceneResult::OutOfMemory;
        }
    }

    // The pending state of a live instance, or nullptr.
    std::shared_ptr<const RTXPTSceneInstanceState> FindInstance(const RTXPTSceneHandle& Handle) const
    {
        if (!Handle.IsValid() || Handle.Slot >= Pending.Instances.size())
            return nullptr;
        const std::shared_ptr<const RTXPTSceneInstanceState>& pState = Pending.Instances[Handle.Slot];
        return pState && Pending.Generations[Handle.Slot] == Handle.Generation ? pState : nullptr;
    }

    // Instance generations come from one counter that a cancelled update does not roll back, so a handle created by
    // a cancelled update never matches a later occupant of its slot.
    Uint32 AllocateInstanceGeneration()
    {
        const Uint32 Generation = NextInstanceGeneration;
        NextInstanceGeneration  = NextGeneration(NextInstanceGeneration);
        return Generation;
    }

    Uint64 NewRevision()
    {
        return NextRevision++;
    }

    static RTXPTSceneResult ValidatePose(const RTXPTGeometryAsset& Geometry, const float4x4* pPose, Uint32 MatrixCount)
    {
        if (!Geometry.IsSkinned())
            return pPose == nullptr && MatrixCount == 0 ? RTXPTSceneResult::Ok : RTXPTSceneResult::InvalidPose;
        if (pPose == nullptr || MatrixCount != Geometry.JointCount)
            return RTXPTSceneResult::InvalidPose;
        for (Uint32 i = 0; i < MatrixCount; ++i)
        {
            if (!IsFinite(pPose[i]))
                return RTXPTSceneResult::InvalidPose;
        }
        return RTXPTSceneResult::Ok;
    }

    static bool IsSamePose(const std::vector<float4x4>& Pose, const float4x4* pPose, Uint32 MatrixCount)
    {
        if (Pose.size() != MatrixCount)
            return false;
        return MatrixCount == 0 || std::memcmp(Pose.data(), pPose, sizeof(float4x4) * MatrixCount) == 0;
    }

    Uint32 NextInstanceGeneration = 1;
};

namespace
{

RTXPTSceneResult ValidateGeometryMeshes(const RTXPTGeometryDesc& Desc)
{
    if (Desc.pMeshes == nullptr || Desc.MeshCount == 0)
        return RTXPTSceneResult::InvalidArgument;
    for (Uint32 MeshIndex = 0; MeshIndex < Desc.MeshCount; ++MeshIndex)
    {
        const RTXPTGeometryMeshDesc& Mesh = Desc.pMeshes[MeshIndex];
        if (Mesh.pPrimitives == nullptr || Mesh.PrimitiveCount == 0)
            return RTXPTSceneResult::InvalidArgument;
        if (Mesh.PrimitiveCount > kMaxMeshPrimitives)
            return RTXPTSceneResult::LimitExceeded;
        for (Uint32 i = 0; i < Mesh.PrimitiveCount; ++i)
        {
            const RTXPTGeometryPrimitiveDesc& Primitive = Mesh.pPrimitives[i];
            if (Primitive.MaterialSlot >= Desc.MaterialSlotCount)
                return RTXPTSceneResult::InvalidArgument;
            if (Primitive.IndexCount > 0)
            {
                if (Primitive.IndexCount % 3 != 0 || Uint64{Primitive.FirstIndex} + Primitive.IndexCount > Desc.IndexCount)
                    return RTXPTSceneResult::InvalidArgument;
            }
            else if (Primitive.VertexCount == 0 || Primitive.VertexCount % 3 != 0 ||
                     Uint64{Primitive.FirstVertex} + Primitive.VertexCount > Desc.VertexCount)
            {
                return RTXPTSceneResult::InvalidArgument;
            }
        }
    }
    return RTXPTSceneResult::Ok;
}

// Exactly one source per stream; streams the geometry does not use have none.
RTXPTSceneResult ValidateGeometrySources(const RTXPTGeometryDesc& Desc)
{
    const bool bSkinned = Desc.JointCount != 0;
    if ((Desc.pVertices != nullptr) == (Desc.pSourceVertexBuffer != nullptr))
        return RTXPTSceneResult::InvalidArgument;
    if (Desc.IndexCount > 0 ? (Desc.pIndices != nullptr) == (Desc.pSourceIndexBuffer != nullptr) :
                              (Desc.pIndices != nullptr || Desc.pSourceIndexBuffer != nullptr))
        return RTXPTSceneResult::InvalidArgument;
    if (bSkinned ? (Desc.pSkin != nullptr) == (Desc.pSourceSkinBuffer != nullptr) :
                   (Desc.pSkin != nullptr || Desc.pSourceSkinBuffer != nullptr || Desc.pNormalSkin != nullptr))
        return RTXPTSceneResult::InvalidArgument;

    if (Desc.pSourceVertexBuffer != nullptr &&
        !IsValidSourceRange(Desc.pSourceVertexBuffer, Desc.SourceVertexOffset, Desc.VertexCount, sizeof(RTXPTGeometryVertex)))
        return RTXPTSceneResult::InvalidArgument;
    if (Desc.pSourceIndexBuffer != nullptr &&
        !IsValidSourceRange(Desc.pSourceIndexBuffer, Desc.SourceIndexOffset, Desc.IndexCount, sizeof(Uint32)))
        return RTXPTSceneResult::InvalidArgument;
    if (Desc.pSourceSkinBuffer != nullptr &&
        !IsValidSourceRange(Desc.pSourceSkinBuffer, Desc.SourceSkinOffset, Desc.VertexCount, sizeof(RTXPTSkinVertex)))
        return RTXPTSceneResult::InvalidArgument;
    return RTXPTSceneResult::Ok;
}

// CPU data is checked here; GPU sources are trusted to hold valid data of the declared size.
RTXPTSceneResult ValidateGeometryData(const RTXPTGeometryDesc& Desc)
{
    if (Desc.pVertices != nullptr)
    {
        for (Uint32 i = 0; i < Desc.VertexCount; ++i)
        {
            if (!IsFinite(Desc.pVertices[i]))
                return RTXPTSceneResult::InvalidArgument;
        }
    }
    if (Desc.pIndices != nullptr)
    {
        for (Uint32 i = 0; i < Desc.IndexCount; ++i)
        {
            if (Desc.pIndices[i] >= Desc.VertexCount)
                return RTXPTSceneResult::InvalidArgument;
        }
    }
    for (const RTXPTSkinVertex* pSkin : {Desc.pSkin, Desc.pNormalSkin})
    {
        if (pSkin == nullptr)
            continue;
        for (Uint32 i = 0; i < Desc.VertexCount; ++i)
        {
            if (!IsValidSkinVertex(pSkin[i], Desc.JointCount))
                return RTXPTSceneResult::InvalidArgument;
        }
    }
    return RTXPTSceneResult::Ok;
}

RTXPTSceneResult ValidateGeometryDesc(const RTXPTGeometryDesc& Desc)
{
    if (Desc.VertexCount == 0 || Desc.MaterialSlotCount == 0)
        return RTXPTSceneResult::InvalidArgument;
    if (Desc.VertexCount > kMaxGeometryVertices || Desc.IndexCount > kMaxGeometryIndices ||
        Desc.MaterialSlotCount > kMaxMaterialSlots || Desc.JointCount > kMaxJoints)
        return RTXPTSceneResult::LimitExceeded;
    RTXPTSceneResult Result = ValidateGeometrySources(Desc);
    if (Result == RTXPTSceneResult::Ok)
        Result = ValidateGeometryMeshes(Desc);
    if (Result == RTXPTSceneResult::Ok)
        Result = ValidateGeometryData(Desc);
    return Result;
}

RTXPTSceneResult ValidateMaterialDesc(const RTXPTMaterialDesc& Desc)
{
    if (Desc.TextureCount > kMaxMaterialTextures)
        return RTXPTSceneResult::LimitExceeded;
    if (Desc.TextureCount > 0 && Desc.ppTextures == nullptr)
        return RTXPTSceneResult::InvalidArgument;
    for (Uint32 i = 0; i < Desc.TextureCount; ++i)
    {
        if (Desc.ppTextures[i] == nullptr)
            return RTXPTSceneResult::InvalidArgument;
    }
    if (!IsFinite(Desc.EmissionMagnitude) || Desc.EmissionMagnitude < 0.0f)
        return RTXPTSceneResult::InvalidArgument;

    // Every texture the flags enable must name one of the material's views.
    const MaterialPTData& Data = Desc.Data;
    const struct
    {
        Uint32 Flag;
        Uint32 Index;
    } TextureRefs[] = {
        {kMaterialFlag_HasBaseColorTexture, Data.baseColorTextureIndex},
        {kMaterialFlag_HasEmissiveTexture, Data.emissiveTextureIndex},
        {kMaterialFlag_HasMetallicRoughnessTexture, Data.metallicRoughnessTextureIndex},
        {kMaterialFlag_HasNormalTexture, Data.normalTextureIndex},
        {kMaterialFlag_HasTransmissionTexture, Data.transmissionTextureIndex},
        {kMaterialFlag_HasThicknessTexture, Data.thicknessTextureIndex},
    };
    for (const auto& Ref : TextureRefs)
    {
        if ((Data.flags & Ref.Flag) != 0u && Ref.Index >= Desc.TextureCount)
            return RTXPTSceneResult::InvalidArgument;
    }
    return RTXPTSceneResult::Ok;
}

} // namespace

RTXPTSceneRuntime::RTXPTSceneRuntime() :
    m_Impl{std::make_unique<Impl>()}
{
    auto pSnapshot           = std::make_shared<RTXPTSceneSnapshot>();
    pSnapshot->Lights        = std::make_shared<const std::vector<RTXPTSceneLight>>();
    m_Impl->Committed.Lights = pSnapshot->Lights;
    m_Impl->Snapshot         = std::move(pSnapshot);
}

RTXPTSceneRuntime::~RTXPTSceneRuntime() = default;

RTXPTSceneResult RTXPTSceneRuntime::RegisterGeometry(const RTXPTGeometryDesc& Desc, RTXPTGeometryHandle& Handle)
{
    Handle                        = {};
    const RTXPTSceneResult Result = ValidateGeometryDesc(Desc);
    if (Result != RTXPTSceneResult::Ok)
        return Result;

    return Impl::Guard([&]() {
        auto pAsset               = std::make_shared<RTXPTGeometryAsset>();
        pAsset->Name              = Desc.Name != nullptr ? Desc.Name : "";
        pAsset->VertexCount       = Desc.VertexCount;
        pAsset->IndexCount        = Desc.IndexCount;
        pAsset->MaterialSlotCount = Desc.MaterialSlotCount;
        pAsset->JointCount        = Desc.JointCount;
        if (Desc.pVertices != nullptr)
            pAsset->Vertices.assign(Desc.pVertices, Desc.pVertices + Desc.VertexCount);
        if (Desc.pIndices != nullptr)
            pAsset->Indices.assign(Desc.pIndices, Desc.pIndices + Desc.IndexCount);
        if (Desc.pSkin != nullptr)
            pAsset->Skin.assign(Desc.pSkin, Desc.pSkin + Desc.VertexCount);
        if (Desc.pNormalSkin != nullptr)
            pAsset->NormalSkin.assign(Desc.pNormalSkin, Desc.pNormalSkin + Desc.VertexCount);
        pAsset->pSourceVertexBuffer = Desc.pSourceVertexBuffer;
        pAsset->SourceVertexOffset  = Desc.SourceVertexOffset;
        pAsset->pSourceIndexBuffer  = Desc.pSourceIndexBuffer;
        pAsset->SourceIndexOffset   = Desc.SourceIndexOffset;
        pAsset->pSourceSkinBuffer   = Desc.pSourceSkinBuffer;
        pAsset->SourceSkinOffset    = Desc.SourceSkinOffset;
        pAsset->Meshes.resize(Desc.MeshCount);
        for (Uint32 i = 0; i < Desc.MeshCount; ++i)
        {
            const RTXPTGeometryMeshDesc& Mesh = Desc.pMeshes[i];
            pAsset->Meshes[i].Primitives.assign(Mesh.pPrimitives, Mesh.pPrimitives + Mesh.PrimitiveCount);
        }
        pAsset->AssetId = m_Impl->NextAssetId++;

        const RTXPTSceneHandle Added = m_Impl->Geometries.Add(std::move(pAsset));
        Handle.Slot                  = Added.Slot;
        Handle.Generation            = Added.Generation;
        return RTXPTSceneResult::Ok;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::RegisterMaterial(const RTXPTMaterialDesc& Desc, RTXPTMaterialHandle& Handle)
{
    Handle                        = {};
    const RTXPTSceneResult Result = ValidateMaterialDesc(Desc);
    if (Result != RTXPTSceneResult::Ok)
        return Result;

    return Impl::Guard([&]() {
        auto pAsset               = std::make_shared<RTXPTMaterialAsset>();
        pAsset->Name              = Desc.Name != nullptr ? Desc.Name : "";
        pAsset->Data              = Desc.Data;
        pAsset->EmissionMagnitude = Desc.EmissionMagnitude;
        pAsset->Textures.reserve(Desc.TextureCount);
        for (Uint32 i = 0; i < Desc.TextureCount; ++i)
            pAsset->Textures.emplace_back(Desc.ppTextures[i]);
        pAsset->AssetId = m_Impl->NextAssetId++;

        const RTXPTSceneHandle Added = m_Impl->Materials.Add(std::move(pAsset));
        Handle.Slot                  = Added.Slot;
        Handle.Generation            = Added.Generation;
        return RTXPTSceneResult::Ok;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::ReleaseGeometry(RTXPTGeometryHandle Handle)
{
    return Impl::Guard([&]() {
        return m_Impl->Geometries.Remove(Handle) ? RTXPTSceneResult::Ok : RTXPTSceneResult::InvalidHandle;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::ReleaseMaterial(RTXPTMaterialHandle Handle)
{
    return Impl::Guard([&]() {
        return m_Impl->Materials.Remove(Handle) ? RTXPTSceneResult::Ok : RTXPTSceneResult::InvalidHandle;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::GetGeometryInfo(RTXPTGeometryHandle Handle, Uint32& MeshCount, Uint32& MaterialSlotCount, Uint32& JointCount) const
{
    MeshCount                                                   = 0;
    MaterialSlotCount                                           = 0;
    JointCount                                                  = 0;
    const std::shared_ptr<const RTXPTGeometryAsset>* ppGeometry = m_Impl->Geometries.Find(Handle);
    if (ppGeometry == nullptr)
        return RTXPTSceneResult::InvalidHandle;
    MeshCount         = static_cast<Uint32>((*ppGeometry)->Meshes.size());
    MaterialSlotCount = (*ppGeometry)->MaterialSlotCount;
    JointCount        = (*ppGeometry)->JointCount;
    return RTXPTSceneResult::Ok;
}

RTXPTSceneResult RTXPTSceneRuntime::BeginUpdate()
{
    if (m_Impl->bUpdating)
        return RTXPTSceneResult::InvalidState;
    return Impl::Guard([&]() {
        Impl::SceneState Pending = m_Impl->Committed;
        m_Impl->Pending          = std::move(Pending);
        m_Impl->bUpdating        = true;
        m_Impl->bContentChanged  = false;
        return RTXPTSceneResult::Ok;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::CreateInstance(const RTXPTInstanceDesc& Desc, RTXPTInstanceHandle& Handle)
{
    Handle  = {};
    Impl& I = *m_Impl;
    if (!I.bUpdating)
        return RTXPTSceneResult::InvalidState;

    const std::shared_ptr<const RTXPTGeometryAsset>* ppGeometry = I.Geometries.Find(Desc.Geometry);
    if (ppGeometry == nullptr)
        return RTXPTSceneResult::InvalidHandle;
    const RTXPTGeometryAsset& Geometry = **ppGeometry;
    if (Desc.MeshIndex >= Geometry.Meshes.size() || !IsValidRTXPTInstanceTransform(Desc.Transform))
        return RTXPTSceneResult::InvalidArgument;
    if (Desc.MaterialCount != Geometry.MaterialSlotCount || Desc.pMaterials == nullptr)
        return RTXPTSceneResult::InvalidArgument;
    for (Uint32 i = 0; i < Desc.MaterialCount; ++i)
    {
        if (I.Materials.Find(Desc.pMaterials[i]) == nullptr)
            return RTXPTSceneResult::InvalidHandle;
    }
    const RTXPTSceneResult PoseResult = Impl::ValidatePose(Geometry, Desc.pPose, Desc.PoseMatrixCount);
    if (PoseResult != RTXPTSceneResult::Ok)
        return PoseResult;
    if (I.Pending.LiveCount >= kMaxInstances)
        return RTXPTSceneResult::LimitExceeded;

    return Impl::Guard([&]() {
        auto pState       = std::make_shared<RTXPTSceneInstanceState>();
        pState->Geometry  = *ppGeometry;
        pState->MeshIndex = Desc.MeshIndex;
        pState->Transform = Desc.Transform;
        pState->Materials.reserve(Desc.MaterialCount);
        for (Uint32 i = 0; i < Desc.MaterialCount; ++i)
            pState->Materials.push_back(*I.Materials.Find(Desc.pMaterials[i]));
        if (Desc.pPose != nullptr)
            pState->Pose = std::make_shared<const std::vector<float4x4>>(Desc.pPose, Desc.pPose + Desc.PoseMatrixCount);
        pState->Enabled = Desc.Enabled;
        pState->UserKey = Desc.UserKey;

        // Reserve first, so that nothing below throws once the pending state changes.
        Impl::SceneState& Pending = I.Pending;
        Uint32            Slot    = 0;
        if (Pending.FreeSlots.empty())
        {
            Pending.Instances.reserve(Pending.Instances.size() + 1);
            Pending.Generations.reserve(Pending.Generations.size() + 1);
            Slot = static_cast<Uint32>(Pending.Instances.size());
            Pending.Instances.emplace_back();
            Pending.Generations.push_back(0);
        }
        else
        {
            Slot = Pending.FreeSlots.back();
            Pending.FreeSlots.pop_back();
        }

        const Uint32 Generation   = I.AllocateInstanceGeneration();
        pState->Handle.Slot       = Slot;
        pState->Handle.Generation = Generation;
        pState->TransformRevision = I.NewRevision();
        pState->PoseRevision      = I.NewRevision();
        pState->MaterialRevision  = I.NewRevision();

        Pending.Generations[Slot] = Generation;
        Pending.Instances[Slot]   = std::move(pState);
        ++Pending.LiveCount;
        if (Desc.Enabled)
            I.bContentChanged = true;

        Handle.Slot       = Slot;
        Handle.Generation = Generation;
        return RTXPTSceneResult::Ok;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::SetInstanceTransform(RTXPTInstanceHandle Handle, const float4x4& Transform)
{
    Impl& I = *m_Impl;
    if (!I.bUpdating)
        return RTXPTSceneResult::InvalidState;
    const std::shared_ptr<const RTXPTSceneInstanceState> pCurrent = I.FindInstance(Handle);
    if (!pCurrent)
        return RTXPTSceneResult::InvalidHandle;
    if (!IsValidRTXPTInstanceTransform(Transform))
        return RTXPTSceneResult::InvalidArgument;
    if (IsSameMatrix(pCurrent->Transform, Transform))
        return RTXPTSceneResult::Ok;

    return Impl::Guard([&]() {
        auto pState                      = std::make_shared<RTXPTSceneInstanceState>(*pCurrent);
        pState->Transform                = Transform;
        pState->TransformRevision        = I.NewRevision();
        I.Pending.Instances[Handle.Slot] = std::move(pState);
        if (pCurrent->Enabled)
            I.bContentChanged = true;
        return RTXPTSceneResult::Ok;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::SetInstancePose(RTXPTInstanceHandle Handle, const float4x4* pPose, Uint32 MatrixCount)
{
    Impl& I = *m_Impl;
    if (!I.bUpdating)
        return RTXPTSceneResult::InvalidState;
    const std::shared_ptr<const RTXPTSceneInstanceState> pCurrent = I.FindInstance(Handle);
    if (!pCurrent)
        return RTXPTSceneResult::InvalidHandle;
    const RTXPTSceneResult PoseResult = Impl::ValidatePose(*pCurrent->Geometry, pPose, MatrixCount);
    if (PoseResult != RTXPTSceneResult::Ok)
        return PoseResult;
    // Rigid instances accept only the empty pose, which is what they already have.
    if (!pCurrent->Pose || Impl::IsSamePose(*pCurrent->Pose, pPose, MatrixCount))
        return RTXPTSceneResult::Ok;

    return Impl::Guard([&]() {
        auto pState                      = std::make_shared<RTXPTSceneInstanceState>(*pCurrent);
        pState->Pose                     = std::make_shared<const std::vector<float4x4>>(pPose, pPose + MatrixCount);
        pState->PoseRevision             = I.NewRevision();
        I.Pending.Instances[Handle.Slot] = std::move(pState);
        if (pCurrent->Enabled)
            I.bContentChanged = true;
        return RTXPTSceneResult::Ok;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::SetInstanceMaterial(RTXPTInstanceHandle Handle, Uint32 Slot, RTXPTMaterialHandle Material)
{
    Impl& I = *m_Impl;
    if (!I.bUpdating)
        return RTXPTSceneResult::InvalidState;
    const std::shared_ptr<const RTXPTSceneInstanceState> pCurrent = I.FindInstance(Handle);
    if (!pCurrent)
        return RTXPTSceneResult::InvalidHandle;
    if (Slot >= pCurrent->Materials.size())
        return RTXPTSceneResult::InvalidArgument;
    const std::shared_ptr<const RTXPTMaterialAsset>* ppMaterial = I.Materials.Find(Material);
    if (ppMaterial == nullptr)
        return RTXPTSceneResult::InvalidHandle;
    if (pCurrent->Materials[Slot] == *ppMaterial)
        return RTXPTSceneResult::Ok;

    return Impl::Guard([&]() {
        auto pState                      = std::make_shared<RTXPTSceneInstanceState>(*pCurrent);
        pState->Materials[Slot]          = *ppMaterial;
        pState->MaterialRevision         = I.NewRevision();
        I.Pending.Instances[Handle.Slot] = std::move(pState);
        if (pCurrent->Enabled)
            I.bContentChanged = true;
        return RTXPTSceneResult::Ok;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::SetInstanceEnabled(RTXPTInstanceHandle Handle, bool Enabled)
{
    Impl& I = *m_Impl;
    if (!I.bUpdating)
        return RTXPTSceneResult::InvalidState;
    const std::shared_ptr<const RTXPTSceneInstanceState> pCurrent = I.FindInstance(Handle);
    if (!pCurrent)
        return RTXPTSceneResult::InvalidHandle;
    if (pCurrent->Enabled == Enabled)
        return RTXPTSceneResult::Ok;

    return Impl::Guard([&]() {
        auto pState                      = std::make_shared<RTXPTSceneInstanceState>(*pCurrent);
        pState->Enabled                  = Enabled;
        I.Pending.Instances[Handle.Slot] = std::move(pState);
        I.bContentChanged                = true;
        return RTXPTSceneResult::Ok;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::RemoveInstance(RTXPTInstanceHandle Handle)
{
    Impl& I = *m_Impl;
    if (!I.bUpdating)
        return RTXPTSceneResult::InvalidState;
    const std::shared_ptr<const RTXPTSceneInstanceState> pCurrent = I.FindInstance(Handle);
    if (!pCurrent)
        return RTXPTSceneResult::InvalidHandle;

    return Impl::Guard([&]() {
        I.Pending.FreeSlots.push_back(Handle.Slot);
        I.Pending.Instances[Handle.Slot].reset();
        --I.Pending.LiveCount;
        if (pCurrent->Enabled)
            I.bContentChanged = true;
        return RTXPTSceneResult::Ok;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::SetLights(const RTXPTSceneLight* pLights, Uint32 LightCount)
{
    Impl& I = *m_Impl;
    if (!I.bUpdating)
        return RTXPTSceneResult::InvalidState;
    if (LightCount > 0 && pLights == nullptr)
        return RTXPTSceneResult::InvalidArgument;
    if (LightCount > kMaxLights)
        return RTXPTSceneResult::LimitExceeded;

    const std::vector<RTXPTSceneLight>& Current = *I.Pending.Lights;
    bool                                bSame   = Current.size() == LightCount;
    for (Uint32 i = 0; bSame && i < LightCount; ++i)
    {
        bSame = std::memcmp(&Current[i].Light, &pLights[i].Light, sizeof(PolymorphicLightInfoFull)) == 0 &&
            Current[i].Key == pLights[i].Key;
    }
    if (bSame)
        return RTXPTSceneResult::Ok;

    return Impl::Guard([&]() {
        I.Pending.Lights        = std::make_shared<const std::vector<RTXPTSceneLight>>(pLights, pLights + LightCount);
        I.Pending.LightRevision = I.NewRevision();
        I.bContentChanged       = true;
        return RTXPTSceneResult::Ok;
    });
}

RTXPTSceneResult RTXPTSceneRuntime::CommitUpdate(RTXPTSceneSnapshotPtr& Snapshot)
{
    Impl& I = *m_Impl;
    if (!I.bUpdating)
        return RTXPTSceneResult::InvalidState;

    // A failed commit keeps the update open, so the host can retry or cancel it.
    return Impl::Guard([&]() {
        auto pSnapshot             = std::make_shared<RTXPTSceneSnapshot>();
        pSnapshot->SnapshotId      = I.NextSnapshotId;
        pSnapshot->ContentRevision = I.bContentChanged ? I.NextRevision : I.ContentRevision;
        pSnapshot->LightRevision   = I.Pending.LightRevision;
        pSnapshot->Lights          = I.Pending.Lights;
        pSnapshot->Instances.reserve(I.Pending.LiveCount);
        for (const std::shared_ptr<const RTXPTSceneInstanceState>& pState : I.Pending.Instances)
        {
            if (pState)
                pSnapshot->Instances.push_back(pState);
        }

        if (I.bContentChanged)
            I.ContentRevision = I.NewRevision();
        ++I.NextSnapshotId;
        I.Committed       = std::move(I.Pending);
        I.Pending         = {};
        I.Snapshot        = std::move(pSnapshot);
        I.bUpdating       = false;
        I.bContentChanged = false;
        Snapshot          = I.Snapshot;
        return RTXPTSceneResult::Ok;
    });
}

void RTXPTSceneRuntime::CancelUpdate()
{
    m_Impl->Pending         = {};
    m_Impl->bUpdating       = false;
    m_Impl->bContentChanged = false;
}

bool RTXPTSceneRuntime::IsUpdating() const
{
    return m_Impl->bUpdating;
}

RTXPTSceneSnapshotPtr RTXPTSceneRuntime::GetSnapshot() const
{
    return m_Impl->Snapshot;
}

} // namespace Diligent
