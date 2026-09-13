#pragma pack_matrix(row_major)

#ifndef RTXPT_POST_PROCESS_MODE
#define RTXPT_POST_PROCESS_MODE 0
#endif

#define RTXPT_POST_PROCESS_STABLE_PLANES_DEBUG_VIZ  3
#define RTXPT_POST_PROCESS_RELAX_PREPARE_INPUTS     4
#define RTXPT_POST_PROCESS_REBLUR_PREPARE_INPUTS    5
#define RTXPT_POST_PROCESS_RELAX_FINAL_MERGE        6
#define RTXPT_POST_PROCESS_REBLUR_FINAL_MERGE       7
#define RTXPT_POST_PROCESS_NO_DENOISER_FINAL_MERGE  8

#define RTXPT_POST_PROCESS_DLSS_RR_PREPARE_INPUTS   9

#define NUM_COMPUTE_THREADS_PER_DIM 8

#ifndef USE_RELAX
#    if RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_RELAX_PREPARE_INPUTS || RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_RELAX_FINAL_MERGE
#        define USE_RELAX 1
#    else
#        define USE_RELAX 0
#    endif
#endif

#include "../PathTracer/PathTracerShared.h"
#include "../PathTracer/PathTracerHelpers.hlsli"
#include "../PathTracer/StablePlanes.hlsli"
#include "RTXPTDenoiserNRD.hlsli"

cbuffer g_Const
{
    SampleConstants g_Frame;
};

cbuffer g_MiniConst
{
    SampleMiniConstants g_Mini;
};

Texture2D<float>        t_Depth;
Texture2D<float4>       t_MotionVectors;
Texture2D<float>        t_SpecularHitT;
Texture2D<float4>       t_DenoiserOutDiffRadianceHitDist;
Texture2D<float4>       t_DenoiserOutSpecRadianceHitDist;
Texture2D<float4>       t_DenoiserOutValidation;
Texture2D<float>        t_DenoiserViewspaceZ;
Texture2D<float>        t_DenoiserDisocclusionThresholdMix;

VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>      u_OutputColor;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>      u_StableRadiance;
VK_IMAGE_FORMAT("r32ui")  RWTexture2DArray<uint>    u_StablePlanesHeader;
RWStructuredBuffer<StablePlane>                     u_StablePlanesBuffer;
VK_IMAGE_FORMAT("r32f")   RWTexture2D<float>        u_DenoiserViewspaceZ;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>      u_DenoiserMotionVectors;
VK_IMAGE_FORMAT("rgb10_a2") RWTexture2D<float4>     u_DenoiserNormalRoughness;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>      u_DenoiserDiffRadianceHitDist;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>      u_DenoiserSpecRadianceHitDist;
VK_IMAGE_FORMAT("r8")      RWTexture2D<float>       u_DenoiserDisocclusionThresholdMix;
VK_IMAGE_FORMAT("r8")      RWTexture2D<float>       u_CombinedHistoryClampRelax;

#if RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_DLSS_RR_PREPARE_INPUTS
#include "../PathTracer/PathTracerCamera.hlsli"
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_RRDiffuseAlbedo;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_RRSpecAlbedo;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_RRNormalsAndRoughness;
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> u_RRDepth;
VK_IMAGE_FORMAT("rg16f") RWTexture2D<float2> u_RRMotionVectors;

// Sanitize before arithmetic: fp16 transport may contain overflow or invalid values.
float RRFiniteNonNegative(float value)
{
    return isnan(value) ? 0.0 : clamp(value, 0.0, HLF_MAX);
}

float3 RRFiniteNonNegative(float3 value)
{
    return float3(RRFiniteNonNegative(value.x), RRFiniteNonNegative(value.y), RRFiniteNonNegative(value.z));
}

