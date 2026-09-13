#ifndef __STABLE_PLANES_HLSLI__
#define __STABLE_PLANES_HLSLI__

#include "Config.h"
#include "PathTracerShared.h"

#if !defined(__cplusplus)
#    include "PathState.hlsli"
#endif

static const uint cStablePlaneMaxVertexIndex   = 15;
static const uint cStablePlaneInvalidBranchID  = 0xFFFFFFFF;
static const uint cStablePlaneEnqueuedBranchID = 0xFFFFFFFE;
static const uint cStablePlaneJustStartedID    = 0;

uint StablePlanesAdvanceBranchID(const uint prevStableBranchID, const uint deltaLobeID);
uint StablePlanesGetParentLobeID(const uint stableBranchID);
uint StablePlanesVertexIndexFromBranchID(const uint stableBranchID);
bool StablePlaneIsOnPlane(const uint planeBranchID, const uint vertexBranchID);
bool StablePlaneIsOnStablePath(const uint planeBranchID, const uint planeVertexIndex, const uint vertexBranchID, const uint vertexIndex);
bool StablePlaneIsOnStablePath(const uint planeBranchID, const uint vertexBranchID);
float3 StablePlaneDebugVizColor(const uint planeIndex);

#if !defined(__cplusplus)
static const float kSpecHeuristicBoost = 1.0f;
#endif

struct StablePlane
{
    float3 RayOrigin;
    float  LastRayTCurrent;
    float3 RayDir;
    float  SceneLength;
    uint3  PackedThpAndMVs;
    uint   VertexIndexAndRoughness;
    uint3  DenoiserPackedBSDFEstimate;
    uint   PackedNormal;
    uint2  PackedNoisyRadianceAndSpecAvg;
    uint   FlagsAndVertexIndex;
    uint   PackedCounters;

#if !defined(__cplusplus)
    bool IsEmpty() { return (VertexIndexAndRoughness >> 16) == 0; }
    float3 GetNormal() { return OctToNDirUnorm32(PackedNormal); }
    float GetRoughness() { return f16tof32(VertexIndexAndRoughness & 0xffff); }
    float3 GetNoisyRadiance() { return Fp16ToFp32(PackedNoisyRadianceAndSpecAvg).xyz; }
    float4 GetNoisyRadianceAndSpecRA() { return Fp16ToFp32(PackedNoisyRadianceAndSpecAvg); }

    float3 GetNoisyDiffRadiance()
    {
        float4 l        = Fp16ToFp32(PackedNoisyRadianceAndSpecAvg);
        float  totalAvg = Average(l.rgb);
        return l.rgb * saturate(1.0 - (l.a * kSpecHeuristicBoost) / (totalAvg + 1e-12)).xxx;
    }

    float3 GetNoisySpecRadiance()
    {
        float4 l        = Fp16ToFp32(PackedNoisyRadianceAndSpecAvg);
        float  totalAvg = Average(l.rgb);
        return l.rgb * saturate((l.a * kSpecHeuristicBoost) / (totalAvg + 1e-12)).xxx;
    }
#endif

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_STABLE_PLANES
    void PackCustomPayload(const uint4 packed[5]);
    void UnpackCustomPayload(inout uint4 packed[5]);
#endif
};

struct StablePlanesContext
{
#if !defined(__cplusplus)
    RWTexture2D<float4>             StableRadianceUAV;
    RWTexture2DArray<uint>          StablePlanesHeaderUAV;
    RWStructuredBuffer<StablePlane> StablePlanesUAV;
    PathTracerConstants             PTConstants;

    static StablePlanesContext make(RWTexture2DArray<uint> stablePlanesHeaderUAV,
                                    RWStructuredBuffer<StablePlane> stablePlanesUAV,
                                    RWTexture2D<float4> stableRadianceUAV,
                                    PathTracerConstants ptConstants)
    {
        StablePlanesContext ret;
        ret.StablePlanesHeaderUAV = stablePlanesHeaderUAV;
        ret.StablePlanesUAV       = stablePlanesUAV;
        ret.StableRadianceUAV     = stableRadianceUAV;
        ret.PTConstants           = ptConstants;
        return ret;
    }

    static uint ComputeDominantAddress(uint2 pixelPos,
                                       RWTexture2DArray<uint> stablePlanesHeaderUAV,
                                       RWStructuredBuffer<StablePlane> stablePlanesUAV,
                                       RWTexture2D<float4> stableRadianceUAV,
                                       PathTracerConstants ptConstants)
    {
        StablePlanesContext stablePlanes = StablePlanesContext::make(
            stablePlanesHeaderUAV, stablePlanesUAV, stableRadianceUAV, ptConstants);
        uint dominantStablePlaneIndex = stablePlanes.LoadDominantIndex(pixelPos);
        return stablePlanes.PixelToAddress(pixelPos, dominantStablePlaneIndex);
    }

