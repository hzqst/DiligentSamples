#ifndef __DENOISING_LAYERS_HLSLI__
#define __DENOISING_LAYERS_HLSLI__

#include "Config.h"
#include "PathTracerShared.h"

#if !defined(__cplusplus)
#    include "PathState.hlsli"
#endif

static const uint cDenoisingLayerMaxVertexIndex   = 15;
static const uint cDenoisingLayerInvalidBranchID  = 0xFFFFFFFF;
static const uint cDenoisingLayerEnqueuedBranchID = 0xFFFFFFFE;
static const uint cDenoisingLayerJustStartedID    = 0;

uint DenoisingLayersAdvanceBranchID(const uint prevStableBranchID, const uint deltaLobeID);
uint DenoisingLayersGetParentLobeID(const uint stableBranchID);
uint DenoisingLayersVertexIndexFromBranchID(const uint stableBranchID);
bool DenoisingLayerIsOnPlane(const uint planeBranchID, const uint vertexBranchID);
bool DenoisingLayerIsOnStablePath(const uint planeBranchID, const uint planeVertexIndex, const uint vertexBranchID, const uint vertexIndex);
bool DenoisingLayerIsOnStablePath(const uint planeBranchID, const uint vertexBranchID);
float3 DenoisingLayerDebugVizColor(const uint planeIndex);

#if !defined(__cplusplus)
static const float kSpecHeuristicBoost = 1.0f;
#endif

struct DenoisingLayer
{
    float3 RayOrigin;
    float  LastRayTCurrent;
    float3 RayDir;
    float  SceneLength;
    uint3  PackedThpAndMVs; // fp16 throughput and virtual world-space motion
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
    void GetThpAndWorldMotionVector(out float3 thp, out float3 toPrevPosW) { UnpackTwoFp32ToFp16(PackedThpAndMVs, thp, toPrevPosW); }
    float3 GetWorldMotionVector() { float3 thp, motion; GetThpAndWorldMotionVector(thp, motion); return motion; }

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

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
    void PackCustomPayload(const uint4 packed[5]);
    void UnpackCustomPayload(inout uint4 packed[5]);
#endif
};

struct DenoisingLayersContext
{
#if !defined(__cplusplus)
    RWTexture2D<float4>             StableRadianceUAV;
    RWTexture2DArray<uint>          DenoisingLayersHeaderUAV;
    RWStructuredBuffer<DenoisingLayer> DenoisingLayersUAV;
    PathTracerConstants             PTConstants;

    static DenoisingLayersContext make(RWTexture2DArray<uint> denoisingLayersHeaderUAV,
                                    RWStructuredBuffer<DenoisingLayer> denoisingLayersUAV,
                                    RWTexture2D<float4> stableRadianceUAV,
                                    PathTracerConstants ptConstants)
    {
        DenoisingLayersContext ret;
        ret.DenoisingLayersHeaderUAV = denoisingLayersHeaderUAV;
        ret.DenoisingLayersUAV       = denoisingLayersUAV;
        ret.StableRadianceUAV     = stableRadianceUAV;
        ret.PTConstants           = ptConstants;
        return ret;
    }

    static uint ComputeDominantAddress(uint2 pixelPos,
                                       RWTexture2DArray<uint> denoisingLayersHeaderUAV,
                                       RWStructuredBuffer<DenoisingLayer> denoisingLayersUAV,
                                       RWTexture2D<float4> stableRadianceUAV,
                                       PathTracerConstants ptConstants)
    {
        DenoisingLayersContext denoisingLayers = DenoisingLayersContext::make(
            denoisingLayersHeaderUAV, denoisingLayersUAV, stableRadianceUAV, ptConstants);
        uint dominantDenoisingLayerIndex = denoisingLayers.LoadDominantIndex(pixelPos);
        return denoisingLayers.PixelToAddress(pixelPos, dominantDenoisingLayerIndex);
    }

    uint PixelToAddress(uint2 pixelPos, uint planeIndex)
    {
        return GenericTSPixelToAddress(pixelPos,
                                       planeIndex,
                                       PTConstants.genericTSLineStride,
                                       PTConstants.genericTSPlaneStride);
    }

    uint PixelToAddress(uint2 pixelPos) { return PixelToAddress(pixelPos, 0); }

    DenoisingLayer LoadDenoisingLayer(const uint2 pixelPos, const uint planeIndex)
    {
        uint address = PixelToAddress(pixelPos, planeIndex);
        return DenoisingLayersUAV[address];
    }

