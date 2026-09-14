#ifndef __PATH_TRACER_SHARED_H__
#define __PATH_TRACER_SHARED_H__

// Mirrors Diligent::kSubInstanceFlag_Indexed in RTXPTAccelerationStructures.hpp.
static const uint kSubInstanceFlagIndexed = 0x1u;
static const uint kSubInstanceFlagSkinned = 0x2u;

// Maximum area-light solid-angle pdf (G4). Mirrors RTXPT-fork PolymorphicLight.hlsli:25 MAX_SOLID_ANGLE_PDF;
// clamps the area->solid-angle conversion for near-grazing / very close emissive-triangle samples.
static const float kMaxSolidAnglePdf = 1e10;

struct PathTracerCameraData
{
    float3 PosW;
    float  NearZ;
    float3 DirectionW;
    float  PixelConeSpreadAngle;
    float3 CameraU;
    float  FarZ;
    float3 CameraV;
    float  FocalDistance;
    float3 CameraW;
    float  AspectRatio;
    uint2  ViewportSize;
    float  ApertureRadius;
    float  _padding0;
    float2 Jitter;
    float  _padding1;
    float  _padding2;
};

struct PathTracerViewData
{
    float4x4 MatWorldToView;
    float4x4 MatViewToClip;
    float4x4 MatWorldToClip;
    float4x4 MatWorldToClipNoOffset;
    float4x4 MatClipToWorldNoOffset;
    float2   ViewportOrigin;
    float2   ViewportSize;
    float2   ViewportSizeInv;
    float2   PixelOffset;
    float2   ClipToWindowScale;
    float2   ClipToWindowBias;
};

// Mirrors Diligent::PathTracerConstants in RTXPTFrameConstants.hpp.
struct PathTracerConstants
{
    uint  imageWidth;
    uint  imageHeight;
    uint  sampleBaseIndex;
    float perPixelJitterAAScale;

    uint  bounceCount;
    uint  diffuseBounceCount;
    float EnvironmentMapDiffuseSampleMIPLevel;
    float texLODBias;

    float invSubSampleCount;
    float fireflyFilterThreshold;
    float preExposedGrayLuminance;
    uint  denoisingEnabled;

    uint frameIndex;
    uint useReSTIRDI;
    uint useReSTIRGI;
    uint resetAccumulation;

    float stablePlanesSplitStopThreshold;
    float _padding3;
    uint  _padding4;
    float stablePlanesSuppressPrimaryIndirectSpecularK;

    float denoiserRadianceClampK;
    float DLSSRRBrightnessClampK; // RR input max-channel clamp; zero disables the user clamp.
    float stablePlanesAntiAliasingFallthrough;
    uint  _activeStablePlaneCount;

    uint maxStablePlaneVertexDepth;
    uint allowPrimarySurfaceReplacement;
    uint genericTSLineStride;
    uint genericTSPlaneStride;

    uint NEEEnabled;
    uint NEEType;
    uint NEECandidateSamples;
    uint NEEFullSamples;

    uint  sampleIndex;
    uint  minBounceCount;
    uint  environmentNEEEnabled;
    float environmentIntensity;

    float lightIntensityScale;
    uint  maxNEEBounceCount;
    uint  analyticLightCount;
    uint  NEEMISType;

    uint nestedDielectricsQuality;
    uint superResolutionActive;
    uint _paddingR6_1;
    uint _paddingR6_2;

    PathTracerCameraData camera;
    PathTracerCameraData prevCamera;

    uint GetActiveStablePlaneCount()
    {
#if defined(RTXPT_ACTIVE_STABLE_PLANE_COUNT)
        return RTXPT_ACTIVE_STABLE_PLANE_COUNT;
#else
        return _activeStablePlaneCount;
#endif
    }
};

struct RTXPTEnvMapConstants
{
    float4 LocalToWorld0;
    float4 LocalToWorld1;
    float4 LocalToWorld2;
    float4 WorldToLocal0;
    float4 WorldToLocal1;
    float4 WorldToLocal2;
    float4 ColorEnabled;
    float4 ImportanceMetadata;
};

// Mirrors Diligent::SampleConstants in RTXPTFrameConstants.hpp.
struct SampleConstants
{
    float4x4             viewProj;
    float4x4             viewProjInv;
    float4               cameraPositionAndTime;
    float4               viewportSizeAndFrameIndex;
    PathTracerViewData   view;
    PathTracerViewData   previousView;
    PathTracerCameraData camera;
    PathTracerConstants  ptConsts;
    RTXPTEnvMapConstants envMap;
};