    uint PixelToAddress(uint2 pixelPos, uint planeIndex)
    {
        return GenericTSPixelToAddress(pixelPos,
                                       planeIndex,
                                       PTConstants.genericTSLineStride,
                                       PTConstants.genericTSPlaneStride);
    }

    uint PixelToAddress(uint2 pixelPos) { return PixelToAddress(pixelPos, 0); }

    StablePlane LoadStablePlane(const uint2 pixelPos, const uint planeIndex)
    {
        uint address = PixelToAddress(pixelPos, planeIndex);
        return StablePlanesUAV[address];
    }

    uint GetBranchID(const uint2 pixelPos, const uint planeIndex)
    {
        return StablePlanesHeaderUAV[uint3(pixelPos, planeIndex)];
    }

    void SetBranchID(const uint2 pixelPos, const uint planeIndex, uint stableBranchID)
    {
        StablePlanesHeaderUAV[uint3(pixelPos, planeIndex)] = stableBranchID;
    }

    static void UnpackStablePlane(const StablePlane sp,
                                  out uint vertexIndex,
                                  out float3 rayOrigin,
                                  out float3 rayDir,
                                  out float sceneLength,
                                  out float3 thp,
                                  out float3 motionVectors)
    {
        vertexIndex = sp.VertexIndexAndRoughness >> 16;
        rayOrigin   = sp.RayOrigin;
        sceneLength = sp.SceneLength;
        rayDir      = sp.RayDir;
        UnpackTwoFp32ToFp16(sp.PackedThpAndMVs, thp, motionVectors);
    }

    void LoadStablePlane(const uint2 pixelPos,
                         const uint planeIndex,
                         out uint vertexIndex,
                         out float3 rayOrigin,
                         out float3 rayDir,
                         out uint stableBranchID,
                         out float sceneLength,
                         out float3 thp,
                         out float3 motionVectors)
    {
        stableBranchID = GetBranchID(pixelPos, planeIndex);
        UnpackStablePlane(LoadStablePlane(pixelPos, planeIndex),
                          vertexIndex,
                          rayOrigin,
                          rayDir,
                          sceneLength,
                          thp,
                          motionVectors);
    }

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_STABLE_PLANES
    void StoreStableRadiance(uint2 pixelPos, float3 radiance)
    {
        StableRadianceUAV[pixelPos] = float4(clamp(radiance, 0.xxx, HLF_MAX.xxx), 0.0);
    }

    void AccumulateStableRadiance(uint2 pixelPos, float3 radiance)
    {
        StableRadianceUAV[pixelPos].xyz += radiance;
    }
#endif

    float3 LoadStableRadiance(uint2 pixelPos) { return StableRadianceUAV[pixelPos].xyz; }

    void StoreFirstHitRayLengthAndClearDominantToZero(uint2 pixelPos, float length)
    {
        StablePlanesHeaderUAV[uint3(pixelPos, 3)] = asuint(min(kMaxRayTravel, length)) & 0xFFFFFFFC;
    }

    float LoadFirstHitRayLength(uint2 pixelPos)
    {
        return asfloat(StablePlanesHeaderUAV[uint3(pixelPos, 3)] & 0xFFFFFFFC);
    }

    void StoreDominantIndex(uint2 pixelPos, uint index)
    {
        StablePlanesHeaderUAV[uint3(pixelPos, 3)] =
            (StablePlanesHeaderUAV[uint3(pixelPos, 3)] & 0xFFFFFFFC) | (0x3 & index);
    }

