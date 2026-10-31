#ifndef __PATH_STATE_HLSLI__
#define __PATH_STATE_HLSLI__

#define PATH_STATE_DEFINED

#include "Config.h"
#include "PathTracerHelpers.hlsli"
#include "PathTracerShared.h"
#include "BSDFs/InteriorList.hlsli"
#include "Utils/SampleGenerators.hlsli"

static const uint kVertexIndexBitCount       = 10u;
static const uint kVertexIndexBitMask        = (1u << kVertexIndexBitCount) - 1u;
static const uint kPathFlagsBitCount         = 32u - kVertexIndexBitCount;
static const uint kPathFlagsBitMask          = ((1u << kPathFlagsBitCount) - 1u) << kVertexIndexBitCount;
static const uint kDenoisingLayerIndexBitOffset = 14u + kVertexIndexBitCount;
static const uint kDenoisingLayerIndexBitMask   = ((1u << 2u) - 1u) << kDenoisingLayerIndexBitOffset;

enum class PathFlags
{
    active                          = (1 << 0),
    hit                             = (1 << 1),
    transmission                    = (1 << 2),
    specular                        = (1 << 3),
    delta                           = (1 << 4),
    insideDielectricVolume          = (1 << 5),
    terminateAtNextBounce           = (1 << 6),
    restirGIStarted                 = (1 << 7),
    restirGICollectSecondarySurface = (1 << 8),
    enableThreadReorder             = (1 << 9),
    deltaTransmissionPath           = (1 << 11),
    deltaOnlyPath                   = (1 << 12),
    deltaTreeExplorer               = (1 << 13),
    denoisingLayerIndexBit0            = (1 << 14),
    denoisingLayerIndexBit1            = (1 << 15),
    denoisingLayerOnPlane              = (1 << 16),
    denoisingLayerOnBranch             = (1 << 17),
    denoisingLayerBaseScatterDiff      = (1 << 18),
    exportSpecHitTQueued            = (1 << 19),
    denoisingLayerOnDominantBranch     = (1 << 20),
    unsupportedMotionPath             = (1 << 10),
};

enum class PackedCounters
{
    DiffuseBounces         = 0,
    RejectedHits           = 1,
    BouncesFromDenoisingLayer = 2,
};

struct PathState
{
    uint4 PackOriginId;
    uint4 PackDirSceneLength;

    uint2 pack23;
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
    uint2 imageXformPacked;
    float4 previousReflectionPlane;
#else
    uint2 pack45;
#endif

#if RTXPT_NESTED_DIELECTRICS_QUALITY > 0 || defined(__INTELLISENSE__)
    InteriorList interiorList;
#endif
    uint packedCounters;
    uint stableBranchID;

    RayCone rayCone;
    uint    pack0;
    uint    pack1;
    uint    flagsAndVertexIndex;

    float3 GetOrigin() { return asfloat(PackOriginId.xyz); }
    void SetOrigin(float3 origin) { PackOriginId.xyz = asuint(origin); }

    uint GetId() { return PackOriginId.w; }
    void SetId(uint id) { PackOriginId.w = id; }

    float3 GetDir() { return asfloat(PackDirSceneLength.xyz); }
    void SetDir(float3 dir) { PackDirSceneLength.xyz = asuint(dir); }

    float GetSceneLength() { return asfloat(PackDirSceneLength.w); }
    void SetSceneLength(float sceneLength) { PackDirSceneLength.w = asuint(sceneLength); }

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
    float3x3 GetImageXform() { return UnpackOrthoMatrix(imageXformPacked); }
    void SetImageXform(float3x3 p) { imageXformPacked = PackOrthoMatrix(p); }

    void SetMotionVectorSceneLength(float l) { pack0 = asuint(l); }
    float GetMotionVectorSceneLength() { return asfloat(pack0); }
    lpuint GetPackedMISInfo() { return 0; }
    lpfloat GetBsdfScatterPdf() { return 0.0; }
    lpfloat GetThpRuRuCorrection() { return 1.0; }
#else
    void SetFireflyFilterK_BsdfScatterPdf(float fireflyFilterK, float bsdfScatterPdf)
    {
        pack0 = (f32tof16(clamp(fireflyFilterK, 0.0, HLF_MAX)) << 16) |
            f32tof16(clamp(bsdfScatterPdf, 0.0, HLF_MAX));
    }
    lpfloat GetFireflyFilterK() { return lpfloat(f16tof32(pack0 >> 16)); }
    lpfloat GetBsdfScatterPdf() { return lpfloat(f16tof32(pack0 & 0xffff)); }

    void SetPackedMISInfo_ThpRuRuCorrection(uint packedMISInfo, float thpRuRuCorrection)
    {
        pack1 = (packedMISInfo << 16) | f32tof16(clamp(thpRuRuCorrection, 0.0, HLF_MAX));
    }
    lpuint GetPackedMISInfo() { return lpuint(pack1 >> 16); }
    lpfloat GetThpRuRuCorrection() { return lpfloat(f16tof32(pack1 & 0xffff)); }
#endif

