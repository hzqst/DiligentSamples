/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

// The persistent RTXPT scene seen by a host: an asset repository of immutable geometry and materials, and a runtime
// scene of instances that reference them. The host describes which instances exist and their state; committed
// updates publish immutable snapshots, which RTXPTSceneFrameBuilder turns into GPU scene frames. Nothing here records
// GPU commands, compiles shaders or waits for the GPU.
//
// Matrices follow Diligent's float4x4 convention: row vectors, p' = p * M, translation in _41.._43.

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "BasicMath.hpp"
#include "BasicTypes.h"
#include "Buffer.h"
#include "RefCntAutoPtr.hpp"
#include "TextureView.h"
#include "RTXPTMaterialData.hpp"
#include "../shaders/PathTracer/Lighting/PolymorphicLight.h"

namespace Diligent
{

// One vertex of the shared geometry layout, matching GeometryVertexData in PathTracer/PathTracerShared.h.
struct RTXPTGeometryVertex
{
    float3 position  = float3{0, 0, 0};
    float3 normal    = float3{0, 1, 0};
    float2 texCoord0 = float2{0, 0};
};
static_assert(sizeof(RTXPTGeometryVertex) == 32, "RTXPTGeometryVertex layout must match GeometryVertexData");
static_assert(offsetof(RTXPTGeometryVertex, normal) == 12, "RTXPTGeometryVertex normal offset must match GeometryVertexData");
static_assert(offsetof(RTXPTGeometryVertex, texCoord0) == 24, "RTXPTGeometryVertex texCoord0 offset must match GeometryVertexData");

// Four joint influences of one vertex, matching SkinVertexData in PathTracer/PathTracerShared.h. Joints index the
// palette of the instance and are stored as floats, as the glTF loader does.
struct RTXPTSkinVertex
{
    float4 joints  = float4{0, 0, 0, 0};
    float4 weights = float4{0, 0, 0, 0};
};
static_assert(sizeof(RTXPTSkinVertex) == 32, "RTXPTSkinVertex layout must match SkinVertexData");

enum class RTXPTSceneResult : Uint32
{
    Ok = 0,
    InvalidArgument,
    InvalidHandle,
    InvalidPose,
    InvalidState,
    Unsupported,
    LimitExceeded,
    OutOfMemory,
};

const char* GetRTXPTSceneResultString(RTXPTSceneResult Result);

// Why a scene import, a frame preparation, or an embedded scene or view failed; GetRTXPTErrorCodeString describes it.
// A failure of one named model, geometry or material also logs its name, and the scene result, as a warning.
enum class RTXPTErrorCode : Uint32
{
    Ok = 0,
    InvalidArgument,
    OutOfMemory,
    NotInitialized,

    // Import
    UpdateNotOpen,
    UnsupportedVertexLayout,
    ModelBuffersInvalid,
    GeometryRejected,
    MaterialTextureMissing,
    MaterialRejected,
    MaterialReferenceMissing,
    JointMatricesMissing,
    InstanceRejected,
    LightsRejected,
    TransformUpdateFailed,

    // Embedded scene
    DeviceFeaturesMissing,
    SkinnedGeometryUnsupported,
    UpdateFailed,
    EnvironmentFailed,

    // Frame preparation
    PlaceholderTextureFailed,
    SceneBuffersFailed,
    ArenaAllocationFailed,
    BindlessTableExceeded,
    SkinningPassFailed,
    JointPaletteFailed,
    SkinningBindingsFailed,
    SkinningDispatchFailed,
    BLASBuildFailed,
    SubInstanceLimitExceeded,
    EmissiveTriangleLimitExceeded,
    SceneTablesFailed,
    LightBufferFailed,
    EmissiveTriangleBufferFailed,
    EmissivePassFailed,
    EmissiveBuildFailed,