    uint LoadDominantIndex(uint2 pixelPos)
    {
        return StablePlanesHeaderUAV[uint3(pixelPos, 3)] & 0x3;
    }

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_STABLE_PLANES
    void StoreStablePlane(const uint2 pixelPos,
                          const uint planeIndex,
                          const uint vertexIndex,
                          const float3 rayOrigin,
                          const float3 rayDir,
                          const uint stableBranchID,
                          const float sceneLength,
                          const float rayTCurrent,
                          const float3 thp,
                          const float3 motionVectors,
                          const float roughness,
                          const float3 worldNormal,
                          const float3 diffBSDFEstimate,
                          const float3 specBSDFEstimate,
                          bool dominantSP,
                          uint flagsAndVertexIndex,
                          uint packedCounters)
    {
        uint address = PixelToAddress(pixelPos, planeIndex);
        StablePlane sp;
        sp.RayOrigin               = rayOrigin;
        sp.RayDir                  = rayDir;
        sp.SceneLength             = sceneLength;
        sp.VertexIndexAndRoughness = (vertexIndex << 16) | f32tof16(roughness);
        sp.PackedThpAndMVs         = PackTwoFp32ToFp16(thp, motionVectors);

        const float kNRDMinReflectance = 0.04f;
        const float kNRDMaxReflectance = 6.5504e+4F;
        const float3 fullDiffBSDFEstimate = clamp(diffBSDFEstimate, kNRDMinReflectance.xxx, kNRDMaxReflectance.xxx);
        const float3 fullSpecBSDFEstimate = clamp(specBSDFEstimate, kNRDMinReflectance.xxx, kNRDMaxReflectance.xxx);

        // Shared NRD/RR material guides: directional BSDF estimates, without path
        // throughput or exposure. RR mixes planes later in DLSSRRDenoiserPrepareInputs.
        sp.DenoiserPackedBSDFEstimate    = PackTwoFp32ToFp16(fullDiffBSDFEstimate, fullSpecBSDFEstimate);
        sp.PackedNormal                  = NDirToOctUnorm32(worldNormal);
        sp.PackedNoisyRadianceAndSpecAvg = Fp32ToFp16(float4(0, 0, 0, 0));
        sp.LastRayTCurrent               = rayTCurrent;
        sp.FlagsAndVertexIndex           = flagsAndVertexIndex;
        sp.PackedCounters                = packedCounters;
        StablePlanesUAV[address]         = sp;
        SetBranchID(pixelPos, planeIndex, stableBranchID);

        if (dominantSP && planeIndex != 0)
            StoreDominantIndex(pixelPos, planeIndex);
    }

    void StoreExplorationStart(uint2 pixelPos, uint planeIndex, const uint4 pathPayload[5])
    {
        uint address = PixelToAddress(pixelPos, planeIndex);
        StablePlane sp;
        sp.PackCustomPayload(pathPayload);
        StablePlanesUAV[address] = sp;
        SetBranchID(pixelPos, planeIndex, cStablePlaneEnqueuedBranchID);
    }

    void ExplorationStart(uint2 pixelPos, uint planeIndex, inout uint4 pathPayload[5])
    {
        uint address = PixelToAddress(pixelPos, planeIndex);
        StablePlane sp = StablePlanesUAV[address];
        sp.UnpackCustomPayload(pathPayload);
        SetBranchID(pixelPos, planeIndex, cStablePlaneJustStartedID);
    }

    int FindNextToExplore(uint2 pixelPos, uint fromPlane)
    {
        for (int i = fromPlane; i < cStablePlaneCount; i++)
            if (GetBranchID(pixelPos, i) == cStablePlaneEnqueuedBranchID)
                return i;
        return -1;
    }

    void GetAvailableEmptyPlanes(const uint2 pixelPos, inout int availableCount, inout int availablePlanes[cStablePlaneCount])
    {
        availableCount = 0;
        for (int i = 1; i < min(PTConstants.GetActiveStablePlaneCount(), cStablePlaneCount); i++)
            if (GetBranchID(pixelPos, i) == cStablePlaneInvalidBranchID)
                availablePlanes[availableCount++] = i;
    }
#endif

    void StartPixel(uint2 pixelPos)
    {
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_STABLE_PLANES
        StoreStableRadiance(pixelPos, 0.xxx);
        StablePlanesHeaderUAV[uint3(pixelPos, 0)] = cStablePlaneInvalidBranchID;
        StablePlanesHeaderUAV[uint3(pixelPos, 1)] = cStablePlaneInvalidBranchID;
        StablePlanesHeaderUAV[uint3(pixelPos, 2)] = cStablePlaneInvalidBranchID;
#endif
    }

#if PATH_TRACER_MODE == PATH_TRACER_MODE_FILL_STABLE_PLANES
    void CommitDenoiserRadiance(inout PathState path)
    {
        const uint2 pixelPos       = path.GetPixelPos();
        const uint  planeIndex     = path.getStablePlaneIndex();
        uint        address        = PixelToAddress(pixelPos, planeIndex);
        float4      accumRadiance  = path.GetL();
        const uint2 existingPacked = StablePlanesUAV[address].PackedNoisyRadianceAndSpecAvg;
        if (existingPacked.x != 0 && existingPacked.y != 0)
            accumRadiance += Fp16ToFp32(existingPacked);
        StablePlanesUAV[address].PackedNoisyRadianceAndSpecAvg = Fp32ToFp16(accumRadiance);
        path.SetL(float4(0, 0, 0, 0));
    }
#endif