struct SampleMiniConstants
{
    uint4 params;
    uint4 params1;
    uint4 params2;
    uint4 params3;
};

static const uint RTXPT_GENERIC_TS_TILE_SIZE = 8u;

uint RTXPTGenericTSComputeLineStride(uint imageWidth)
{
    const uint safeWidth  = max(imageWidth, 1u);
    const uint tileCountX = (safeWidth + RTXPT_GENERIC_TS_TILE_SIZE - 1u) / RTXPT_GENERIC_TS_TILE_SIZE;
    return tileCountX * RTXPT_GENERIC_TS_TILE_SIZE;
}

uint RTXPTGenericTSComputePlaneStride(uint imageWidth, uint imageHeight)
{
    const uint safeHeight = max(imageHeight, 1u);
    const uint tileCountY = (safeHeight + RTXPT_GENERIC_TS_TILE_SIZE - 1u) / RTXPT_GENERIC_TS_TILE_SIZE;
    return RTXPTGenericTSComputeLineStride(imageWidth) * tileCountY * RTXPT_GENERIC_TS_TILE_SIZE;
}

// Primary ray payload (Phase 5.1 compatibility - kept for the bridge sanity helpers).
struct PrimaryPayload
{
    float4 ColorDepth;
};

// Legacy material-hit payload kept for compatibility notes and size audits. Reference primary and
// visibility rays now use PathPayload.hlsli.
// Size is 160 bytes (40 floats); keep RTXPTRayTracingPass::Initialize MaxPayloadSize in sync when this changes.
struct RTXPTMaterialHitPayload
{
    float3 worldPos;
    float  hitDistance;

    float3 worldNormal; // Shading normal oriented against the incoming ray.
    uint   hitFlag;

    float3 faceNormal; // Geometric face normal oriented against the incoming ray.
    uint   materialID;

    float3 baseColor;
    float  metallic;

    float3 emission;
    float  roughness;

    float emissiveLightPdf;
    float ior;
    float transmissionFactor;
    float diffuseTransmissionFactor;

    float3 transmissionColor;
    float  volumeAttenuationDistance;

    float3 volumeAttenuationColor;
    uint   materialFlags;

    uint  nestedPriority;
    uint  frontFacing;
    uint  thinSurface;
    float alpha;

    float3 vertexNormal;     // Interpolated vertex normal, corrected for face side, before normal mapping.
    float  shadowNoLFadeout; // RTXPT-fork MaterialPT::ShadowNoLFadeout.
};

// Mirrors Diligent::SubInstanceData in RTXPTAccelerationStructures.hpp.
// One entry per (BLAS instance, geometry) pair. The C++ side stores the per-instance
// sub-instance base in TLAS CustomId, exposed in closest-hit shaders as InstanceID().
// index = InstanceID() + GeometryIndex().
struct SubInstanceData
{
    uint MaterialID;
    uint Flags;
    uint IndexOffset;
    uint IndexCount;
    uint VertexOffset;
    uint VertexCount;
    uint emissiveTriangleOffset; // First entry in t_EmissiveTriangles for this sub-instance (G4).
    uint _padding1;
};

// Mirrors Diligent::MaterialPTData in RTXPTMaterials.hpp (must keep order/size in sync; total size 144 bytes).
struct MaterialPTData
{
    float4 baseColorFactor; // offset 0

    float3 emissiveFactor; // offset 16
    float  alphaCutoff;    // offset 28

    uint  flags;                 // offset 32
    uint  baseColorTextureIndex; // offset 36
    uint  emissiveTextureIndex;  // offset 40
    float metallicFactor;        // offset 44

    float roughnessFactor;               // offset 48
    float baseColorTextureSlice;         // offset 52
    float emissiveTextureSlice;          // offset 56
    uint  metallicRoughnessTextureIndex; // offset 60

    float metallicRoughnessTextureSlice; // offset 64
    uint  normalTextureIndex;            // offset 68
    float normalTextureSlice;            // offset 72
    float normalScale;                   // offset 76

    float transmissionFactor;        // offset 80
    float diffuseTransmissionFactor; // offset 84
    float ior;                       // offset 88
    float thicknessFactor;           // offset 92

    float3 volumeAttenuationColor;    // offset 96
    float  volumeAttenuationDistance; // offset 108

    uint  transmissionTextureIndex; // offset 112
    float transmissionTextureSlice; // offset 116
    uint  thicknessTextureIndex;    // offset 120
    float thicknessTextureSlice;    // offset 124

    // RTXPT-fork authored priority: 0 is the special highest-priority value; 14 is the default/max authored value.
    uint  nestedPriority;         // offset 128
    uint  pathDecompositionFlags; // offset 132
    float shadowNoLFadeout;       // offset 136
    float _paddingR7_1;
};