    // Embedded view
    FrameTargetsFailed,
    LightSamplingSetupFailed,
    RayTracingSetupFailed,
    PostProcessSetupFailed,
    FrameNotPrepared,
    SceneBindingsFailed,
    ConstantsUpdateFailed,
    TraceFailed,
    AccumulationFailed,
    ToneMappingFailed,
    UnsupportedViewConfiguration,
};

const char* GetRTXPTErrorCodeString(RTXPTErrorCode Code);

// Opaque handles: a slot and the generation of its current occupant. A released slot is reused with a new
// generation, so a stale handle is detected rather than aliased. Generation 0 is never valid.
struct RTXPTSceneHandle
{
    Uint32 Slot       = 0;
    Uint32 Generation = 0;

    bool IsValid() const { return Generation != 0; }
    bool operator==(const RTXPTSceneHandle& Other) const { return Slot == Other.Slot && Generation == Other.Generation; }
    bool operator!=(const RTXPTSceneHandle& Other) const { return !(*this == Other); }
};

struct RTXPTGeometryHandle : RTXPTSceneHandle
{};
struct RTXPTMaterialHandle : RTXPTSceneHandle
{};
struct RTXPTInstanceHandle : RTXPTSceneHandle
{};

// A triangle list range of a mesh. With IndexCount > 0 the primitive is indexed: FirstIndex/IndexCount select
// indices of the geometry, and indices address geometry vertices from vertex 0. Otherwise FirstVertex/VertexCount
// select consecutive vertices. MaterialSlot selects the material binding of the instance.
struct RTXPTGeometryPrimitiveDesc
{
    Uint32 FirstIndex   = 0;
    Uint32 IndexCount   = 0;
    Uint32 FirstVertex  = 0;
    Uint32 VertexCount  = 0;
    Uint32 MaterialSlot = 0;
};

// The primitives one instance draws together under one transform; one BLAS of the scene.
struct RTXPTGeometryMeshDesc
{
    const RTXPTGeometryPrimitiveDesc* pPrimitives    = nullptr;
    Uint32                            PrimitiveCount = 0;
};

// Immutable geometry: vertices, 32-bit indices, meshes and an optional skin layout. Each of vertices, indices and skin
// data comes either from CPU arrays, copied before RegisterGeometry returns, or from a range of a GPU buffer, which the
// repository references until the frame builder has copied it. JointCount > 0 makes the geometry skinned: it then needs
// skin data, and every instance needs a palette of JointCount matrices. pNormalSkin optionally gives normals their own
// influences (for example GoldSrc MDL normal bones); without it normals use the position influences.
struct RTXPTGeometryDesc
{
    const char* Name = nullptr;

    Uint32 VertexCount = 0;
    Uint32 IndexCount  = 0;

    const RTXPTGeometryVertex* pVertices   = nullptr;
    const Uint32*              pIndices    = nullptr;
    const RTXPTSkinVertex*     pSkin       = nullptr;
    const RTXPTSkinVertex*     pNormalSkin = nullptr;

    IBuffer* pSourceVertexBuffer = nullptr;
    Uint64   SourceVertexOffset  = 0;
    IBuffer* pSourceIndexBuffer  = nullptr;
    Uint64   SourceIndexOffset   = 0;
    IBuffer* pSourceSkinBuffer   = nullptr;
    Uint64   SourceSkinOffset    = 0;

    const RTXPTGeometryMeshDesc* pMeshes   = nullptr;
    Uint32                       MeshCount = 0;