    float3 GetAllRadiance(uint2 pixelPos, bool includeNoisy = true)
    {
        float3 pathL = LoadStableRadiance(pixelPos);
        if (includeNoisy)
        {
            for (int i = 0; i < cStablePlaneCount; i++)
            {
                if (GetBranchID(pixelPos, i) == cStablePlaneInvalidBranchID)
                    continue;
                pathL += StablePlanesUAV[PixelToAddress(pixelPos, i)].GetNoisyRadiance();
            }
        }
        return pathL;
    }
#endif
};

inline uint StablePlanesAdvanceBranchID(const uint prevStableBranchID, const uint deltaLobeID)
{
    return (prevStableBranchID << 2) | deltaLobeID;
}

inline uint StablePlanesGetParentLobeID(const uint stableBranchID)
{
    return stableBranchID & 0x3;
}

inline uint StablePlanesVertexIndexFromBranchID(const uint stableBranchID)
{
#if defined(__cplusplus)
    uint v = stableBranchID;
    uint r = 0;
    while (v >>= 1)
        r++;
    return r / 2 + 1;
#else
    return firstbithigh(stableBranchID) / 2 + 1;
#endif
}

inline bool StablePlaneIsOnPlane(const uint planeBranchID, const uint vertexBranchID)
{
    return planeBranchID == vertexBranchID;
}

inline bool StablePlaneIsOnStablePath(const uint planeBranchID, const uint planeVertexIndex, const uint vertexBranchID, const uint vertexIndex)
{
    if (vertexIndex > planeVertexIndex)
        return false;
    return (planeBranchID >> ((planeVertexIndex - vertexIndex) * 2)) == vertexBranchID;
}

inline bool StablePlaneIsOnStablePath(const uint planeBranchID, const uint vertexBranchID)
{
    return StablePlaneIsOnStablePath(planeBranchID,
                                     StablePlanesVertexIndexFromBranchID(planeBranchID),
                                     vertexBranchID,
                                     StablePlanesVertexIndexFromBranchID(vertexBranchID));
}

inline float3 StablePlaneDebugVizColor(const uint planeIndex)
{
    return float3(planeIndex == 0 || planeIndex == 3, planeIndex == 1, planeIndex == 2 || planeIndex == 3);
}

#if !defined(__cplusplus)
inline uint3 StablePlaneDebugVizFourWaySplitCoord(const int dbgPlaneIndex, const uint2 pixelPos, const uint2 screenSize)
{
    if (dbgPlaneIndex >= 0)
        return uint3(pixelPos.xy, dbgPlaneIndex);

    const uint2 halfSize = screenSize / 2;
    const uint2 quadrant = pixelPos >= halfSize;
    uint3 ret;
    ret.xy = (pixelPos - quadrant * halfSize) * 2.0;
    ret.z  = quadrant.x + quadrant.y * 2;
    return ret;
}
#endif

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_STABLE_PLANES
void StablePlane::PackCustomPayload(const uint4 packed[5])
{
    RayOrigin                     = asfloat(packed[0].xyz);
    LastRayTCurrent               = asfloat(packed[0].w);
    RayDir                        = asfloat(packed[1].xyz);
    SceneLength                   = asfloat(packed[1].w);
    PackedThpAndMVs               = packed[2].xyz;
    VertexIndexAndRoughness       = packed[2].w;
    DenoiserPackedBSDFEstimate    = packed[3].xyz;
    PackedNormal                  = packed[3].w;
    PackedNoisyRadianceAndSpecAvg = packed[4].xy;
    FlagsAndVertexIndex           = packed[4].z;
    PackedCounters                = packed[4].w;
}

void StablePlane::UnpackCustomPayload(inout uint4 packed[5])
{
    packed[0].xyz = asuint(RayOrigin);
    packed[0].w   = asuint(LastRayTCurrent);
    packed[1].xyz = asuint(RayDir);
    packed[1].w   = asuint(SceneLength);
    packed[2].xyz = PackedThpAndMVs;
    packed[2].w   = VertexIndexAndRoughness;
    packed[3].xyz = DenoiserPackedBSDFEstimate;
    packed[3].w   = PackedNormal;
    packed[4].xy  = PackedNoisyRadianceAndSpecAvg;
    packed[4].z   = FlagsAndVertexIndex;
    packed[4].w   = PackedCounters;
}
#endif

#endif // __STABLE_PLANES_HLSLI__
