/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "RTXPTSceneFrame.hpp"
#include "RTXPTGpuProfiler.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BottomLevelAS.h"
#include "DebugUtilities.hpp"
#include "GraphicsAccessories.hpp"
#include "RefCntAutoPtr.hpp"
#include "Texture.h"
#include "TextureView.h"
#include "AsyncPrefix.hpp"
#include "RTXPTSubInstanceData.hpp"
#include "RTXPTEmissiveTrianglePass.hpp"
#include "RTXPTSkinnedGeometry.hpp"
#include "RTXPTSkinnedVertexHistory.hpp"

namespace Diligent
{

namespace
{

constexpr Uint32 kMaxSubInstanceCount     = 1u << 24; // TLAS CustomId has 24 bits.
constexpr Uint32 kHitGroupStride          = 2;        // Keep in sync with RTXPT_HIT_GROUP_STRIDE in PathTracer/Config.h.
constexpr Uint32 kMinArenaVertices        = 1u << 16;
constexpr Uint32 kMinArenaIndices         = 1u << 18;
constexpr Uint32 kMinArenaSkinnedVertices = 1u << 14;
constexpr Uint32 kMinJointMatrices        = 256;
constexpr Uint32 kMinMaterialRows         = 256;
constexpr Uint32 kMinTLASInstances        = 64;
constexpr Uint32 kMinSubInstances         = 256;
constexpr Uint64 kBLASRetainPrepares      = 120; // Unused BLASes survive this many prepares, so toggled instances reuse them.
constexpr Uint32 kTextureTableSizes[]     = {256, 1024, 4096};
constexpr Uint32 kGeometryVertexStride    = sizeof(RTXPTGeometryVertex);

Uint32 NextCapacity(Uint32 Current, Uint32 Required, Uint32 Minimum)
{
    Uint64 Capacity = std::max(Current, Minimum);
    while (Capacity < Required)
        Capacity *= 2;
    return static_cast<Uint32>(std::min<Uint64>(Capacity, std::numeric_limits<Uint32>::max()));
}

InstanceMatrix ToInstanceMatrix(const float4x4& Transform)
{
    InstanceMatrix Matrix;
    Matrix.SetRotation(&Transform._11, 4);
    Matrix.SetTranslation(Transform._41, Transform._42, Transform._43);
    return Matrix;
}

// First-fit allocator of element ranges with coalescing free ranges.
class RangeAllocator
{
public:
    Uint32 GetCapacity() const { return m_Capacity; }

    bool Allocate(Uint32 Count, Uint32& Offset)
    {
        for (auto It = m_Free.begin(); It != m_Free.end(); ++It)
        {
            if (It->second < Count)
                continue;
            Offset                 = It->first;
            const Uint32 Remainder = It->second - Count;
            m_Free.erase(It);
            if (Remainder != 0)
                m_Free.emplace(Offset + Count, Remainder);
            return true;
        }
        return false;
    }

    void Free(Uint32 Offset, Uint32 Count)
    {
        if (Count == 0)
            return;
        auto Next = m_Free.lower_bound(Offset);
        if (Next != m_Free.begin())
        {
            auto Prev = std::prev(Next);
            if (Prev->first + Prev->second == Offset)
            {
                Offset = Prev->first;
                Count += Prev->second;
                m_Free.erase(Prev);
            }
        }
        if (Next != m_Free.end() && Offset + Count == Next->first)
        {
            Count += Next->second;
            m_Free.erase(Next);
        }
        m_Free.emplace(Offset, Count);
    }

    void Grow(Uint32 Capacity)
    {
        if (Capacity > m_Capacity)
            Free(m_Capacity, Capacity - m_Capacity);
        m_Capacity = std::max(m_Capacity, Capacity);
    }

    void Clear()
    {
        m_Free.clear();
        m_Capacity = 0;
    }

private:
    std::map<Uint32, Uint32> m_Free;
    Uint32                   m_Capacity = 0;
};

BufferDesc MakeBufferDesc(const char* Name, Uint64 Size, BIND_FLAGS BindFlags, BUFFER_MODE Mode, Uint32 Stride)
{
    BufferDesc Desc;
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    Desc.Name                 = Name;
    Desc.Usage                = USAGE_DEFAULT;
    Desc.BindFlags            = BindFlags;
    Desc.Mode                 = Mode;
    Desc.ElementByteStride    = Mode != BUFFER_MODE_UNDEFINED ? Stride : 0;
    Desc.Size                 = Size;
    return Desc;
}

// A device buffer of Stride-byte elements whose ranges are suballocated. Growing copies the contents into a larger
// buffer, so the ranges keep their offsets.
struct BufferArena
{
    const char*            Name      = nullptr;
    BIND_FLAGS             BindFlags = BIND_NONE;
    BUFFER_MODE            Mode      = BUFFER_MODE_UNDEFINED;
    Uint32                 Stride    = 0;
    Uint32                 Minimum   = 0;
    RefCntAutoPtr<IBuffer> Buffer;
    RangeAllocator         Ranges;

    void Clear()
    {
        Buffer.Release();
        Ranges.Clear();
    }

    // Creates the buffer at its minimum capacity, so it can be bound before anything is allocated.
    bool Reserve(IRenderDevice* pDevice)
    {
        if (Buffer)
            return true;
        pDevice->CreateBuffer(MakeBufferDesc(Name, Uint64{Minimum} * Stride, BindFlags, Mode, Stride), nullptr, &Buffer);
        if (!Buffer)
            return false;
        Ranges.Grow(Minimum);
        return true;
    }