void DLSSRRDenoiserPrepareInputs(uint2 pixelPos)
{
    // BUILD uses mini-constant sample offset zero. Depth is the dominant path's
    // virtual ray distance, not hardware depth or linear view Z.
    const CameraRay cameraRay = ComputePathTracerCameraRay(g_Frame.ptConsts, pixelPos, g_Frame.ptConsts.sampleBaseIndex);
    const float rayDistance = t_Depth[pixelPos];
    const float3 virtualWorldPos = cameraRay.origin + cameraRay.dir * rayDistance;
    const float viewZ = mul(float4(virtualWorldPos, 1.0), g_Frame.view.MatWorldToView).z;
    const float kSkyViewZ = kEnvironmentMapSceneDistance;
    u_RRDepth[pixelPos] = isfinite(rayDistance) && rayDistance > 0.0 && isfinite(viewZ) && viewZ > 0.0 ? viewZ : kSkyViewZ;
    const float2 motion = t_MotionVectors[pixelPos].xy;
    u_RRMotionVectors[pixelPos] = all(isfinite(motion)) ? clamp(motion, -HLF_MAX, HLF_MAX) : 0.0.xx;

    StablePlanesContext stablePlanes = StablePlanesContext::make(
        u_StablePlanesHeader, u_StablePlanesBuffer, u_StableRadiance, g_Frame.ptConsts);
    const uint activeCount = min(g_Frame.ptConsts.GetActiveStablePlaneCount(), cStablePlaneCount);
    const uint dominantIndex = stablePlanes.LoadDominantIndex(pixelPos);

    float3 combinedRadiance = RRFiniteNonNegative(stablePlanes.LoadStableRadiance(pixelPos));
    const float stableMax = max(combinedRadiance.x, max(combinedRadiance.y, combinedRadiance.z));
    const float3 stableAlbedo = sqrt(combinedRadiance / (1.0 + stableMax)); // ReinhardMax
    const float stableAlbedoAvg = Average(stableAlbedo);

    // RTXPT-fork's active default: throughput + equalization + dominant-plane bias.
    const float kThroughputWeight = 0.2;
    const float kEqualizationWeight = 0.01;
    const float kDominantWeight = 0.05;
    float3 available = 0.0.xxx;
    float3 throughputWeights = float3(1, 0, 0);
    for (uint planeIndex = 0; planeIndex < activeCount; ++planeIndex)
    {
        if (stablePlanes.GetBranchID(pixelPos, planeIndex) == cStablePlaneInvalidBranchID)
            continue;
        available[planeIndex] = 1.0;
        if (planeIndex != 0)
        {
            StablePlane sp = stablePlanes.LoadStablePlane(pixelPos, planeIndex);
            float3 throughput, motionVectors;
            UnpackTwoFp32ToFp16(sp.PackedThpAndMVs, throughput, motionVectors);
            const float weight = saturate(Average(RRFiniteNonNegative(throughput)));
            throughputWeights[planeIndex] = weight;
            throughputWeights[0] = saturate(throughputWeights[0] - weight);
        }
    }
    float3 spWeights = throughputWeights * kThroughputWeight + kEqualizationWeight;
    if (dominantIndex < activeCount)
        spWeights[dominantIndex] += kDominantWeight;
    spWeights *= available;
    const float weightSum = spWeights.x + spWeights.y + spWeights.z;
    spWeights /= max(weightSum, 1e-7);

    float3 guideNormals = float3(0, 0, 1e-6);
    float3 diffAlbedo = 0.0.xxx;
    float3 specAlbedo = 0.0.xxx;
    float roughness = 0.0;
    for (uint planeIndex = 0; planeIndex < activeCount; ++planeIndex)
    {
        if (available[planeIndex] == 0.0)
            continue;
        StablePlane sp = stablePlanes.LoadStablePlane(pixelPos, planeIndex);
        if (!isfinite(sp.SceneLength))
            continue; // Sky/emission already belongs to StableRadiance.
        combinedRadiance += RRFiniteNonNegative(sp.GetNoisyRadiance());
        const float weight = spWeights[planeIndex];
        if (weight > 1e-6)
        {
            float3 diffBSDFEstimate, specBSDFEstimate;
            UnpackTwoFp32ToFp16(sp.DenoiserPackedBSDFEstimate, diffBSDFEstimate, specBSDFEstimate);
            guideNormals += weight * sp.GetNormal();
            roughness += weight * saturate(RRFiniteNonNegative(sp.GetRoughness()));
            diffAlbedo += weight * saturate(RRFiniteNonNegative(diffBSDFEstimate));
            specAlbedo += weight * saturate(RRFiniteNonNegative(specBSDFEstimate));
        }
    }

    const float3 stableAlbedoGreyMix = lerp(stableAlbedo, 0.5.xxx, 0.2);
    diffAlbedo = lerp(diffAlbedo, stableAlbedoGreyMix,
        stableAlbedoAvg / (Average(diffAlbedo) + sqrt(stableAlbedoAvg) + 1e-7));
    const float normalLength = length(guideNormals);
    guideNormals = isfinite(normalLength) && normalLength >= 1e-5 ?
        guideNormals / normalLength : float3(0, 0, 1);
    const float kMinAlbedo = 0.05;
    if (Average(diffAlbedo + specAlbedo) < kMinAlbedo)
        diffAlbedo += kMinAlbedo;

    // Color is scene-linear, not pre-exposed. CPU supplies the clamp threshold;
    // PreExposure and ExposureScale at the RR consumer must both be 1.
    const float maxRadiance = max(combinedRadiance.x, max(combinedRadiance.y, combinedRadiance.z));
    const float brightnessClamp = g_Frame.ptConsts.DLSSRRBrightnessClampK;
    const float clampMax = isfinite(brightnessClamp) && brightnessClamp > 0.0 ?
        min(brightnessClamp, HLF_MAX) : HLF_MAX;
    if (maxRadiance > clampMax)
        combinedRadiance *= clampMax / maxRadiance;

    u_OutputColor[pixelPos] = float4(combinedRadiance, 1);
    u_RRDiffuseAlbedo[pixelPos] = float4(saturate(diffAlbedo), 1);
    u_RRSpecAlbedo[pixelPos] = float4(saturate(specAlbedo), 1);
    u_RRNormalsAndRoughness[pixelPos] = float4(guideNormals, saturate(roughness));
}
#endif