    uint GetBranchID(const uint2 pixelPos, const uint planeIndex)
    {
        return DenoisingLayersHeaderUAV[uint3(pixelPos, planeIndex)];
    }

    void SetBranchID(const uint2 pixelPos, const uint planeIndex, uint stableBranchID)
    {
        DenoisingLayersHeaderUAV[uint3(pixelPos, planeIndex)] = stableBranchID;
    }

    static void UnpackDenoisingLayer(const DenoisingLayer sp,
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

    void LoadDenoisingLayer(const uint2 pixelPos,
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
        UnpackDenoisingLayer(LoadDenoisingLayer(pixelPos, planeIndex),
                          vertexIndex,
                          rayOrigin,
                          rayDir,
                          sceneLength,
                          thp,
                          motionVectors);
    }

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
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
        DenoisingLayersHeaderUAV[uint3(pixelPos, 3)] = asuint(min(kMaxRayTravel, length)) & 0xFFFFFFFC;
    }

    float LoadFirstHitRayLength(uint2 pixelPos)
    {
        return asfloat(DenoisingLayersHeaderUAV[uint3(pixelPos, 3)] & 0xFFFFFFFC);
    }

    void StoreDominantIndex(uint2 pixelPos, uint index)
    {
        DenoisingLayersHeaderUAV[uint3(pixelPos, 3)] =
            (DenoisingLayersHeaderUAV[uint3(pixelPos, 3)] & 0xFFFFFFFC) | (0x3 & index);
    }

    uint LoadDominantIndex(uint2 pixelPos)
    {
        return DenoisingLayersHeaderUAV[uint3(pixelPos, 3)] & 0x3;
    }

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
    void StoreDenoisingLayer(const uint2 pixelPos,
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
        DenoisingLayer sp;
        sp.RayOrigin               = rayOrigin;
        sp.RayDir                  = rayDir;
        sp.SceneLength             = sceneLength;
        sp.VertexIndexAndRoughness = (vertexIndex << 16) | f32tof16(roughness);
        sp.PackedThpAndMVs         = PackTwoFp32ToFp16(thp, motionVectors);

        const float kNRDMaxReflectance = 6.5504e+4F;
        // Each denoiser applies its own reflectance floor when preparing guides.
        const float3 fullDiffBSDFEstimate = clamp(diffBSDFEstimate, 0.0.xxx, kNRDMaxReflectance.xxx);
        const float3 fullSpecBSDFEstimate = clamp(specBSDFEstimate, 0.0.xxx, kNRDMaxReflectance.xxx);

        // Shared NRD/RR material guides: directional BSDF estimates, without path
        // throughput or exposure. RR mixes planes later in DLSSRRDenoiserPrepareInputs.
        sp.DenoiserPackedBSDFEstimate    = PackTwoFp32ToFp16(fullDiffBSDFEstimate, fullSpecBSDFEstimate);
        sp.PackedNormal                  = NDirToOctUnorm32(worldNormal);
        sp.PackedNoisyRadianceAndSpecAvg = Fp32ToFp16(float4(0, 0, 0, 0));
        sp.LastRayTCurrent               = rayTCurrent;
        sp.FlagsAndVertexIndex           = flagsAndVertexIndex;
        sp.PackedCounters                = packedCounters;
        DenoisingLayersUAV[address]         = sp;
        SetBranchID(pixelPos, planeIndex, stableBranchID);

        if (dominantSP && planeIndex != 0)
            StoreDominantIndex(pixelPos, planeIndex);
    }

    void StoreExplorationStart(uint2 pixelPos, uint planeIndex, const uint4 pathPayload[5])
    {
        uint address = PixelToAddress(pixelPos, planeIndex);
        DenoisingLayer sp;
        sp.PackCustomPayload(pathPayload);
        DenoisingLayersUAV[address] = sp;
        SetBranchID(pixelPos, planeIndex, cDenoisingLayerEnqueuedBranchID);
    }

    void ExplorationStart(uint2 pixelPos, uint planeIndex, inout uint4 pathPayload[5])
    {
        uint address = PixelToAddress(pixelPos, planeIndex);
        DenoisingLayer sp = DenoisingLayersUAV[address];
        sp.UnpackCustomPayload(pathPayload);
        SetBranchID(pixelPos, planeIndex, cDenoisingLayerJustStartedID);
    }

    int FindNextToExplore(uint2 pixelPos, uint fromPlane)
    {
        for (int i = fromPlane; i < cDenoisingLayerCount; i++)
            if (GetBranchID(pixelPos, i) == cDenoisingLayerEnqueuedBranchID)
                return i;
        return -1;
    }

