#pragma pack_matrix(row_major)

#ifndef RTXPT_POST_PROCESS_MODE
#define RTXPT_POST_PROCESS_MODE 0
#endif

#define RTXPT_POST_PROCESS_DENOISING_LAYERS_DEBUG_VIZ  3
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
#include "../PathTracer/PathTracerCamera.hlsli"
#include "../PathTracer/DenoisingLayers.hlsli"
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
VK_IMAGE_FORMAT("r32ui")  RWTexture2DArray<uint>    u_DenoisingLayersHeader;
RWStructuredBuffer<DenoisingLayer>                     u_DenoisingLayersBuffer;
VK_IMAGE_FORMAT("r32f")   RWTexture2D<float>        u_DenoiserViewspaceZ;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>      u_DenoiserMotionVectors;
VK_IMAGE_FORMAT("rgb10_a2") RWTexture2D<float4>     u_DenoiserNormalRoughness;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>      u_DenoiserDiffRadianceHitDist;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>      u_DenoiserSpecRadianceHitDist;
VK_IMAGE_FORMAT("r8")      RWTexture2D<float>       u_DenoiserDisocclusionThresholdMix;
VK_IMAGE_FORMAT("r8")      RWTexture2D<float>       u_CombinedHistoryClampRelax;

#if RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_DLSS_RR_PREPARE_INPUTS
#include "../PathTracer/DenoisingGuidesRR.hlsli"
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

float RTXPTComputeNeighbourDisocclusionRelaxation(DenoisingLayersContext DenoisingLayers,
                                                  int2 PixelPos,
                                                  int2 ImageSize,
                                                  uint DenoisingLayerIndex,
                                                  float3 RayDirC,
                                                  int2 Offset)
{
    const float kEdge     = 0.02;
    uint2       PixelPosN = clamp(PixelPos + Offset, int2(0, 0), ImageSize - 1.xx);
    uint        BranchID  = DenoisingLayers.GetBranchID(PixelPosN, DenoisingLayerIndex);
    if (BranchID == cDenoisingLayerInvalidBranchID)
        return kEdge;

    DenoisingLayer SP      = DenoisingLayers.LoadDenoisingLayer(PixelPosN, DenoisingLayerIndex);
    float3      RayDirN = SP.GetNormal();
    return 1.0 - dot(RayDirC, RayDirN);
}