float RTXPTLuminance(float3 Color)
{
    return dot(Color, float3(0.2126, 0.7152, 0.0722));
}

void RTXPTClampRadiance(inout float3 Radiance, float RangeK)
{
    const float SafeRangeK = max(RangeK, 1.0);
    const float ClampMax   = min(255.0, g_Frame.ptConsts.preExposedGrayLuminance * SafeRangeK);
    const float Lum        = RTXPTLuminance(Radiance);
    if (Lum > ClampMax)
        Radiance *= ClampMax / Lum;
}

float RTXPTComputeNeighbourDisocclusionRelaxation(StablePlanesContext StablePlanes,
                                                  int2 PixelPos,
                                                  int2 ImageSize,
                                                  uint StablePlaneIndex,
                                                  float3 RayDirC,
                                                  int2 Offset)
{
    const float kEdge     = 0.02;
    uint2       PixelPosN = clamp(PixelPos + Offset, int2(0, 0), ImageSize - 1.xx);
    uint        BranchID  = StablePlanes.GetBranchID(PixelPosN, StablePlaneIndex);
    if (BranchID == cStablePlaneInvalidBranchID)
        return kEdge;

    StablePlane SP      = StablePlanes.LoadStablePlane(PixelPosN, StablePlaneIndex);
    float3      RayDirN = SP.GetNormal();
    return 1.0 - dot(RayDirC, RayDirN);
}

float RTXPTComputeDisocclusionRelaxation(StablePlanesContext StablePlanes,
                                         uint2 PixelPos,
                                         uint StablePlaneIndex,
                                         StablePlane SP)
{
    const int2   ImageSize = int2(g_Frame.ptConsts.imageWidth, g_Frame.ptConsts.imageHeight);
    const float3 RayDirC   = SP.GetNormal();
    float        Relax     = 0.0;

    Relax += RTXPTComputeNeighbourDisocclusionRelaxation(StablePlanes, PixelPos, ImageSize, StablePlaneIndex, RayDirC, int2(-1, 0));
    Relax += RTXPTComputeNeighbourDisocclusionRelaxation(StablePlanes, PixelPos, ImageSize, StablePlaneIndex, RayDirC, int2(1, 0));
    Relax += RTXPTComputeNeighbourDisocclusionRelaxation(StablePlanes, PixelPos, ImageSize, StablePlaneIndex, RayDirC, int2(0, -1));
    Relax += RTXPTComputeNeighbourDisocclusionRelaxation(StablePlanes, PixelPos, ImageSize, StablePlaneIndex, RayDirC, int2(0, 1));
    return saturate((Relax - 0.00002) * 25.0);
}