    void GetAvailableEmptyPlanes(const uint2 pixelPos, inout int availableCount, inout int availablePlanes[cDenoisingLayerCount])
    {
        availableCount = 0;
        for (int i = 1; i < min(PTConstants.GetActiveDenoisingLayerCount(), cDenoisingLayerCount); i++)
            if (GetBranchID(pixelPos, i) == cDenoisingLayerInvalidBranchID)
                availablePlanes[availableCount++] = i;
    }
#endif

    void StartPixel(uint2 pixelPos)
    {
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        StoreStableRadiance(pixelPos, 0.xxx);
        DenoisingLayersHeaderUAV[uint3(pixelPos, 0)] = cDenoisingLayerInvalidBranchID;
        DenoisingLayersHeaderUAV[uint3(pixelPos, 1)] = cDenoisingLayerInvalidBranchID;
        DenoisingLayersHeaderUAV[uint3(pixelPos, 2)] = cDenoisingLayerInvalidBranchID;
#endif
    }

#if PATH_TRACER_MODE == PATH_TRACER_MODE_FILL_DENOISING_LAYERS
    void CommitDenoiserRadiance(inout PathState path)
    {
        const uint2 pixelPos       = path.GetPixelPos();
        const uint  planeIndex     = path.getDenoisingLayerIndex();
        uint        address        = PixelToAddress(pixelPos, planeIndex);
        float4      accumRadiance  = path.GetL();
        const uint2 existingPacked = DenoisingLayersUAV[address].PackedNoisyRadianceAndSpecAvg;
        if (existingPacked.x != 0 && existingPacked.y != 0)
            accumRadiance += Fp16ToFp32(existingPacked);
        DenoisingLayersUAV[address].PackedNoisyRadianceAndSpecAvg = Fp32ToFp16(accumRadiance);
        path.SetL(float4(0, 0, 0, 0));
    }
#endif

    float3 GetAllRadiance(uint2 pixelPos, bool includeNoisy = true)
    {
        float3 pathL = LoadStableRadiance(pixelPos);
        if (includeNoisy)
        {
            for (int i = 0; i < cDenoisingLayerCount; i++)
            {
                if (GetBranchID(pixelPos, i) == cDenoisingLayerInvalidBranchID)
                    continue;
                pathL += DenoisingLayersUAV[PixelToAddress(pixelPos, i)].GetNoisyRadiance();
            }
        }
        return pathL;
    }
#endif
};

inline uint DenoisingLayersAdvanceBranchID(const uint prevStableBranchID, const uint deltaLobeID)
{
    return (prevStableBranchID << 2) | deltaLobeID;
}

inline uint DenoisingLayersGetParentLobeID(const uint stableBranchID)
{
    return stableBranchID & 0x3;
}

inline uint DenoisingLayersVertexIndexFromBranchID(const uint stableBranchID)
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

inline bool DenoisingLayerIsOnPlane(const uint planeBranchID, const uint vertexBranchID)
{
    return planeBranchID == vertexBranchID;
}

inline bool DenoisingLayerIsOnStablePath(const uint planeBranchID, const uint planeVertexIndex, const uint vertexBranchID, const uint vertexIndex)
{
    if (vertexIndex > planeVertexIndex)
        return false;
    return (planeBranchID >> ((planeVertexIndex - vertexIndex) * 2)) == vertexBranchID;
}

inline bool DenoisingLayerIsOnStablePath(const uint planeBranchID, const uint vertexBranchID)
{
    return DenoisingLayerIsOnStablePath(planeBranchID,
                                     DenoisingLayersVertexIndexFromBranchID(planeBranchID),
                                     vertexBranchID,
                                     DenoisingLayersVertexIndexFromBranchID(vertexBranchID));
}

inline float3 DenoisingLayerDebugVizColor(const uint planeIndex)
{
    return float3(planeIndex == 0 || planeIndex == 3, planeIndex == 1, planeIndex == 2 || planeIndex == 3);
}

#if !defined(__cplusplus)
inline uint3 DenoisingLayerDebugVizFourWaySplitCoord(const int dbgPlaneIndex, const uint2 pixelPos, const uint2 screenSize)
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

#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
void DenoisingLayer::PackCustomPayload(const uint4 packed[5])
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

void DenoisingLayer::UnpackCustomPayload(inout uint4 packed[5])
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

#endif // __DENOISING_LAYERS_HLSLI__