float RTXPTComputeDisocclusionRelaxation(DenoisingLayersContext DenoisingLayers,
                                         uint2 PixelPos,
                                         uint DenoisingLayerIndex,
                                         DenoisingLayer SP)
{
    const int2   ImageSize = int2(g_Frame.ptConsts.imageWidth, g_Frame.ptConsts.imageHeight);
    const float3 RayDirC   = SP.GetNormal();
    float        Relax     = 0.0;

    Relax += RTXPTComputeNeighbourDisocclusionRelaxation(DenoisingLayers, PixelPos, ImageSize, DenoisingLayerIndex, RayDirC, int2(-1, 0));
    Relax += RTXPTComputeNeighbourDisocclusionRelaxation(DenoisingLayers, PixelPos, ImageSize, DenoisingLayerIndex, RayDirC, int2(1, 0));
    Relax += RTXPTComputeNeighbourDisocclusionRelaxation(DenoisingLayers, PixelPos, ImageSize, DenoisingLayerIndex, RayDirC, int2(0, -1));
    Relax += RTXPTComputeNeighbourDisocclusionRelaxation(DenoisingLayers, PixelPos, ImageSize, DenoisingLayerIndex, RayDirC, int2(0, 1));
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
    const uint DenoisingLayerIndex       = g_Mini.params.x;
    const bool InitWithStableRadiance = g_Mini.params.y != 0;
    const uint ActiveDenoisingLayerCount = min(g_Frame.ptConsts.GetActiveDenoisingLayerCount(), cDenoisingLayerCount);
    if (DenoisingLayerIndex >= ActiveDenoisingLayerCount)
        return;

    DenoisingLayersContext DenoisingLayers = DenoisingLayersContext::make(
        u_DenoisingLayersHeader, u_DenoisingLayersBuffer, u_StableRadiance, g_Frame.ptConsts);

    if (InitWithStableRadiance)
    {
        u_OutputColor[PixelPos]               = float4(DenoisingLayers.LoadStableRadiance(PixelPos), 1.0);
        u_CombinedHistoryClampRelax[PixelPos] = 0.0;
    }

    bool HasSurface = false;
    uint BranchID   = DenoisingLayers.GetBranchID(PixelPos, DenoisingLayerIndex);
    if (BranchID != cDenoisingLayerInvalidBranchID)
    {
        DenoisingLayer SP         = DenoisingLayers.LoadDenoisingLayer(PixelPos, DenoisingLayerIndex);
        const bool  HitSurface = isfinite(SP.SceneLength);
        if (HitSurface)
        {
            HasSurface = true;

            float3 DiffBSDFEstimate;
            float3 SpecBSDFEstimate;
            UnpackTwoFp32ToFp16(SP.DenoiserPackedBSDFEstimate, DiffBSDFEstimate, SpecBSDFEstimate);
            DiffBSDFEstimate = max(DiffBSDFEstimate, 0.04.xxx);
            SpecBSDFEstimate = max(SpecBSDFEstimate, 0.04.xxx);

            float3 Throughput;
            float3 MotionVectors;
            UnpackTwoFp32ToFp16(SP.PackedThpAndMVs, Throughput, MotionVectors);

            CameraRay    PrimaryCameraRay = ComputeRayThinlens(g_Frame.ptConsts.camera, PixelPos, g_Frame.ptConsts.camera.Jitter, 0.5.xx);
            const float3 VirtualWorldPos  = PrimaryCameraRay.origin + PrimaryCameraRay.dir * SP.SceneLength;
            const float4 ViewPos         = mul(float4(VirtualWorldPos, 1.0), g_Frame.view.MatWorldToView);
            const float  VirtualViewZ    = ViewPos.z;
            MotionVectors = ComputePathTracerMotionVector(VirtualWorldPos, VirtualWorldPos + MotionVectors, g_Frame.view, g_Frame.previousView);

            float FinalRoughness     = max(0.2, SP.GetRoughness());
            float DisocclusionRelax  = 0.0;
            if (DenoisingLayersVertexIndexFromBranchID(BranchID) > 1)
                DisocclusionRelax = RTXPTComputeDisocclusionRelaxation(DenoisingLayers, PixelPos, DenoisingLayerIndex, SP);

            u_DenoiserViewspaceZ[PixelPos]               = VirtualViewZ;
            u_DenoiserMotionVectors[PixelPos]            = float4(MotionVectors, 0.0);
            u_DenoiserDisocclusionThresholdMix[PixelPos] = DisocclusionRelax;
            u_CombinedHistoryClampRelax[PixelPos]        = saturate(u_CombinedHistoryClampRelax[PixelPos] + DisocclusionRelax * saturate(RTXPTLuminance(Throughput)));

            FinalRoughness = saturate(FinalRoughness + DisocclusionRelax);

            float3 DenoiserDiffRadiance = SP.GetNoisyDiffRadiance() / DiffBSDFEstimate;
            float3 DenoiserSpecRadiance = SP.GetNoisySpecRadiance() / SpecBSDFEstimate;

            if (DenoisingLayerIndex == 0 &&
                g_Frame.ptConsts.denoisingLayersSuppressPrimaryIndirectSpecularK != 0.0 &&
                g_Frame.ptConsts.GetActiveDenoisingLayerCount() > 1)
            {
                bool ShouldSuppress = true;
                for (uint Plane = 1; Plane < g_Frame.ptConsts.GetActiveDenoisingLayerCount(); ++Plane)
                    ShouldSuppress = ShouldSuppress && DenoisingLayers.GetBranchID(PixelPos, Plane) != cDenoisingLayerInvalidBranchID;
                DenoiserSpecRadiance *= ShouldSuppress ? saturate(1.0 - g_Frame.ptConsts.denoisingLayersSuppressPrimaryIndirectSpecularK) : 1.0;
            }

            RTXPTClampRadiance(DenoiserDiffRadiance, g_Frame.ptConsts.denoiserRadianceClampK * 16.0);
            RTXPTClampRadiance(DenoiserSpecRadiance, g_Frame.ptConsts.denoiserRadianceClampK * 16.0);

            float SpecHitT = 0.0;
            if (DenoisingLayers.LoadDominantIndex(PixelPos) == DenoisingLayerIndex)
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
    const uint DenoisingLayerIndex = g_Mini.params.x;
    const bool HasValidation    = g_Mini.params.y != 0;
    const uint ActiveDenoisingLayerCount = min(g_Frame.ptConsts.GetActiveDenoisingLayerCount(), cDenoisingLayerCount);
    if (DenoisingLayerIndex >= ActiveDenoisingLayerCount)
        return;

    const bool HasSurface = t_DenoiserViewspaceZ[PixelPos] != RTXPT_VIEWZ_SKY_MARKER;
    const uint SPAddress  = GenericTSPixelToAddress(PixelPos,
                                                    DenoisingLayerIndex,
                                                    g_Frame.ptConsts.genericTSLineStride,
                                                    g_Frame.ptConsts.genericTSPlaneStride);

    float4 DiffRadiance = 0.0.xxxx;
    float4 SpecRadiance = 0.0.xxxx;

    if (HasSurface)
    {
        float3 DiffBSDFEstimate;
        float3 SpecBSDFEstimate;
        UnpackTwoFp32ToFp16(u_DenoisingLayersBuffer[SPAddress].DenoiserPackedBSDFEstimate, DiffBSDFEstimate, SpecBSDFEstimate);
        DiffBSDFEstimate = max(DiffBSDFEstimate, 0.04.xxx);
        SpecBSDFEstimate = max(SpecBSDFEstimate, 0.04.xxx);

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
    DenoisingLayersContext DenoisingLayers = DenoisingLayersContext::make(
        u_DenoisingLayersHeader, u_DenoisingLayersBuffer, u_StableRadiance, g_Frame.ptConsts);
    u_OutputColor[PixelPos] = float4(DenoisingLayers.GetAllRadiance(PixelPos), 1.0);
#endif

#if RTXPT_POST_PROCESS_MODE == RTXPT_POST_PROCESS_DENOISING_LAYERS_DEBUG_VIZ
    const int   DebugPlaneIndex = int(g_Mini.params.x) - 1;
    const uint3 DebugCoord      = DenoisingLayerDebugVizFourWaySplitCoord(DebugPlaneIndex, PixelPos, uint2(g_Frame.ptConsts.imageWidth, g_Frame.ptConsts.imageHeight));

    DenoisingLayersContext DenoisingLayers = DenoisingLayersContext::make(
        u_DenoisingLayersHeader, u_DenoisingLayersBuffer, u_StableRadiance, g_Frame.ptConsts);

    float3 Color = 0.0.xxx;
    if (DebugCoord.z < g_Frame.ptConsts.GetActiveDenoisingLayerCount())
    {
        const uint BranchID = DenoisingLayers.GetBranchID(DebugCoord.xy, DebugCoord.z);
        Color = BranchID == cDenoisingLayerInvalidBranchID ?
            float3(((PixelPos.x + PixelPos.y) & 1u) != 0u, 0.0, 0.0) :
            DenoisingLayerDebugVizColor(DebugCoord.z);
    }

    u_OutputColor[PixelPos] = float4(Color, 1.0);
#endif
}