    Uint32 MaterialSlotCount = 0;
    Uint32 JointCount        = 0;
};

// Immutable material: the GPU record and the texture views it samples. The texture index fields of Data
// (baseColor, emissive, metallicRoughness, normal, transmission, thickness) index ppTextures; the frame builder
// rebases them into its bindless table. The repository keeps references to the views. EmissionMagnitude weighs the
// emissive triangles of the material for light sampling.
struct RTXPTMaterialDesc
{
    const char*          Name              = nullptr;
    MaterialPTData       Data              = {};
    ITextureView* const* ppTextures        = nullptr;
    Uint32               TextureCount      = 0;
    float                EmissionMagnitude = 0.0f;
};

// An analytic light of the scene and the key that identifies it across lighting updates (light sampling history).
struct RTXPTSceneLight
{
    PolymorphicLightInfoFull Light = {};
    std::array<Uint64, 4>    Key   = {};
};

// A new instance: one mesh of a geometry under one transform, with a complete binding of its material slots.
// Skinned geometry requires its initial palette (pPose, PoseMatrixCount == JointCount). UserKey identifies the
// instance in light sampling history and diagnostics; it does not need to be unique.
struct RTXPTInstanceDesc
{
    RTXPTGeometryHandle        Geometry;
    Uint32                     MeshIndex       = 0;
    float4x4                   Transform       = float4x4::Identity();
    const RTXPTMaterialHandle* pMaterials      = nullptr;
    Uint32                     MaterialCount   = 0;
    bool                       Enabled         = true;
    const float4x4*            pPose           = nullptr;
    Uint32                     PoseMatrixCount = 0;
    Uint64                     UserKey         = 0;
};

struct RTXPTGeometryMesh
{
    std::vector<RTXPTGeometryPrimitiveDesc> Primitives;
};

// A registered geometry. Immutable and shared by every instance and snapshot that uses it. The GPU sources stay
// referenced, so that a frame builder that lost its residency can upload the geometry again.
struct RTXPTGeometryAsset
{
    Uint64                           AssetId = 0;
    std::string                      Name;
    Uint32                           VertexCount       = 0;
    Uint32                           IndexCount        = 0;
    Uint32                           MaterialSlotCount = 0;
    Uint32                           JointCount        = 0;
    std::vector<RTXPTGeometryVertex> Vertices;
    std::vector<Uint32>              Indices;
    std::vector<RTXPTSkinVertex>     Skin;
    std::vector<RTXPTSkinVertex>     NormalSkin;
    RefCntAutoPtr<IBuffer>           pSourceVertexBuffer;
    Uint64                           SourceVertexOffset = 0;
    RefCntAutoPtr<IBuffer>           pSourceIndexBuffer;
    Uint64                           SourceIndexOffset = 0;
    RefCntAutoPtr<IBuffer>           pSourceSkinBuffer;
    Uint64                           SourceSkinOffset = 0;
    std::vector<RTXPTGeometryMesh>   Meshes;

    bool IsSkinned() const { return JointCount != 0; }
    bool HasSkinData() const { return !Skin.empty() || pSourceSkinBuffer; }
    bool HasNormalSkin() const { return !NormalSkin.empty(); }
};

// A registered material. Immutable and shared by every instance and snapshot that uses it.
struct RTXPTMaterialAsset
{
    Uint64                                   AssetId = 0;
    std::string                              Name;
    MaterialPTData                           Data;
    std::vector<RefCntAutoPtr<ITextureView>> Textures;
    float                                    EmissionMagnitude = 0.0f;

    // Alpha-tested and NEE-excluded materials need any-hit filtering, so their geometry cannot be built opaque.
    bool NeedsAnyHit() const { return (Data.flags & (kMaterialFlag_AlphaTested | kMaterialFlag_ExcludeFromNEE)) != 0u; }
    bool IsEmissiveAreaLight() const { return (Data.flags & kMaterialFlag_EmissiveAreaLight) != 0u; }
};

// The committed state of one instance. Immutable once published.
struct RTXPTSceneInstanceState
{
    RTXPTInstanceHandle                                    Handle;
    std::shared_ptr<const RTXPTGeometryAsset>              Geometry;
    Uint32                                                 MeshIndex = 0;
    float4x4                                               Transform = float4x4::Identity();
    std::vector<std::shared_ptr<const RTXPTMaterialAsset>> Materials;
    std::shared_ptr<const std::vector<float4x4>>           Pose;
    bool                                                   Enabled = true;
    Uint64                                                 UserKey = 0;
    // Incremented when the corresponding state of this instance changes; equal values mean equal state.
    Uint64 TransformRevision = 0;
    Uint64 PoseRevision      = 0;
    Uint64 MaterialRevision  = 0;
};

// An immutable committed scene version. Later updates never change it.
struct RTXPTSceneSnapshot
{
    // Increments on every commit, also when nothing changed.
    Uint64 SnapshotId = 0;
    // Changes when anything that affects the image changed: an enabled instance's existence, transform, pose or
    // materials, enabling, or lights. Repeating equal values does not change it.
    Uint64 ContentRevision = 0;
    // Changes when the analytic lights change.
    Uint64 LightRevision = 0;