// Mirrors the kMaterialFlag_* constants in RTXPTMaterials.hpp.
static const uint kMaterialFlagHasBaseColorTexture         = 0x1u;
static const uint kMaterialFlagAlphaTested                 = 0x2u;
static const uint kMaterialFlagHasEmissiveTexture          = 0x4u;
static const uint kMaterialFlagHasMetallicRoughnessTexture = 0x8u;
static const uint kMaterialFlagHasNormalTexture            = 0x10u;
static const uint kMaterialFlagEmissiveAreaLight           = 0x20u;
static const uint kMaterialFlagHasTransmission             = 0x40u;
static const uint kMaterialFlagHasTransmissionTexture      = 0x80u;
static const uint kMaterialFlagHasVolume                   = 0x100u;
static const uint kMaterialFlagHasThicknessTexture         = 0x200u;
static const uint kMaterialFlagThinSurface                 = 0x400u;
static const uint kMaterialFlagExcludeFromNEE              = 0x1000u;

static const uint kMaterialPathDecompositionFlagPSDExclude                          = 0x1u;
static const uint kMaterialPathDecompositionFlagPSDBlockMotionVectorsAtSurfaceMask  = 0x6u;
static const uint kMaterialPathDecompositionFlagPSDBlockMotionVectorsAtSurfaceShift = 1u;
static const uint kMaterialPathDecompositionFlagIgnoreMeshTangentSpace              = 0x8u;
static const uint kMaterialPathDecompositionFlagPSDDominantDeltaLobeP1Mask          = 0xF0u;
static const uint kMaterialPathDecompositionFlagPSDDominantDeltaLobeP1Shift         = 4u;

// Mirrors Diligent::PolymorphicLightInfo in RTXPTLights.hpp.
struct PolymorphicLightInfo
{
    float4 colorType;
    float4 positionRadius;
    float4 directionRange;
    float4 shaping;
};

static const uint kPolymorphicLightTypeSphere      = 0u;
static const uint kPolymorphicLightTypeDirectional = 2u;
static const uint kPolymorphicLightTypePoint       = 4u;

static const uint kLightProxyKindAnalytic         = 0u;
static const uint kLightProxyKindEmissiveTriangle = 1u;

// Mirrors Diligent::EmissiveTriangle in RTXPTLights.hpp (total size 64 bytes). One world-space,
// NEE-eligible emissive triangle (constant emitter). Stores base + two edges + radiance like
// RTXPT-fork TriangleLight; the surface normal and area are recomputed on the fly.
struct EmissiveTriangle
{
    float4 base;     // xyz: world-space vertex 0 (.w unused)
    float4 edge1;    // xyz: world-space (vertex1 - vertex0) (.w unused)
    float4 edge2;    // xyz: world-space (vertex2 - vertex0) (.w unused)
    float4 radiance; // rgb: emitted radiance (.w unused)
};

// Unpacked geometry used by shading. Do not use this type in structured buffers:
// Vulkan aligns the second float3 to 16 bytes and gives the element a 48-byte stride.
struct GeometryVertexData
{
    float3 position;
    float3 normal;
    float2 texCoord0;
};

// Default GLTF buffer 0: POSITION + NORMAL + TEXCOORD_0, at offsets 0/12/24.
// Two float4 values preserve the 32-byte CPU layout on both D3D12 and Vulkan.
struct PackedGeometryVertexData
{
    float4 positionAndNormalX;
    float4 normalYZAndTexCoord0;
};

inline GeometryVertexData UnpackGeometryVertex(PackedGeometryVertexData packed)
{
    GeometryVertexData vertex;
    vertex.position  = packed.positionAndNormalX.xyz;
    vertex.normal    = float3(packed.positionAndNormalX.w, packed.normalYZAndTexCoord0.xy);
    vertex.texCoord0 = packed.normalYZAndTexCoord0.zw;
    return vertex;
}

inline PackedGeometryVertexData PackGeometryVertex(GeometryVertexData vertex)
{
    PackedGeometryVertexData packed;
    packed.positionAndNormalX   = float4(vertex.position, vertex.normal.x);
    packed.normalYZAndTexCoord0 = float4(vertex.normal.yz, vertex.texCoord0);
    return packed;
}

// Mirrors the default GLTF skin stream in vertex buffer 1: JOINTS_0 followed by WEIGHTS_0.
struct SkinVertexData
{
    float4 joints;
    float4 weights;
};

#endif // __PATH_TRACER_SHARED_H__