    void SetThp(float3 thp)
    {
        pack23 = Fp32ToFp16NoClamp(float4(clamp(thp, 0.xxx, HLF_MAX.xxx), 0.0));
    }
    float3 GetThp() { return Fp16ToFp32(pack23).xyz; }

#if PATH_TRACER_MODE != PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
    void SetL(float4 l)
    {
        pack45 = Fp32ToFp16NoClamp(clamp(l, 0.xxxx, HLF_MAX.xxxx));
    }
    float4 GetL() { return Fp16ToFp32(pack45); }
#endif

    bool isTerminated() { return !isActive(); }
    bool isActive() { return hasFlag(PathFlags::active); }
    bool isHit() { return hasFlag(PathFlags::hit); }
    bool wasScatterTransmission() { return hasFlag(PathFlags::transmission); }
    bool wasScatterSpecular() { return hasFlag(PathFlags::specular); }
    bool wasScatterDelta() { return hasFlag(PathFlags::delta); }
    bool isInsideDielectricVolume() { return hasFlag(PathFlags::insideDielectricVolume); }
    bool isDeltaTransmissionPath() { return hasFlag(PathFlags::deltaTransmissionPath); }
    bool isDeltaOnlyPath() { return hasFlag(PathFlags::deltaOnlyPath); }
    bool isTerminatingAtNextBounce() { return hasFlag(PathFlags::terminateAtNextBounce); }

    void terminate() { setFlag(PathFlags::active, false); }
    void setActive() { setFlag(PathFlags::active); }
    void clearHit() { setFlag(PathFlags::hit, false); }
    void setTerminateAtNextBounce() { setFlag(PathFlags::terminateAtNextBounce); }

    void clearScatterEventFlags()
    {
        const uint bits = (((uint)PathFlags::transmission) |
                           ((uint)PathFlags::specular) |
                           ((uint)PathFlags::delta)) << kVertexIndexBitCount;
        flagsAndVertexIndex &= ~bits;
    }

    void setScatterTransmission(bool value = true) { setFlag(PathFlags::transmission, value); }
    void setScatterSpecular(bool value = true) { setFlag(PathFlags::specular, value); }
    void setScatterDelta(bool value = true) { setFlag(PathFlags::delta, value); }
    void setInsideDielectricVolume(bool value = true) { setFlag(PathFlags::insideDielectricVolume, value); }
    void setDeltaTransmissionPath(bool value = true) { setFlag(PathFlags::deltaTransmissionPath, value); }
    void setDeltaOnlyPath(bool value = true) { setFlag(PathFlags::deltaOnlyPath, value); }

    bool hasFlag(PathFlags flag)
    {
        const uint bit = ((uint)flag) << kVertexIndexBitCount;
        return (flagsAndVertexIndex & bit) != 0;
    }

    void setFlag(PathFlags flag, bool value = true)
    {
        const uint bit = ((uint)flag) << kVertexIndexBitCount;
        if (value)
            flagsAndVertexIndex |= bit;
        else
            flagsAndVertexIndex &= ~bit;
    }

    uint getCounter(PackedCounters type)
    {
        const uint shift = ((uint)type) << 3;
        return (packedCounters >> shift) & 0xff;
    }

    void setCounter(PackedCounters type, uint bounces)
    {
        const uint shift = ((uint)type) << 3;
        packedCounters = (packedCounters & ~((uint)0xff << shift)) | ((bounces & 0xff) << shift);
    }

    void incrementCounter(PackedCounters type)
    {
        packedCounters += 1u << (((uint)type) << 3);
    }

    uint2 GetPixelPos() { return PathIDToPixel(GetId()); }

    void setVertexIndex(uint index)
    {
        flagsAndVertexIndex &= kPathFlagsBitMask;
        flagsAndVertexIndex |= index;
    }

    uint getVertexIndex() { return flagsAndVertexIndex & kVertexIndexBitMask; }
    void incrementVertexIndex() { flagsAndVertexIndex += 1; }
    void decrementVertexIndex() { flagsAndVertexIndex -= 1; }

    Ray getScatterRay()
    {
        return Ray::make(GetOrigin(), GetDir(), 0.0, kMaxRayTravel);
    }

    uint getDenoisingLayerIndex() { return (flagsAndVertexIndex & kDenoisingLayerIndexBitMask) >> kDenoisingLayerIndexBitOffset; }
    void setDenoisingLayerIndex(uint index)
    {
        flagsAndVertexIndex &= ~kDenoisingLayerIndexBitMask;
        flagsAndVertexIndex |= index << kDenoisingLayerIndexBitOffset;
    }
};

#endif // __PATH_STATE_HLSLI__