    // The live instances, ordered by slot.
    std::vector<std::shared_ptr<const RTXPTSceneInstanceState>> Instances;
    std::shared_ptr<const std::vector<RTXPTSceneLight>>         Lights;
};

using RTXPTSceneSnapshotPtr = std::shared_ptr<const RTXPTSceneSnapshot>;

// Asset repository and runtime scene. Updates are serial: BeginUpdate, setters, then CommitUpdate or CancelUpdate.
// A failed setter leaves the pending update unchanged; a cancelled update publishes nothing and invalidates the
// handles it created. Assets can be registered and released at any time; an instance keeps its assets alive after
// they are released from the repository.
class RTXPTSceneRuntime
{
public:
    RTXPTSceneRuntime();
    ~RTXPTSceneRuntime();
    RTXPTSceneRuntime(const RTXPTSceneRuntime&) = delete;
    RTXPTSceneRuntime& operator=(const RTXPTSceneRuntime&) = delete;

    RTXPTSceneResult RegisterGeometry(const RTXPTGeometryDesc& Desc, RTXPTGeometryHandle& Handle);
    RTXPTSceneResult RegisterMaterial(const RTXPTMaterialDesc& Desc, RTXPTMaterialHandle& Handle);
    RTXPTSceneResult ReleaseGeometry(RTXPTGeometryHandle Handle);
    RTXPTSceneResult ReleaseMaterial(RTXPTMaterialHandle Handle);
    // Mesh and slot counts of a registered geometry, for hosts that create instances of imported assets.
    RTXPTSceneResult GetGeometryInfo(RTXPTGeometryHandle Handle, Uint32& MeshCount, Uint32& MaterialSlotCount, Uint32& JointCount) const;

    RTXPTSceneResult BeginUpdate();
    RTXPTSceneResult CreateInstance(const RTXPTInstanceDesc& Desc, RTXPTInstanceHandle& Handle);
    RTXPTSceneResult SetInstanceTransform(RTXPTInstanceHandle Handle, const float4x4& Transform);
    RTXPTSceneResult SetInstancePose(RTXPTInstanceHandle Handle, const float4x4* pPose, Uint32 MatrixCount);
    RTXPTSceneResult SetInstanceMaterial(RTXPTInstanceHandle Handle, Uint32 Slot, RTXPTMaterialHandle Material);
    RTXPTSceneResult SetInstanceEnabled(RTXPTInstanceHandle Handle, bool Enabled);
    RTXPTSceneResult RemoveInstance(RTXPTInstanceHandle Handle);
    RTXPTSceneResult SetLights(const RTXPTSceneLight* pLights, Uint32 LightCount);
    RTXPTSceneResult CommitUpdate(RTXPTSceneSnapshotPtr& Snapshot);
    void             CancelUpdate();

    bool                  IsUpdating() const;
    RTXPTSceneSnapshotPtr GetSnapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

// Whether Transform is a finite, invertible affine transform (last column 0, 0, 0, 1).
bool IsValidRTXPTInstanceTransform(const float4x4& Transform);

} // namespace Diligent