[numthreads(NUM_COMPUTE_THREADS_PER_DIM, NUM_COMPUTE_THREADS_PER_DIM, 1)]
void main(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    const uint2 PixelPos = DispatchThreadID.xy;
    if (PixelPos.x >= g_Frame.ptConsts.imageWidth || PixelPos.y >= g_Frame.ptConsts.imageHeight)
        return;

#if RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_DLSS_RR_PREPARE_INPUTS
    DLSSRRDenoiserPrepareInputs(PixelPos);
#endif

#if RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_RELAX_PREPARE_INPUTS || RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_REBLUR_PREPARE_INPUTS
    const uint StablePlaneIndex       = g_Mini.params.x;
    const bool InitWithStableRadiance = g_Mini.params.y != 0;
    const uint ActiveStablePlaneCount = min(g_Frame.ptConsts.GetActiveStablePlaneCount(), cStablePlaneCount);
    if (StablePlaneIndex >= ActiveStablePlaneCount)
        return;

    StablePlanesContext StablePlanes = StablePlanesContext::make(
        u_StablePlanesHeader, u_StablePlanesBuffer, u_StableRadiance, g_Frame.ptConsts);

    if (InitWithStableRadiance)
    {
        u_OutputColor[PixelPos]               = float4(StablePlanes.LoadStableRadiance(PixelPos), 1.0);
        u_CombinedHistoryClampRelax[PixelPos] = 0.0;
    }

    bool HasSurface = false;
    uint BranchID   = StablePlanes.GetBranchID(PixelPos, StablePlaneIndex);
    if (BranchID != cStablePlaneInvalidBranchID)
    {
        StablePlane SP         = StablePlanes.LoadStablePlane(PixelPos, StablePlaneIndex);
        const bool  HitSurface = isfinite(SP.SceneLength);
        if (HitSurface)
        {
            HasSurface = true;

            float3 DiffBSDFEstimate;
            float3 SpecBSDFEstimate;
            UnpackTwoFp32ToFp16(SP.DenoiserPackedBSDFEstimate, DiffBSDFEstimate, SpecBSDFEstimate);
            DiffBSDFEstimate = max(DiffBSDFEstimate, 1e-4.xxx);
            SpecBSDFEstimate = max(SpecBSDFEstimate, 1e-4.xxx);

            float3 Throughput;
            float3 MotionVectors;
            UnpackTwoFp32ToFp16(SP.PackedThpAndMVs, Throughput, MotionVectors);

            CameraRay    PrimaryCameraRay = ComputeRayThinlens(g_Frame.ptConsts.camera, PixelPos, g_Frame.ptConsts.camera.Jitter, 0.5.xx);
            const float3 VirtualWorldPos  = PrimaryCameraRay.origin + PrimaryCameraRay.dir * SP.SceneLength;
            const float4 ViewPos         = mul(float4(VirtualWorldPos, 1.0), g_Frame.view.MatWorldToView);
            const float  VirtualViewZ    = ViewPos.z;

            float FinalRoughness     = max(0.2, SP.GetRoughness());
            float DisocclusionRelax  = 0.0;
            if (StablePlanesVertexIndexFromBranchID(BranchID) > 1)
                DisocclusionRelax = RTXPTComputeDisocclusionRelaxation(StablePlanes, PixelPos, StablePlaneIndex, SP);

            u_DenoiserViewspaceZ[PixelPos]               = VirtualViewZ;
            u_DenoiserMotionVectors[PixelPos]            = float4(MotionVectors, 0.0);
            u_DenoiserDisocclusionThresholdMix[PixelPos] = DisocclusionRelax;
            u_CombinedHistoryClampRelax[PixelPos]        = saturate(u_CombinedHistoryClampRelax[PixelPos] + DisocclusionRelax * saturate(RTXPTLuminance(Throughput)));

            FinalRoughness = saturate(FinalRoughness + DisocclusionRelax);

            float3 DenoiserDiffRadiance = SP.GetNoisyDiffRadiance() / DiffBSDFEstimate;
            float3 DenoiserSpecRadiance = SP.GetNoisySpecRadiance() / SpecBSDFEstimate;

            if (StablePlaneIndex == 0 &&
                g_Frame.ptConsts.stablePlanesSuppressPrimaryIndirectSpecularK != 0.0 &&
                g_Frame.ptConsts.GetActiveStablePlaneCount() > 1)
            {
                bool ShouldSuppress = true;
                for (uint Plane = 1; Plane < g_Frame.ptConsts.GetActiveStablePlaneCount(); ++Plane)
                    ShouldSuppress = ShouldSuppress && StablePlanes.GetBranchID(PixelPos, Plane) != cStablePlaneInvalidBranchID;
                DenoiserSpecRadiance *= ShouldSuppress ? saturate(1.0 - g_Frame.ptConsts.stablePlanesSuppressPrimaryIndirectSpecularK) : 1.0;
            }

            RTXPTClampRadiance(DenoiserDiffRadiance, g_Frame.ptConsts.denoiserRadianceClampK * 16.0);
            RTXPTClampRadiance(DenoiserSpecRadiance, g_Frame.ptConsts.denoiserRadianceClampK * 16.0);

            float SpecHitT = 0.0;
            if (StablePlanes.LoadDominantIndex(PixelPos) == StablePlaneIndex)
                SpecHitT = t_SpecularHitT[PixelPos];

            u_DenoiserNormalRoughness[PixelPos] = RTXPTDenoiserPackNormalAndRoughness(SP.GetNormal(), FinalRoughness);

#    if RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_RELAX_PREPARE_INPUTS
            u_DenoiserDiffRadianceHitDist[PixelPos] = RTXPTDenoiserRelaxPackRadianceHitDist(DenoiserDiffRadiance, 0.0);
            u_DenoiserSpecRadianceHitDist[PixelPos] = RTXPTDenoiserRelaxPackRadianceHitDist(DenoiserSpecRadiance, SpecHitT);
#    else
            const float DiffNormHitDist = 0.0;
            const float SpecNormHitDist = RTXPTDenoiserReblurGetNormHitDist(SpecHitT, VirtualViewZ, SP.GetRoughness());
            u_DenoiserDiffRadianceHitDist[PixelPos] = RTXPTDenoiserReblurPackRadianceNormHitDist(DenoiserDiffRadiance, DiffNormHitDist);
            u_DenoiserSpecRadianceHitDist[PixelPos] = RTXPTDenoiserReblurPackRadianceNormHitDist(DenoiserSpecRadiance, SpecNormHitDist);
#    endif
        }
    }

    if (!HasSurface)
        u_DenoiserViewspaceZ[PixelPos] = RTXPT_VIEWZ_SKY_MARKER;
#endif

#if RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_RELAX_FINAL_MERGE || RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_REBLUR_FINAL_MERGE
    const uint StablePlaneIndex = g_Mini.params.x;
    const bool HasValidation    = g_Mini.params.y != 0;
    const uint ActiveStablePlaneCount = min(g_Frame.ptConsts.GetActiveStablePlaneCount(), cStablePlaneCount);
    if (StablePlaneIndex >= ActiveStablePlaneCount)
        return;

    const bool HasSurface = t_DenoiserViewspaceZ[PixelPos] != RTXPT_VIEWZ_SKY_MARKER;
    const uint SPAddress  = GenericTSPixelToAddress(PixelPos,
                                                    StablePlaneIndex,
                                                    g_Frame.ptConsts.genericTSLineStride,
                                                    g_Frame.ptConsts.genericTSPlaneStride);

    float4 DiffRadiance = 0.0.xxxx;
    float4 SpecRadiance = 0.0.xxxx;

    if (HasSurface)
    {
        float3 DiffBSDFEstimate;
        float3 SpecBSDFEstimate;
        UnpackTwoFp32ToFp16(u_StablePlanesBuffer[SPAddress].DenoiserPackedBSDFEstimate, DiffBSDFEstimate, SpecBSDFEstimate);

        DiffRadiance = t_DenoiserOutDiffRadianceHitDist[PixelPos];
        SpecRadiance = t_DenoiserOutSpecRadianceHitDist[PixelPos];
        RTXPTDenoiserPostDenoiseProcess(DiffBSDFEstimate, SpecBSDFEstimate, DiffRadiance, SpecRadiance);

        u_OutputColor[PixelPos].xyz += max(0.0.xxx, DiffRadiance.rgb + SpecRadiance.rgb);
    }

    if (HasValidation)
    {
        const float4 Validation = t_DenoiserOutValidation[PixelPos];
        if (Validation.a > 0.0)
            u_OutputColor[PixelPos] = float4(Validation.rgb, 1.0);
    }
#endif

#if RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_NO_DENOISER_FINAL_MERGE
    StablePlanesContext StablePlanes = StablePlanesContext::make(
        u_StablePlanesHeader, u_StablePlanesBuffer, u_StableRadiance, g_Frame.ptConsts);
    u_OutputColor[PixelPos] = float4(StablePlanes.GetAllRadiance(PixelPos), 1.0);
#endif

#if RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_STABLE_PLANES_DEBUG_VIZ
    const int   DebugPlaneIndex = int(g_Mini.params.x) - 1;
    const uint3 DebugCoord      = StablePlaneDebugVizFourWaySplitCoord(DebugPlaneIndex, PixelPos, uint2(g_Frame.ptConsts.imageWidth, g_Frame.ptConsts.imageHeight));

    StablePlanesContext StablePlanes = StablePlanesContext::make(
        u_StablePlanesHeader, u_StablePlanesBuffer, u_StableRadiance, g_Frame.ptConsts);

    float3 Color = 0.0.xxx;
    if (DebugCoord.z < g_Frame.ptConsts.GetActiveStablePlaneCount())
    {
        const uint BranchID = StablePlanes.GetBranchID(DebugCoord.xy, DebugCoord.z);
        Color = BranchID == cStablePlaneInvalidBranchID ?
            float3(((PixelPos.x + PixelPos.y) & 1u) != 0u, 0.0, 0.0) :
            StablePlaneDebugVizColor(DebugCoord.z);
    }

    u_OutputColor[PixelPos] = float4(Color, 1.0);
#endif
}