    // Allocates Count elements. Sets Grown when the buffer object changed.
    bool Allocate(IRenderDevice* pDevice, IDeviceContext* pContext, Uint32 Count, Uint32& Offset, bool& Grown)
    {
        if (Count == 0)
        {
            Offset = 0;
            return true;
        }
        if (Ranges.Allocate(Count, Offset))
            return true;

        const Uint32 OldCapacity = Ranges.GetCapacity();
        const Uint64 Required    = Uint64{OldCapacity} + Count;
        if (Required > std::numeric_limits<Uint32>::max())
            return false;
        const Uint32           Capacity = NextCapacity(OldCapacity, static_cast<Uint32>(Required), Minimum);
        RefCntAutoPtr<IBuffer> NewBuffer;
        pDevice->CreateBuffer(MakeBufferDesc(Name, Uint64{Capacity} * Stride, BindFlags, Mode, Stride), nullptr, &NewBuffer);
        if (!NewBuffer)
            return false;
        if (Buffer && OldCapacity != 0)
        {
            pContext->CopyBuffer(Buffer, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, NewBuffer, 0, Uint64{OldCapacity} * Stride,
                                 RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        Buffer = std::move(NewBuffer);
        Ranges.Grow(Capacity);
        Grown = true;
        return Ranges.Allocate(Count, Offset);
    }
};

// A device buffer rewritten with CPU contents. It is recreated when the contents outgrow it and written only when they
// change.
template <typename T>
struct UploadBuffer
{
    const char*            Name      = nullptr;
    BIND_FLAGS             BindFlags = BIND_SHADER_RESOURCE;
    Uint32                 Minimum   = 0;
    RefCntAutoPtr<IBuffer> Buffer;
    std::vector<T>         Uploaded;

    void Clear()
    {
        Buffer.Release();
        Uploaded.clear();
    }

    // Sets Recreated when the buffer object changed.
    bool Upload(IRenderDevice* pDevice, IDeviceContext* pContext, const std::vector<T>& Data, bool& Recreated)
    {
        const Uint32 Count = std::max<Uint32>(static_cast<Uint32>(Data.size()), 1u);
        if (!Buffer || Buffer->GetDesc().Size < Uint64{Count} * sizeof(T))
        {
            const Uint32 OldCapacity = Buffer ? static_cast<Uint32>(Buffer->GetDesc().Size / sizeof(T)) : 0u;
            const Uint32 Capacity    = NextCapacity(OldCapacity, Count, Minimum);
            Buffer.Release();
            pDevice->CreateBuffer(MakeBufferDesc(Name, Uint64{Capacity} * sizeof(T), BindFlags, BUFFER_MODE_STRUCTURED, sizeof(T)), nullptr, &Buffer);
            if (!Buffer)
                return false;
            Uploaded.clear();
            Recreated = true;
        }
        if (!Data.empty() && (Uploaded.size() != Data.size() || std::memcmp(Uploaded.data(), Data.data(), Data.size() * sizeof(T)) != 0))
        {
            pContext->UpdateBuffer(Buffer, 0, Data.size() * sizeof(T), Data.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            Uploaded = Data;
        }
        return true;
    }
};

} // namespace

struct RTXPTSceneFrameBuilder::Impl
{
    struct GeometryResidency
    {
        std::weak_ptr<const RTXPTGeometryAsset> Asset;
        Uint32                                  VertexBase  = 0;
        Uint32                                  VertexCount = 0;
        Uint32                                  IndexBase   = 0;
        Uint32                                  IndexCount  = 0;
        // Skin influences of skinned geometry; NormalSkinBase equals SkinBase when normals follow the positions.
        Uint32 SkinBase        = 0;
        Uint32 SkinCount       = 0;
        Uint32 NormalSkinBase  = 0;
        Uint32 NormalSkinCount = 0;
    };

    // The skinned vertices and BLAS of one skinned instance.
    struct SkinnedInstance
    {
        Uint64                        GeometryId = 0;
        RTXPTSkinnedVertexHistory     Vertices;
        std::vector<Uint64>           AnyHitMask;
        RefCntAutoPtr<IBottomLevelAS> BLAS;
        // Survives a failed Prepare during collection, before any skin job can invalidate the old BLAS.
        bool   NeedsBLASRebuild = false;
        Uint64 LastUsed         = 0;
    };

    // A skinned instance of the snapshot being prepared, and the work it needs.
    struct SkinJob
    {
        SkinnedInstance*               pInstance = nullptr;
        const RTXPTSceneInstanceState* pState    = nullptr;
        const GeometryResidency*       pGeometry = nullptr;
        Uint32                         TLASIndex = 0;
        bool                           Reskin    = false;
    };

    struct MaterialResidency
    {
        std::weak_ptr<const RTXPTMaterialAsset> Asset;
        Uint32                                  Row = 0;
        std::vector<Uint32>                     TextureSlots;
    };

    // Rigid BLASes are shared by every instance of a mesh whose primitives need the same any-hit filtering.
    struct BLASKey
    {
        Uint64              GeometryId = 0;
        Uint32              MeshIndex  = 0;
        std::vector<Uint64> AnyHitMask;

        bool operator<(const BLASKey& Other) const
        {
            return std::tie(GeometryId, MeshIndex, AnyHitMask) < std::tie(Other.GeometryId, Other.MeshIndex, Other.AnyHitMask);
        }
    };

    struct BLASEntry
    {
        RefCntAutoPtr<IBottomLevelAS> BLAS;
        Uint64                        LastUsed = 0;
    };

    struct TextureSlot
    {
        RefCntAutoPtr<ITextureView> View;
        Uint32                      References = 0;
    };

    // The state of one emissive sub-instance; the emissive triangles are rebuilt when any of it changes.
    struct EmissiveState
    {
        Uint64 Handle            = 0;
        Uint64 TransformRevision = 0;
        Uint64 PoseRevision      = 0;
        Uint64 MaterialRevision  = 0;
        Uint32 SubInstance       = 0;

        bool operator==(const EmissiveState& Other) const
        {
            return Handle == Other.Handle && TransformRevision == Other.TransformRevision && PoseRevision == Other.PoseRevision &&
                MaterialRevision == Other.MaterialRevision && SubInstance == Other.SubInstance;
        }
    };

    RefCntAutoPtr<IRenderDevice>     Device;
    RTXPTSceneFrameBuilderCreateInfo CreateInfo;
    bool                             Initialized = false;

    BufferArena                                 Vertices;
    BufferArena                                 Indices;
    BufferArena                                 SkinData;
    BufferArena                                 SkinnedVertices;
    UploadBuffer<float4x4>                      JointMatrices;
    RTXPTSkinningPass                           Skinning;
    std::unordered_map<Uint64, SkinnedInstance> SkinnedInstances;
    RangeAllocator                              MaterialRows;
    RefCntAutoPtr<IBuffer>                      MaterialBuffer;
    std::vector<MaterialPTData>                 MaterialData;
    std::vector<Uint32>                         DirtyMaterialRows;
    UploadBuffer<SubInstanceData>               SubInstances;
    RTXPTSceneHistory                           History;
    UploadBuffer<float4x4>                      SubInstanceTransforms;

    std::unordered_map<Uint64, GeometryResidency> Geometries;
    std::unordered_map<Uint64, MaterialResidency> Materials;
    std::map<BLASKey, BLASEntry>                  BLASes;

    std::vector<TextureSlot>                        TextureSlots;
    std::unordered_map<const ITextureView*, Uint32> TextureSlotOfView;
    std::vector<Uint32>                             FreeTextureSlots;
    std::vector<IDeviceObject*>                     TextureTable;
    RefCntAutoPtr<ITexture>                         PlaceholderTexture;
    RefCntAutoPtr<IBottomLevelAS>                   PlaceholderBLAS;
    RefCntAutoPtr<IBuffer>                          PlaceholderVertices;

    RefCntAutoPtr<ITopLevelAS> TLAS;
    RefCntAutoPtr<IBuffer>     TLASInstanceBuffer;
    RefCntAutoPtr<IBuffer>     TLASScratch;
    RefCntAutoPtr<IBuffer>     BLASScratch;

    RTXPTLights                        Lights;
    RTXPTEmissiveTrianglePass          EmissivePass;
    Uint32                             EmissivePassTextureCount = 0;
    std::vector<EmissiveState>         EmissiveStates;
    std::vector<RTXPTLightSourceRange> EmissiveSources;
    Uint64                             LightRevision  = 0;
    bool                               LightsUploaded = false;
    bool                               EmissiveBuilt  = false;
    bool                               HasPrepared    = false;
    // Revisions stay monotonic across Reset, so users notice the new objects.
    Uint64 LastBindingRevision = 0;
    Uint64 LightingRevision    = 0;
    Uint64 EmissiveRevision    = 0;

    RTXPTSceneFrame      Frame;
    RTXPTSceneFrameStats Stats;

    bool CreatePlaceholderTexture()
    {
        TextureDesc Desc;
        Desc.Name                 = "RTXPT placeholder material texture";
        Desc.Type                 = RESOURCE_DIM_TEX_2D_ARRAY;
        Desc.Width                = 1;
        Desc.Height               = 1;
        Desc.ArraySize            = 1;
        Desc.MipLevels            = 1;
        Desc.Format               = TEX_FORMAT_RGBA8_UNORM;
        Desc.Usage                = USAGE_IMMUTABLE;
        Desc.BindFlags            = BIND_SHADER_RESOURCE;
        Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
        const Uint32      White   = 0xFFFFFFFFu;
        TextureSubResData Level{&White, sizeof(White)};
        TextureData       Data{&Level, 1};
        Device->CreateTexture(Desc, &Data, &PlaceholderTexture);
        return PlaceholderTexture != nullptr;
    }

    // Grows the bindless table to the smallest supported size that holds Required textures.
    bool ReserveTextureTable(Uint32 Required, bool& BindingsChanged)
    {
        if (Required <= TextureTable.size())
            return true;
        for (Uint32 Size : kTextureTableSizes)
        {
            if (Size < Required)
                continue;
            TextureTable.resize(Size, PlaceholderTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
            ++Stats.TextureTableGrowCount;
            BindingsChanged = true;
            return true;
        }
        return false;
    }

    bool AcquireTextureSlot(ITextureView* pView, Uint32& Slot, bool& BindingsChanged)
    {
        auto It = TextureSlotOfView.find(pView);
        if (It != TextureSlotOfView.end())
        {
            Slot = It->second;
            ++TextureSlots[Slot].References;
            return true;
        }
        if (FreeTextureSlots.empty())
        {
            if (!ReserveTextureTable(static_cast<Uint32>(TextureSlots.size()) + 1u, BindingsChanged))
                return false;
            Slot = static_cast<Uint32>(TextureSlots.size());
            TextureSlots.emplace_back();
        }
        else
        {
            Slot = FreeTextureSlots.back();
            FreeTextureSlots.pop_back();
        }
        TextureSlots[Slot].View       = pView;
        TextureSlots[Slot].References = 1;
        TextureSlotOfView.emplace(pView, Slot);
        TextureTable[Slot] = pView;
        BindingsChanged    = true;
        return true;
    }

    void ReleaseTextureSlot(Uint32 Slot, bool& BindingsChanged)
    {
        TextureSlot& Entry = TextureSlots[Slot];
        if (--Entry.References != 0)
            return;
        TextureSlotOfView.erase(Entry.View.RawPtr());
        Entry.View.Release();
        TextureTable[Slot] = PlaceholderTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        FreeTextureSlots.push_back(Slot);
        BindingsChanged = true;
    }

    RTXPTErrorCode MakeGeometryResident(IDeviceContext* pContext, const std::shared_ptr<const RTXPTGeometryAsset>& pAsset, const GeometryResidency*& pResidency, bool& BindingsChanged)
    {
        auto It = Geometries.find(pAsset->AssetId);
        if (It != Geometries.end())
        {
            pResidency = &It->second;
            return RTXPTErrorCode::Ok;
        }

        GeometryResidency Residency;
        Residency.Asset       = pAsset;
        Residency.VertexCount = pAsset->VertexCount;
        Residency.IndexCount  = pAsset->IndexCount;
        if (!Vertices.Allocate(Device, pContext, Residency.VertexCount, Residency.VertexBase, BindingsChanged))
        {
            LOG_ERROR_MESSAGE("RTXPT vertex arena allocation failed for ", pAsset->Name);
            return RTXPTErrorCode::ArenaAllocationFailed;
        }
        if (!Indices.Allocate(Device, pContext, Residency.IndexCount, Residency.IndexBase, BindingsChanged))
        {
            Vertices.Ranges.Free(Residency.VertexBase, Residency.VertexCount);
            LOG_ERROR_MESSAGE("RTXPT index arena allocation failed for ", pAsset->Name);
            return RTXPTErrorCode::ArenaAllocationFailed;
        }

        const Uint64 VertexBytes = Uint64{pAsset->VertexCount} * kGeometryVertexStride;
        const Uint64 IndexBytes  = Uint64{pAsset->IndexCount} * sizeof(Uint32);
        if (!pAsset->Vertices.empty())
        {
            pContext->UpdateBuffer(Vertices.Buffer, Uint64{Residency.VertexBase} * kGeometryVertexStride, VertexBytes, pAsset->Vertices.data(),
                                   RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        else
        {
            pContext->CopyBuffer(pAsset->pSourceVertexBuffer, pAsset->SourceVertexOffset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                 Vertices.Buffer, Uint64{Residency.VertexBase} * kGeometryVertexStride, VertexBytes,
                                 RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        if (pAsset->IndexCount != 0)
        {
            if (!pAsset->Indices.empty())
            {
                pContext->UpdateBuffer(Indices.Buffer, Uint64{Residency.IndexBase} * sizeof(Uint32), IndexBytes, pAsset->Indices.data(),
                                       RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            }
            else
            {
                pContext->CopyBuffer(pAsset->pSourceIndexBuffer, pAsset->SourceIndexOffset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                     Indices.Buffer, Uint64{Residency.IndexBase} * sizeof(Uint32), IndexBytes,
                                     RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            }
        }
        Stats.AssetUploadBytes += VertexBytes + IndexBytes;
        if (pAsset->IsSkinned())
        {
            const RTXPTErrorCode Code = UploadSkin(pContext, *pAsset, Residency, BindingsChanged);
            if (Code != RTXPTErrorCode::Ok)
            {
                Vertices.Ranges.Free(Residency.VertexBase, Residency.VertexCount);
                Indices.Ranges.Free(Residency.IndexBase, Residency.IndexCount);
                return Code;
            }
        }
        pResidency = &Geometries.emplace(pAsset->AssetId, std::move(Residency)).first->second;
        return RTXPTErrorCode::Ok;
    }

    RTXPTErrorCode UploadSkin(IDeviceContext* pContext, const RTXPTGeometryAsset& Asset, GeometryResidency& Residency, bool& BindingsChanged)
    {
        Residency.SkinCount       = Asset.VertexCount;
        Residency.NormalSkinCount = Asset.HasNormalSkin() ? Asset.VertexCount : 0u;
        if (!SkinData.Allocate(Device, pContext, Residency.SkinCount, Residency.SkinBase, BindingsChanged))
        {
            LOG_ERROR_MESSAGE("RTXPT skin arena allocation failed for ", Asset.Name);
            return RTXPTErrorCode::ArenaAllocationFailed;
        }
        if (!SkinData.Allocate(Device, pContext, Residency.NormalSkinCount, Residency.NormalSkinBase, BindingsChanged))
        {
            SkinData.Ranges.Free(Residency.SkinBase, Residency.SkinCount);
            LOG_ERROR_MESSAGE("RTXPT skin arena allocation failed for ", Asset.Name);
            return RTXPTErrorCode::ArenaAllocationFailed;
        }
        if (Residency.NormalSkinCount == 0)
            Residency.NormalSkinBase = Residency.SkinBase;

        const Uint64 SkinBytes = Uint64{Asset.VertexCount} * sizeof(RTXPTSkinVertex);
        if (!Asset.Skin.empty())
        {
            pContext->UpdateBuffer(SkinData.Buffer, Uint64{Residency.SkinBase} * sizeof(RTXPTSkinVertex), SkinBytes, Asset.Skin.data(),
                                   RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        else
        {
            pContext->CopyBuffer(Asset.pSourceSkinBuffer, Asset.SourceSkinOffset, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                 SkinData.Buffer, Uint64{Residency.SkinBase} * sizeof(RTXPTSkinVertex), SkinBytes,
                                 RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        if (Residency.NormalSkinCount != 0)
        {
            pContext->UpdateBuffer(SkinData.Buffer, Uint64{Residency.NormalSkinBase} * sizeof(RTXPTSkinVertex), SkinBytes, Asset.NormalSkin.data(),
                                   RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        Stats.AssetUploadBytes += SkinBytes * (Residency.NormalSkinCount != 0 ? 2u : 1u);
        return RTXPTErrorCode::Ok;
    }

    RTXPTErrorCode MakeMaterialResident(const std::shared_ptr<const RTXPTMaterialAsset>& pAsset, const MaterialResidency*& pResidency, bool& BindingsChanged)
    {
        auto It = Materials.find(pAsset->AssetId);
        if (It != Materials.end())
        {
            pResidency = &It->second;
            return RTXPTErrorCode::Ok;
        }

        MaterialResidency Residency;
        Residency.Asset = pAsset;
        if (!MaterialRows.Allocate(1, Residency.Row))
        {
            const Uint32 Capacity = NextCapacity(MaterialRows.GetCapacity(), MaterialRows.GetCapacity() + 1u, kMinMaterialRows);
            MaterialRows.Grow(Capacity);
            MaterialData.resize(Capacity);
            MaterialBuffer.Release();
            MaterialRows.Allocate(1, Residency.Row);
        }

        // Texture indices of the asset address its own views; rebase them into the bindless table.
        Residency.TextureSlots.reserve(pAsset->Textures.size());
        for (const RefCntAutoPtr<ITextureView>& pView : pAsset->Textures)
        {
            Uint32 Slot = 0;
            if (!AcquireTextureSlot(pView, Slot, BindingsChanged))
            {
                for (Uint32 Acquired : Residency.TextureSlots)
                    ReleaseTextureSlot(Acquired, BindingsChanged);
                MaterialRows.Free(Residency.Row, 1);
                LOG_ERROR_MESSAGE("RTXPT material textures exceed the bindless table for ", pAsset->Name);
                return RTXPTErrorCode::BindlessTableExceeded;
            }
            Residency.TextureSlots.push_back(Slot);
        }
        MaterialPTData                   Data          = pAsset->Data;
        const std::pair<Uint32, Uint32*> TextureRefs[] = {
            {kMaterialFlag_HasBaseColorTexture, &Data.baseColorTextureIndex},
            {kMaterialFlag_HasEmissiveTexture, &Data.emissiveTextureIndex},
            {kMaterialFlag_HasMetallicRoughnessTexture, &Data.metallicRoughnessTextureIndex},
            {kMaterialFlag_HasNormalTexture, &Data.normalTextureIndex},
            {kMaterialFlag_HasTransmissionTexture, &Data.transmissionTextureIndex},
            {kMaterialFlag_HasThicknessTexture, &Data.thicknessTextureIndex},
        };
        for (const auto& Ref : TextureRefs)
            *Ref.second = (Data.flags & Ref.first) != 0u ? Residency.TextureSlots[*Ref.second] : 0u;

        MaterialData[Residency.Row] = Data;
        DirtyMaterialRows.push_back(Residency.Row);
        pResidency = &Materials.emplace(pAsset->AssetId, std::move(Residency)).first->second;
        return RTXPTErrorCode::Ok;
    }

    // Releases the residency of assets that no snapshot and no repository holds anymore.
    void EvictReleasedAssets(bool& BindingsChanged)
    {
        for (auto It = Geometries.begin(); It != Geometries.end();)
        {
            if (!It->second.Asset.expired())
            {
                ++It;
                continue;
            }
            const Uint64 GeometryId = It->first;
            for (auto BLASIt = BLASes.begin(); BLASIt != BLASes.end();)
                BLASIt = BLASIt->first.GeometryId == GeometryId ? BLASes.erase(BLASIt) : std::next(BLASIt);
            Vertices.Ranges.Free(It->second.VertexBase, It->second.VertexCount);
            Indices.Ranges.Free(It->second.IndexBase, It->second.IndexCount);
            SkinData.Ranges.Free(It->second.SkinBase, It->second.SkinCount);
            if (It->second.NormalSkinCount != 0)
                SkinData.Ranges.Free(It->second.NormalSkinBase, It->second.NormalSkinCount);
            It = Geometries.erase(It);
        }
        for (auto It = Materials.begin(); It != Materials.end();)
        {
            if (!It->second.Asset.expired())
            {
                ++It;
                continue;
            }
            for (Uint32 Slot : It->second.TextureSlots)
                ReleaseTextureSlot(Slot, BindingsChanged);
            MaterialRows.Free(It->second.Row, 1);
            It = Materials.erase(It);
        }
    }

    bool BuildBLAS(IDeviceContext* pContext, IBottomLevelAS* pBLAS, const BLASBuildTriangleData* pTriangles, Uint32 TriangleCount, bool Update = false)
    {
        const Uint64 ScratchSize = Update ? pBLAS->GetScratchBufferSizes().Update : pBLAS->GetScratchBufferSizes().Build;
        if (!BLASScratch || BLASScratch->GetDesc().Size < ScratchSize)
        {
            BLASScratch.Release();
            Device->CreateBuffer(MakeBufferDesc("RTXPT BLAS scratch buffer", std::max<Uint64>(ScratchSize, 1u << 16), BIND_RAY_TRACING, BUFFER_MODE_UNDEFINED, 0),
                                 nullptr, &BLASScratch);
            if (!BLASScratch)
                return false;
        }
        BuildBLASAttribs Attribs;
        Attribs.pBLAS                       = pBLAS;
        Attribs.BLASTransitionMode          = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Attribs.GeometryTransitionMode      = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Attribs.pTriangleData               = pTriangles;
        Attribs.TriangleDataCount           = TriangleCount;
        Attribs.pScratchBuffer              = BLASScratch;
        Attribs.ScratchBufferTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Attribs.Update                      = Update;
        pContext->BuildBLAS(Attribs);
        return true;
    }

    // The BLAS of one mesh of a resident geometry: one geometry per primitive, opaque unless its material needs
    // any-hit filtering. Rigid meshes read the vertex arena; skinned instances read their skinned vertices from
    // VertexBase and update their existing BLAS.
    bool BuildMeshBLAS(IDeviceContext* pContext, const RTXPTGeometryAsset& Geometry, const GeometryResidency& Residency, Uint32 MeshIndex, const std::vector<Uint64>& AnyHitMask, IBuffer* pVertexBuffer, Uint32 VertexBase, RefCntAutoPtr<IBottomLevelAS>& pBLAS)
    {
        const bool                         Skinned = Geometry.IsSkinned();
        const bool                         Update  = Skinned && pBLAS != nullptr;
        const RTXPTGeometryMesh&           Mesh    = Geometry.Meshes[MeshIndex];
        std::vector<std::string>           Names(Mesh.Primitives.size());
        std::vector<BLASTriangleDesc>      Descs(Mesh.Primitives.size());
        std::vector<BLASBuildTriangleData> Triangles(Mesh.Primitives.size());
        for (size_t i = 0; i < Mesh.Primitives.size(); ++i)
        {
            const RTXPTGeometryPrimitiveDesc& Primitive = Mesh.Primitives[i];
            const bool                        Indexed   = Primitive.IndexCount > 0;
            const bool                        AnyHit    = (AnyHitMask[i / 64] >> (i % 64)) & 1u;
            Names[i]                                    = "primitive " + std::to_string(i);

            BLASTriangleDesc& Desc    = Descs[i];
            Desc.GeometryName         = Names[i].c_str();
            Desc.MaxVertexCount       = Indexed ? Geometry.VertexCount : Primitive.VertexCount;
            Desc.VertexValueType      = VT_FLOAT32;
            Desc.VertexComponentCount = 3;
            Desc.MaxPrimitiveCount    = (Indexed ? Primitive.IndexCount : Primitive.VertexCount) / 3u;
            Desc.IndexType            = Indexed ? VT_UINT32 : VT_UNDEFINED;

            BLASBuildTriangleData& Data = Triangles[i];
            Data.GeometryName           = Desc.GeometryName;
            Data.pVertexBuffer          = pVertexBuffer;
            Data.VertexOffset           = Uint64{VertexBase + (Indexed ? 0u : Primitive.FirstVertex)} * kGeometryVertexStride;
            Data.VertexStride           = kGeometryVertexStride;
            Data.VertexCount            = Desc.MaxVertexCount;
            Data.VertexValueType        = VT_FLOAT32;
            Data.VertexComponentCount   = 3;
            Data.PrimitiveCount         = Desc.MaxPrimitiveCount;
            // Alpha-tested and ExcludeFromNEE materials need programmable candidate filtering; everything else stays
            // opaque to skip any-hit entirely.
            Data.Flags = AnyHit ? RAYTRACING_GEOMETRY_FLAG_NONE : RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            if (Indexed)
            {
                Data.pIndexBuffer = Indices.Buffer;
                Data.IndexOffset  = Uint64{Residency.IndexBase + Primitive.FirstIndex} * sizeof(Uint32);
                Data.IndexType    = VT_UINT32;
            }
        }

        RefCntAutoPtr<IBottomLevelAS> NewBLAS;
        if (!Update)
        {
            const std::string Name = "RTXPT BLAS " + Geometry.Name + " mesh " + std::to_string(MeshIndex);
            BottomLevelASDesc Desc;
            Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
            Desc.Name                 = Name.c_str();
            Desc.Flags                = Skinned ? RAYTRACING_BUILD_AS_ALLOW_UPDATE | RAYTRACING_BUILD_AS_PREFER_FAST_BUILD : RAYTRACING_BUILD_AS_PREFER_FAST_TRACE;
            Desc.pTriangles           = Descs.data();
            Desc.TriangleCount        = static_cast<Uint32>(Descs.size());
            Device->CreateBLAS(Desc, &NewBLAS);
            if (!NewBLAS)
                return false;
        }
        if (!BuildBLAS(pContext, Update ? pBLAS.RawPtr() : NewBLAS.RawPtr(), Triangles.data(), static_cast<Uint32>(Triangles.size()), Update))
            return false;
        if (!Update)
            pBLAS = std::move(NewBLAS);
        ++(Update ? Stats.SkinnedBLASUpdateCount : Skinned ? Stats.SkinnedBLASBuildCount : Stats.RigidBLASBuildCount);
        return true;
    }

    // Skins the instances whose pose changed and builds or updates their BLASes.
    RTXPTErrorCode PrepareSkinnedInstances(IDeviceContext* pContext, std::vector<SkinJob>& Jobs, std::vector<TLASBuildInstanceData>& TLASInstances, bool& BindingsChanged)
    {
        std::vector<float4x4> Palette;
        std::vector<Uint32>   JointBases(Jobs.size(), 0u);
        for (size_t i = 0; i < Jobs.size(); ++i)
        {
            if (!Jobs[i].Reskin)
                continue;
            JointBases[i] = static_cast<Uint32>(Palette.size());
            Palette.insert(Palette.end(), Jobs[i].pState->Pose->begin(), Jobs[i].pState->Pose->end());
        }
        if (!Palette.empty())
        {
            if (!Skinning.IsReady() && !Skinning.Initialize(Device, CreateInfo.pEngineFactory, CreateInfo.pStateCache, CreateInfo.pPSOCache))
                return RTXPTErrorCode::SkinningPassFailed;
            bool PaletteRecreated = false;
            if (!JointMatrices.Upload(Device, pContext, Palette, PaletteRecreated))
                return RTXPTErrorCode::JointPaletteFailed;
            RTXPTSkinningBindings Bindings;
            Bindings.pSourceVertices  = Vertices.Buffer;
            Bindings.pSkinData        = SkinData.Buffer;
            Bindings.pJointMatrices   = JointMatrices.Buffer;
            Bindings.pSkinnedVertices = SkinnedVertices.Buffer;
            if (!Skinning.Bind(Bindings))
                return RTXPTErrorCode::SkinningBindingsFailed;
        }

        for (size_t i = 0; i < Jobs.size(); ++i)
        {
            SkinJob&                  Job      = Jobs[i];
            SkinnedInstance&          Instance = *Job.pInstance;
            const RTXPTGeometryAsset& Geometry = *Job.pState->Geometry;
            if (Job.Reskin)
            {
                RTXPTSkinningDispatch Range;
                Range.SourceVertexBase = Job.pGeometry->VertexBase;
                Range.DestVertexBase   = Instance.Vertices.OutputBase;
                Range.JointBase        = JointBases[i];
                Range.VertexCount      = Instance.Vertices.VertexCount;
                Range.SkinBase         = Job.pGeometry->SkinBase;
                Range.NormalSkinBase   = Job.pGeometry->NormalSkinBase;
                if (!Skinning.Dispatch(pContext, Range))
                {
                    LOG_ERROR_MESSAGE("RTXPT skinning dispatch failed for ", Geometry.Name);
                    return RTXPTErrorCode::SkinningDispatchFailed;
                }
                ++Stats.SkinningDispatchCount;
            }
            if (Instance.NeedsBLASRebuild)
                Instance.BLAS.Release();
            if ((Job.Reskin || !Instance.BLAS) &&
                !BuildMeshBLAS(pContext, Geometry, *Job.pGeometry, Job.pState->MeshIndex, Instance.AnyHitMask, SkinnedVertices.Buffer,
                               Instance.Vertices.OutputBase, Instance.BLAS))
            {
                LOG_ERROR_MESSAGE("RTXPT skinned BLAS build failed for ", Geometry.Name);
                return RTXPTErrorCode::BLASBuildFailed;
            }
            Instance.Vertices.PoseRevision     = Job.pState->PoseRevision;
            Instance.NeedsBLASRebuild          = false;
            TLASInstances[Job.TLASIndex].pBLAS = Instance.BLAS;
        }
        return RTXPTErrorCode::Ok;
    }

    // An empty scene still needs a TLAS: it holds one instance of this BLAS that no ray mask selects.
    bool BuildPlaceholderBLAS(IDeviceContext* pContext)
    {
        if (PlaceholderBLAS)
            return true;
        const float3 Positions[3] = {float3{0, 0, 0}, float3{1, 0, 0}, float3{0, 1, 0}};
        BufferData   Data{Positions, sizeof(Positions)};
        BufferDesc   VertexDesc = MakeBufferDesc("RTXPT placeholder BLAS vertices", sizeof(Positions), BIND_RAY_TRACING, BUFFER_MODE_UNDEFINED, 0);
        VertexDesc.Usage        = USAGE_IMMUTABLE;
        RefCntAutoPtr<IBuffer> Vertices;
        Device->CreateBuffer(VertexDesc, &Data, &Vertices);
        if (!Vertices)
            return false;

        BLASTriangleDesc Triangle;
        Triangle.GeometryName         = "placeholder";
        Triangle.MaxVertexCount       = 3;
        Triangle.VertexValueType      = VT_FLOAT32;
        Triangle.VertexComponentCount = 3;
        Triangle.MaxPrimitiveCount    = 1;
        BottomLevelASDesc Desc;
        Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
        Desc.Name                 = "RTXPT placeholder BLAS";
        Desc.pTriangles           = &Triangle;
        Desc.TriangleCount        = 1;
        RefCntAutoPtr<IBottomLevelAS> BLAS;
        Device->CreateBLAS(Desc, &BLAS);
        if (!BLAS)
            return false;

        BLASBuildTriangleData Build;
        Build.GeometryName         = Triangle.GeometryName;
        Build.pVertexBuffer        = Vertices;
        Build.VertexStride         = sizeof(float3);
        Build.VertexCount          = 3;
        Build.VertexValueType      = VT_FLOAT32;
        Build.VertexComponentCount = 3;
        Build.PrimitiveCount       = 1;
        Build.Flags                = RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        if (!BuildBLAS(pContext, BLAS, &Build, 1))
            return false;
        PlaceholderVertices = std::move(Vertices);
        PlaceholderBLAS     = std::move(BLAS);
        return true;
    }

    bool BuildTLAS(IDeviceContext* pContext, const std::vector<TLASBuildInstanceData>& Instances, bool& BindingsChanged)
    {
        const Uint32 Count = static_cast<Uint32>(Instances.size());
        if (!TLAS || TLAS->GetDesc().MaxInstanceCount < Count)
        {
            const Uint32   Capacity = NextCapacity(TLAS ? TLAS->GetDesc().MaxInstanceCount : 0u, Count, kMinTLASInstances);
            TopLevelASDesc Desc;
            Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
            Desc.Name                 = "RTXPT scene TLAS";
            Desc.MaxInstanceCount     = Capacity;
            Desc.Flags                = RAYTRACING_BUILD_AS_PREFER_FAST_TRACE;
            RefCntAutoPtr<ITopLevelAS> NewTLAS;
            RefCntAutoPtr<IBuffer>     InstanceBuffer;
            RefCntAutoPtr<IBuffer>     Scratch;
            Device->CreateTLAS(Desc, &NewTLAS);
            if (!NewTLAS)
                return false;
            Device->CreateBuffer(MakeBufferDesc("RTXPT TLAS instance buffer", Uint64{Capacity} * TLAS_INSTANCE_DATA_SIZE, BIND_RAY_TRACING, BUFFER_MODE_UNDEFINED, 0),
                                 nullptr, &InstanceBuffer);
            Device->CreateBuffer(MakeBufferDesc("RTXPT TLAS scratch buffer", std::max<Uint64>(NewTLAS->GetScratchBufferSizes().Build, 1u), BIND_RAY_TRACING, BUFFER_MODE_UNDEFINED, 0),
                                 nullptr, &Scratch);
            if (!InstanceBuffer || !Scratch)
                return false;
            TLAS               = std::move(NewTLAS);
            TLASInstanceBuffer = std::move(InstanceBuffer);
            TLASScratch        = std::move(Scratch);
            ++Stats.TLASCreateCount;
            BindingsChanged = true;
        }

        BuildTLASAttribs Attribs;
        Attribs.pTLAS                        = TLAS;
        Attribs.TLASTransitionMode           = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Attribs.BLASTransitionMode           = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Attribs.pInstances                   = Instances.data();
        Attribs.InstanceCount                = Count;
        Attribs.pInstanceBuffer              = TLASInstanceBuffer;
        Attribs.InstanceBufferTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Attribs.HitGroupStride               = kHitGroupStride;
        Attribs.BindingMode                  = HIT_GROUP_BINDING_MODE_PER_GEOMETRY;
        Attribs.pScratchBuffer               = TLASScratch;
        Attribs.ScratchBufferTransitionMode  = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Attribs.Update                       = false;
        pContext->BuildTLAS(Attribs);
        ++Stats.TLASBuildCount;
        return true;
    }

    bool UploadMaterials(IDeviceContext* pContext, bool& BindingsChanged)
    {
        if (MaterialData.empty())
        {
            MaterialRows.Grow(kMinMaterialRows);
            MaterialData.resize(kMinMaterialRows);
        }
        if (!MaterialBuffer)
        {
            BufferDesc Desc = MakeBufferDesc("RTXPT scene material buffer", MaterialData.size() * sizeof(MaterialPTData), BIND_SHADER_RESOURCE,
                                             BUFFER_MODE_STRUCTURED, sizeof(MaterialPTData));
            BufferData Data{MaterialData.data(), Desc.Size};
            Device->CreateBuffer(Desc, &Data, &MaterialBuffer);
            if (!MaterialBuffer)
                return false;
            DirtyMaterialRows.clear();
            BindingsChanged = true;
            return true;
        }
        for (Uint32 Row : DirtyMaterialRows)
        {
            pContext->UpdateBuffer(MaterialBuffer, Uint64{Row} * sizeof(MaterialPTData), sizeof(MaterialPTData), &MaterialData[Row],
                                   RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        DirtyMaterialRows.clear();
        return true;
    }

    RTXPTErrorCode Prepare(IDeviceContext* pContext, const RTXPTSceneSnapshotPtr& Snapshot, RTXPTGpuProfiler* Profiler)
    {
        const Uint64 PrepareIndex    = ++Stats.PrepareCount;
        bool         BindingsChanged = !HasPrepared;
        Frame                        = {};
        EvictReleasedAssets(BindingsChanged);
        if (!Vertices.Reserve(Device) || !Indices.Reserve(Device) || !SkinData.Reserve(Device) || !SkinnedVertices.Reserve(Device) ||
            !BuildPlaceholderBLAS(pContext))
            return RTXPTErrorCode::SceneBuffersFailed;

        std::vector<SubInstanceData>       SubInstanceRows;
        std::vector<float4x4>              SubInstanceMatrices;
        std::vector<TLASBuildInstanceData> TLASInstances;
        std::vector<std::string>           InstanceNames;
        std::vector<EmissiveState>         Emissive;
        std::vector<RTXPTLightSourceRange> Sources;
        Uint32                             EmissiveTriangles = 0;
        float                              ProxyWeight       = 0.0f;
        std::vector<SkinJob>               SkinJobs;
        Uint32                             ValidPreviousRigidInstances   = 0;
        Uint32                             ValidPreviousSkinnedInstances = 0;
        TLASInstances.reserve(Snapshot->Instances.size());
        InstanceNames.reserve(Snapshot->Instances.size() + 1u);

        std::vector<const MaterialResidency*> PrimitiveMaterials;
        for (const std::shared_ptr<const RTXPTSceneInstanceState>& pState : Snapshot->Instances)
        {
            if (!pState->Enabled)
                continue;
            const RTXPTGeometryAsset& Geometry = *pState->Geometry;
            const Uint64              Handle   = (Uint64{pState->Handle.Slot} << 32u) | pState->Handle.Generation;

            const GeometryResidency* pGeometry    = nullptr;
            const RTXPTErrorCode     GeometryCode = MakeGeometryResident(pContext, pState->Geometry, pGeometry, BindingsChanged);
            if (GeometryCode != RTXPTErrorCode::Ok)
                return GeometryCode;

            const RTXPTGeometryMesh& Mesh           = Geometry.Meshes[pState->MeshIndex];
            const Uint32             PrimitiveCount = static_cast<Uint32>(Mesh.Primitives.size());
            BLASKey                  Key;
            Key.GeometryId = Geometry.AssetId;
            Key.MeshIndex  = pState->MeshIndex;
            Key.AnyHitMask.assign((PrimitiveCount + 63u) / 64u, 0u);
            PrimitiveMaterials.resize(PrimitiveCount);
            for (Uint32 i = 0; i < PrimitiveCount; ++i)
            {
                const std::shared_ptr<const RTXPTMaterialAsset>& pMaterial    = pState->Materials[Mesh.Primitives[i].MaterialSlot];
                const RTXPTErrorCode                             MaterialCode = MakeMaterialResident(pMaterial, PrimitiveMaterials[i], BindingsChanged);
                if (MaterialCode != RTXPTErrorCode::Ok)
                    return MaterialCode;
                if (pMaterial->NeedsAnyHit())
                    Key.AnyHitMask[i / 64u] |= Uint64{1} << (i % 64u);
            }

            IBottomLevelAS* pBLAS = nullptr;
            if (Geometry.IsSkinned())
            {
                // Skinned instances own their skinned vertices and BLAS; both are built after the loop.
                SkinnedInstance& Skinned = SkinnedInstances[Handle];
                SkinJob          Job;
                Job.pInstance = &Skinned;
                Job.pState    = pState.get();
                Job.pGeometry = pGeometry;
                Job.TLASIndex = static_cast<Uint32>(TLASInstances.size());
                if (Skinned.GeometryId != Geometry.AssetId)
                {
                    Uint32 AllocationBase = 0;
                    if (Geometry.VertexCount > std::numeric_limits<Uint32>::max() / RTXPTSkinnedVertexHistory::RangeCount ||
                        !SkinnedVertices.Allocate(Device, pContext, Geometry.VertexCount * RTXPTSkinnedVertexHistory::RangeCount, AllocationBase, BindingsChanged))
                    {
                        LOG_ERROR_MESSAGE("RTXPT skinned vertex arena allocation failed for ", Geometry.Name);
                        return RTXPTErrorCode::ArenaAllocationFailed;
                    }
                    if (Skinned.GeometryId != 0)
                        SkinnedVertices.Ranges.Free(Skinned.Vertices.AllocationBase, Skinned.Vertices.VertexCount * RTXPTSkinnedVertexHistory::RangeCount);
                    Skinned            = {};
                    Skinned.GeometryId = Geometry.AssetId;
                    Skinned.Vertices   = RTXPTSkinnedVertexHistory{AllocationBase, Geometry.VertexCount};
                }
                Job.Reskin = Skinned.Vertices.PreparePose(pState->PoseRevision);
                if (Skinned.AnyHitMask != Key.AnyHitMask)
                {
                    Skinned.NeedsBLASRebuild = true;
                    Skinned.AnyHitMask       = Key.AnyHitMask;
                }
                Skinned.LastUsed = PrepareIndex;
                SkinJobs.push_back(Job);
            }
            else
            {
                BLASEntry& Entry = BLASes[Key];
                if (!Entry.BLAS &&
                    !BuildMeshBLAS(pContext, Geometry, *pGeometry, Key.MeshIndex, Key.AnyHitMask, Vertices.Buffer, pGeometry->VertexBase, Entry.BLAS))
                {
                    BLASes.erase(Key);
                    LOG_ERROR_MESSAGE("RTXPT BLAS build failed for ", Geometry.Name);
                    return RTXPTErrorCode::BLASBuildFailed;
                }
                Entry.LastUsed = PrepareIndex;
                pBLAS          = Entry.BLAS;
            }

            const auto* Previous = History.FindPreviousSurface(*pState);
            if (Geometry.IsSkinned())
            {
                if (Previous && !SkinnedInstances[Handle].Vertices.HasPreviousPose(Previous->PoseRevision))
                    Previous = nullptr;
                if (Previous)
                    ++ValidPreviousSkinnedInstances;
            }
            else if (Previous)
            {
                ++ValidPreviousRigidInstances;
            }
            const Uint32 SubInstanceBase = static_cast<Uint32>(SubInstanceRows.size());
            if (Uint64{SubInstanceBase} + PrimitiveCount > kMaxSubInstanceCount)
                return RTXPTErrorCode::SubInstanceLimitExceeded;
            for (Uint32 i = 0; i < PrimitiveCount; ++i)
            {
                const RTXPTGeometryPrimitiveDesc& Primitive = Mesh.Primitives[i];
                const RTXPTMaterialAsset&         Material  = *pState->Materials[Primitive.MaterialSlot];
                SubInstanceData                   Row;
                Row.MaterialID = PrimitiveMaterials[i]->Row;
                // Skinned instances read their own skinned vertices.
                const Uint32 VertexBase = Geometry.IsSkinned() ? SkinnedInstances[Handle].Vertices.OutputBase : pGeometry->VertexBase;
                Row.Flags               = Geometry.IsSkinned() ? kSubInstanceFlag_Skinned : 0u;
                if (Previous)
                {
                    Row.Flags |= Geometry.IsSkinned() ? kSubInstanceFlag_PreviousSkinnedSurface : kSubInstanceFlag_PreviousRigidSurface;
                    Row.PreviousTransform = Previous->Transform;
                    if (Geometry.IsSkinned())
                        Row.PreviousVertexOffset = SkinnedInstances[Handle].Vertices.GetPreviousBase() + (Primitive.IndexCount > 0 ? 0u : Primitive.FirstVertex);
                }
                Row.VertexOffset = VertexBase + (Primitive.IndexCount > 0 ? 0u : Primitive.FirstVertex);
                Row.VertexCount  = Primitive.VertexCount;
                if (Primitive.IndexCount > 0)
                {
                    Row.Flags |= kSubInstanceFlag_Indexed;
                    Row.IndexOffset = pGeometry->IndexBase + Primitive.FirstIndex;
                    Row.IndexCount  = Primitive.IndexCount;
                }
                Row.EmissiveTriangleOffset = EmissiveTriangles;
                if (Material.IsEmissiveAreaLight())
                {
                    const Uint32 TriangleCount = (Primitive.IndexCount > 0 ? Primitive.IndexCount : Primitive.VertexCount) / 3u;
                    if (Uint64{EmissiveTriangles} + TriangleCount > std::numeric_limits<Uint32>::max())
                        return RTXPTErrorCode::EmissiveTriangleLimitExceeded;
                    Sources.push_back({{2u, Handle, Geometry.AssetId, (Uint64{pState->MeshIndex} << 32u) | i}, EmissiveTriangles, TriangleCount});
                    Emissive.push_back({Handle, pState->TransformRevision, pState->PoseRevision, pState->MaterialRevision, SubInstanceBase + i});
                    EmissiveTriangles += TriangleCount;
                    ProxyWeight += static_cast<float>(TriangleCount) * std::max(1e-6f, Material.EmissionMagnitude);
                }
                SubInstanceRows.push_back(Row);
                SubInstanceMatrices.push_back(pState->Transform);
            }

            InstanceNames.push_back(std::to_string(pState->Handle.Slot));
            TLASBuildInstanceData Instance;
            Instance.pBLAS     = pBLAS;
            Instance.Transform = ToInstanceMatrix(pState->Transform);
            Instance.CustomId  = SubInstanceBase;
            Instance.Flags     = RAYTRACING_INSTANCE_NONE;
            Instance.Mask      = 0xFF;
            TLASInstances.push_back(Instance);
        }

        const Uint32 InstanceCount = static_cast<Uint32>(TLASInstances.size());
        if (TLASInstances.empty())
        {
            InstanceNames.push_back("placeholder");
            TLASBuildInstanceData Instance;
            Instance.pBLAS = PlaceholderBLAS;
            Instance.Mask  = 0;
            TLASInstances.push_back(Instance);
        }
        if (SubInstanceRows.empty())
        {
            // The sub-instance tables are bound unconditionally.
            SubInstanceRows.emplace_back();
            SubInstanceMatrices.push_back(float4x4::Identity());
        }
        for (size_t i = 0; i < TLASInstances.size(); ++i)
            TLASInstances[i].InstanceName = InstanceNames[i].c_str();

        // BLASes and skinned instances that were not used for a while are released.
        for (auto It = BLASes.begin(); It != BLASes.end();)
            It = PrepareIndex - It->second.LastUsed > kBLASRetainPrepares ? BLASes.erase(It) : std::next(It);
        for (auto It = SkinnedInstances.begin(); It != SkinnedInstances.end();)
        {
            // Failed attempts must not evict vertices belonging to the last successfully published scene.
            if (It->second.Vertices.HasPrevious() || PrepareIndex - It->second.LastUsed <= kBLASRetainPrepares)
            {
                ++It;
                continue;
            }
            SkinnedVertices.Ranges.Free(It->second.Vertices.AllocationBase, It->second.Vertices.VertexCount * RTXPTSkinnedVertexHistory::RangeCount);
            It = SkinnedInstances.erase(It);
        }
        if (!SkinJobs.empty())
        {
            RTXPTGpuProfiler::Scope Timing{Profiler, pContext, RTXPTGpuProfiler::Stage::SkinAndBLAS};
            const RTXPTErrorCode    Code = PrepareSkinnedInstances(pContext, SkinJobs, TLASInstances, BindingsChanged);
            if (Code != RTXPTErrorCode::Ok)
                return Code;
        }

        if (!ReserveTextureTable(1u, BindingsChanged) || !UploadMaterials(pContext, BindingsChanged) ||
            !SubInstances.Upload(Device, pContext, SubInstanceRows, BindingsChanged) ||
            !SubInstanceTransforms.Upload(Device, pContext, SubInstanceMatrices, BindingsChanged))
            return RTXPTErrorCode::SceneTablesFailed;
        {
            RTXPTGpuProfiler::Scope Timing{Profiler, pContext, RTXPTGpuProfiler::Stage::TLAS};
            if (!BuildTLAS(pContext, TLASInstances, BindingsChanged))
                return RTXPTErrorCode::SceneTablesFailed;
        }

        bool LightingChanged = false;
        if (!LightsUploaded || Snapshot->LightRevision != LightRevision)
        {
            if (!Lights.UploadAnalyticLights(Device, *Snapshot->Lights))
                return RTXPTErrorCode::LightBufferFailed;
            LightRevision   = Snapshot->LightRevision;
            LightsUploaded  = true;
            LightingChanged = true;
        }

        auto SameSource = [](const RTXPTLightSourceRange& A, const RTXPTLightSourceRange& B) {
            return A.Key == B.Key && A.Base == B.Base && A.Count == B.Count;
        };
        const bool     SourcesChanged    = !std::equal(Sources.begin(), Sources.end(), EmissiveSources.begin(), EmissiveSources.end(), SameSource);
        const IBuffer* pPreviousEmissive = Lights.GetEmissiveTriangleBuffer();
        if (SourcesChanged)
            EmissiveSources = Sources;
        if (!Lights.SetEmissiveTriangles(Device, std::move(Sources), EmissiveTriangles, ProxyWeight))
            return RTXPTErrorCode::EmissiveTriangleBufferFailed;
        LightingChanged = LightingChanged || SourcesChanged || pPreviousEmissive != Lights.GetEmissiveTriangleBuffer();

        bool EmissiveRebuilt = false;
        if (EmissiveTriangles != 0 && (LightingChanged || BindingsChanged || !EmissiveBuilt || !(Emissive == EmissiveStates)))
        {
            const RTXPTErrorCode Code = BuildEmissiveTriangles(pContext, static_cast<Uint32>(SubInstanceRows.size()));
            if (Code != RTXPTErrorCode::Ok)
                return Code;
            EmissiveRebuilt = true;
        }
        EmissiveBuilt  = EmissiveTriangles != 0;
        EmissiveStates = std::move(Emissive);

        if (BindingsChanged)
            ++LastBindingRevision;
        if (LightingChanged)
            ++LightingRevision;
        else if (EmissiveRebuilt)
            ++EmissiveRevision;

        Frame.Snapshot                    = Snapshot;
        Frame.BuildIndex                  = Stats.TLASBuildCount;
        Frame.BindingRevision             = LastBindingRevision;
        Frame.LightingRevision            = LightingRevision;
        Frame.EmissiveRevision            = EmissiveRevision;
        Frame.pTLAS                       = TLAS;
        Frame.pVertexBuffer               = Vertices.Buffer;
        Frame.pSkinnedVertexBuffer        = SkinnedVertices.Buffer;
        Frame.pIndexBuffer                = Indices.Buffer;
        Frame.pSubInstanceBuffer          = SubInstances.Buffer;
        Frame.pSubInstanceTransformBuffer = SubInstanceTransforms.Buffer;
        Frame.pMaterialBuffer             = MaterialBuffer;
        Frame.ppMaterialTextures          = TextureTable.data();
        Frame.MaterialTextureCount        = static_cast<Uint32>(TextureTable.size());
        Frame.pLights                     = &Lights;
        Frame.InstanceCount               = InstanceCount;
        Frame.SubInstanceCount            = static_cast<Uint32>(SubInstanceRows.size());
        Frame.InvalidHistoryInstances     = InstanceCount - ValidPreviousRigidInstances - ValidPreviousSkinnedInstances;

        Stats.InstanceCount                 = InstanceCount;
        Stats.SubInstanceCount              = Frame.SubInstanceCount;
        Stats.ResidentGeometryCount         = static_cast<Uint32>(Geometries.size());
        Stats.ResidentMaterialCount         = static_cast<Uint32>(Materials.size());
        Stats.BLASCount                     = static_cast<Uint32>(BLASes.size());
        Stats.SkinnedInstanceCount          = static_cast<Uint32>(SkinJobs.size());
        Stats.TextureCount                  = static_cast<Uint32>(TextureSlotOfView.size());
        Stats.TextureCapacity               = Frame.MaterialTextureCount;
        Stats.EmissiveTriangleCount         = EmissiveTriangles;
        Stats.ValidPreviousRigidInstances   = ValidPreviousRigidInstances;
        Stats.ValidPreviousSkinnedInstances = ValidPreviousSkinnedInstances;
        Stats.InvalidHistoryInstances       = Frame.InvalidHistoryInstances;
        Stats.PreviousTransformStorageBytes = SubInstances.Buffer->GetDesc().Size / sizeof(SubInstanceData) * sizeof(float4x4);
        Stats.SkinnedVertexStorageBytes     = SkinnedVertices.Buffer->GetDesc().Size;
        Stats.PreviousSkinnedRangeBytes     = 0;
        // All allocations, skin/BLAS work, scene tables and lighting preparation succeeded. Publish each current
        // range only now, after SubInstanceRows captured the preceding range. No throwing work follows this point.
        for (auto& Entry : SkinnedInstances)
        {
            auto& Instance = Entry.second;
            if (Instance.LastUsed == PrepareIndex)
                Instance.Vertices.Publish();
            else
                Instance.Vertices.InvalidatePrevious();
            Stats.PreviousSkinnedRangeBytes += Uint64{Instance.Vertices.VertexCount} * kGeometryVertexStride;
        }
        HasPrepared = true;
        return RTXPTErrorCode::Ok;
    }

    RTXPTErrorCode BuildEmissiveTriangles(IDeviceContext* pContext, Uint32 SubInstanceCount)
    {
        const Uint32 TextureCount = static_cast<Uint32>(TextureTable.size());
        if (EmissivePassTextureCount != TextureCount)
        {
            // The bindless table size is compiled into the pass.
            RTXPTEmissiveTrianglePassCreateInfo PassCI;
            PassCI.pEngineFactory         = CreateInfo.pEngineFactory;
            PassCI.pStateCache            = CreateInfo.pStateCache;
            PassCI.pPSOCache              = CreateInfo.pPSOCache;
            PassCI.IndexValueType         = VT_UINT32;
            PassCI.MaterialTextureCount   = TextureCount;
            PassCI.EnableMaterialTextures = true;
            PassCI.ComputeSupported       = true;
            EmissivePassTextureCount      = 0;
            if (!EmissivePass.Initialize(Device, PassCI))
                return RTXPTErrorCode::EmissivePassFailed;
            EmissivePassTextureCount = TextureCount;
            ++Stats.PipelineCompileCount;
        }

        RTXPTEmissiveTriangleBindings Bindings;
        Bindings.pMaterialBuffer             = MaterialBuffer;
        Bindings.pSubInstanceBuffer          = SubInstances.Buffer;
        Bindings.pSubInstanceTransformBuffer = SubInstanceTransforms.Buffer;
        Bindings.pVertexBuffer               = Vertices.Buffer;
        Bindings.pSkinnedVertexBuffer        = SkinnedVertices.Buffer;
        Bindings.pIndexBuffer                = Indices.Buffer;
        Bindings.pEmissiveTriangleBuffer     = Lights.GetEmissiveTriangleBuffer();
        Bindings.pMaterialTextures           = TextureTable.data();
        Bindings.MaterialTextureCount        = TextureCount;
        if (!EmissivePass.Bind(Bindings) || !EmissivePass.Dispatch(pContext, SubInstanceCount))
            return RTXPTErrorCode::EmissiveBuildFailed;
        ++Stats.EmissiveBuildCount;
        return RTXPTErrorCode::Ok;
    }

    void Reset()
    {
        Vertices.Clear();
        Indices.Clear();
        SkinData.Clear();
        SkinnedVertices.Clear();
        JointMatrices.Clear();
        Skinning.Reset();
        SkinnedInstances.clear();
        MaterialRows.Clear();
        MaterialBuffer.Release();
        MaterialData.clear();
        DirtyMaterialRows.clear();
        SubInstances.Clear();
        History.Reset();
        SubInstanceTransforms.Clear();
        Geometries.clear();
        Materials.clear();
        BLASes.clear();
        TextureSlots.clear();
        TextureSlotOfView.clear();
        FreeTextureSlots.clear();
        TextureTable.clear();
        PlaceholderTexture.Release();
        PlaceholderBLAS.Release();
        PlaceholderVertices.Release();
        TLAS.Release();
        TLASInstanceBuffer.Release();
        TLASScratch.Release();
        BLASScratch.Release();
        Lights.Reset();
        EmissivePass.Reset();
        EmissivePassTextureCount = 0;
        EmissiveStates.clear();
        EmissiveSources.clear();
        LightRevision  = 0;
        LightsUploaded = false;
        EmissiveBuilt  = false;
        HasPrepared    = false;
        Frame          = {};
        Initialized    = false;
    }
};

RTXPTSceneFrameBuilder::RTXPTSceneFrameBuilder() :
    m_Impl{std::make_unique<Impl>()}
{
}

RTXPTSceneFrameBuilder::~RTXPTSceneFrameBuilder() = default;

RTXPTErrorCode RTXPTSceneFrameBuilder::Initialize(IRenderDevice* pDevice, const RTXPTSceneFrameBuilderCreateInfo& CreateInfo)
{
    Impl& I = *m_Impl;
    I.Reset();
    if (pDevice == nullptr || CreateInfo.pEngineFactory == nullptr || CreateInfo.pStateCache == nullptr)
        return RTXPTErrorCode::InvalidArgument;
    I.Device     = pDevice;
    I.CreateInfo = CreateInfo;

    I.Vertices.Name      = "RTXPT scene vertex arena";
    I.Vertices.BindFlags = BIND_SHADER_RESOURCE | BIND_RAY_TRACING;
    I.Vertices.Mode      = BUFFER_MODE_STRUCTURED;
    I.Vertices.Stride    = kGeometryVertexStride;
    I.Vertices.Minimum   = kMinArenaVertices;
    I.Indices.Name       = "RTXPT scene index arena";
    I.Indices.BindFlags  = BIND_SHADER_RESOURCE | BIND_RAY_TRACING;
    I.Indices.Mode       = BUFFER_MODE_FORMATTED;
    I.Indices.Stride     = sizeof(Uint32);
    I.Indices.Minimum    = kMinArenaIndices;

    I.SkinData.Name             = "RTXPT scene skin arena";
    I.SkinData.BindFlags        = BIND_SHADER_RESOURCE;
    I.SkinData.Mode             = BUFFER_MODE_STRUCTURED;
    I.SkinData.Stride           = sizeof(RTXPTSkinVertex);
    I.SkinData.Minimum          = kMinArenaSkinnedVertices;
    I.SkinnedVertices.Name      = "RTXPT skinned vertex arena";
    I.SkinnedVertices.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS | BIND_RAY_TRACING;
    I.SkinnedVertices.Mode      = BUFFER_MODE_STRUCTURED;
    I.SkinnedVertices.Stride    = kGeometryVertexStride;
    I.SkinnedVertices.Minimum   = kMinArenaSkinnedVertices;
    I.JointMatrices.Name        = "RTXPT joint palette";
    I.JointMatrices.Minimum     = kMinJointMatrices;

    I.SubInstances.Name             = "RTXPT sub-instance buffer";
    I.SubInstances.Minimum          = kMinSubInstances;
    I.SubInstanceTransforms.Name    = "RTXPT sub-instance transform buffer";
    I.SubInstanceTransforms.Minimum = kMinSubInstances;

    if (!I.CreatePlaceholderTexture())
        return RTXPTErrorCode::PlaceholderTextureFailed;
    I.Initialized = true;
    return RTXPTErrorCode::Ok;
}

void RTXPTSceneFrameBuilder::Reset()
{
    m_Impl->Reset();
}

bool RTXPTSceneFrameBuilder::IsInitialized() const
{
    return m_Impl->Initialized;
}

RTXPTErrorCode RTXPTSceneFrameBuilder::Prepare(IDeviceContext* pContext, const RTXPTSceneSnapshotPtr& Snapshot, RTXPTGpuProfiler* Profiler)
{
    Impl& I = *m_Impl;
    I.History.BeginPrepare();
    I.Frame = {};
    if (!I.Initialized)
        return RTXPTErrorCode::NotInitialized;
    if (pContext == nullptr || !Snapshot)
        return RTXPTErrorCode::InvalidArgument;
    RTXPTErrorCode Code = RTXPTErrorCode::OutOfMemory;
    try
    {
        Code = I.Prepare(pContext, Snapshot, Profiler);
        if (Code == RTXPTErrorCode::Ok)
        {
            I.Frame.Prepared = I.History.Publish(Snapshot);
            return Code;
        }
    }
    catch (const std::bad_alloc&)
    {
    }
    I.Frame = {};
    //Some immutable cache allocations may have succeeded before a later step failed. Rebind their generation on
    //retry even when the retry itself does not replace a buffer.
    I.HasPrepared = false;
    return Code;
}

const RTXPTSceneFrame& RTXPTSceneFrameBuilder::GetFrame() const
{
    return m_Impl->Frame;
}

const RTXPTSceneFrameStats& RTXPTSceneFrameBuilder::GetStats() const
{
    return m_Impl->Stats;
}

} // namespace Diligent
