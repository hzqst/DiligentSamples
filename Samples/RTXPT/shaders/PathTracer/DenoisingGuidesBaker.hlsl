#ifndef __DENOISING_GUIDES_BAKER__
#define __DENOISING_GUIDES_BAKER__

#include "PathTracerShared.h"

struct DenoisingGuidesBakerConstants
{
    uint2 RenderResolution;
    uint2 DisplayResolution;

    int  DebugView;
    uint Ping;
    uint _padding1;
    uint _padding2;

    uint4 _padding3;
    uint4 _padding4;
};

#define DGB_2D_THREADGROUP_SIZE 8

#if !defined(__cplusplus)

#include "PathTracerHelpers.hlsli"
#include "DenoisingLayers.hlsli"

#if RTXPT_PREPARE_DLSS_RR
cbuffer g_Const { SampleConstants g_Frame; };
Texture2D<float> t_Depth;
Texture2D<float4> t_MotionVectors;
Texture2D<float> t_SpecularHitT;
VK_IMAGE_FORMAT("rg16f") RWTexture2D<float2> u_RRSpecMotionVectors;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_OutputColor;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_StableRadiance;
VK_IMAGE_FORMAT("r32ui") RWTexture2DArray<uint> u_DenoisingLayersHeader;
RWStructuredBuffer<DenoisingLayer> u_DenoisingLayersBuffer;
#include "DenoisingGuidesRR.hlsli"
[numthreads(DGB_2D_THREADGROUP_SIZE, DGB_2D_THREADGROUP_SIZE, 1)]
void PrepareDLSSRRGuides(uint2 pixelPos : SV_DispatchThreadID)
{
    if (all(pixelPos < uint2(g_Frame.ptConsts.imageWidth, g_Frame.ptConsts.imageHeight)))
        DLSSRRDenoiserPrepareInputs(pixelPos);
}
#else
ConstantBuffer<SampleConstants>               g_Const;
ConstantBuffer<DenoisingGuidesBakerConstants> g_DenoisingGuidesBakerConstants;

Texture2D<float>                                  t_Depth;
Texture2D<float4>                                 t_MotionVectors;
VK_IMAGE_FORMAT("r32f") RWTexture2D<float>        u_SpecularHitT;
VK_IMAGE_FORMAT("r32f") RWTexture2D<float>        u_ScratchFloat1;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>    u_StableRadiance;
VK_IMAGE_FORMAT("r32ui") RWTexture2DArray<uint>   u_DenoisingLayersHeader;
RWStructuredBuffer<DenoisingLayer>                   u_DenoisingLayersBuffer;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>    u_DenoiserAvgLayerRadianceHalfRes;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4>    u_DebugOutput;

static const int RTXPT_DENOISING_GUIDE_DEBUG_DISABLED           = 0;
static const int RTXPT_DENOISING_GUIDE_DEBUG_DEPTH              = 1;
static const int RTXPT_DENOISING_GUIDE_DEBUG_MOTION_VECTORS     = 2;
static const int RTXPT_DENOISING_GUIDE_DEBUG_SPECULAR_HIT_T     = 3;
static const int RTXPT_DENOISING_GUIDE_DEBUG_AVG_LAYER_RADIANCE = 4;
static const int RTXPT_DENOISING_GUIDE_DEBUG_PRIMARY_LAYER      = 5;

float3 DenoisingGuidesHeatMap(float GreyValue)
{
    const float T = saturate(GreyValue);
    return saturate(float3(1.5 - abs(4.0 * T - 3.0),
                           1.5 - abs(4.0 * T - 2.0),
                           1.5 - abs(4.0 * T - 1.0)));
}

float SpecHitTNeighbourhood(RWTexture2D<float> TexSrc, int2 PixelPos)
{
    const float CenterD = t_Depth[PixelPos];
    float       PrevHitT = max(0.0, TexSrc[PixelPos]);

    const float MinSpecHitT = 5e-2f;
    if (PrevHitT < MinSpecHitT)
        PrevHitT = 0.0;

    const int NeighbourRadius = 2;
    uint      Width           = 0;
    uint      Height          = 0;
    TexSrc.GetDimensions(Width, Height);

    float ValueAverage = PrevHitT;
    float SumWeight    = PrevHitT > 0.0;
    for (int X = -NeighbourRadius; X <= NeighbourRadius; ++X)
    {
        for (int Y = -NeighbourRadius; Y <= NeighbourRadius; ++Y)
        {
            if (X == 0 && Y == 0)
                continue;

            const int2 NeighbourPixelPos = PixelPos + int2(X, Y);
            if (NeighbourPixelPos.x < 0 || NeighbourPixelPos.y < 0 ||
                NeighbourPixelPos.x >= Width || NeighbourPixelPos.y >= Height)
                continue;

            const float V              = min(TexSrc[NeighbourPixelPos], HLF_MAX);
            const float D              = max(0.0, t_Depth[NeighbourPixelPos]);
            const float DepthThreshold = 0.025;
            float       Weight         = V > 0.0;
            Weight *= abs(D - CenterD) <= (D + CenterD + 1e-5f) * DepthThreshold;

            if (Weight > 0.0)
            {
                ValueAverage += V * Weight;
                SumWeight += Weight;
            }
        }
    }

    if (SumWeight == 0.0)
        return PrevHitT;

    ValueAverage /= SumWeight;
    return PrevHitT <= 0.0 ? ValueAverage : min(PrevHitT * 1.5 + 0.5, ValueAverage);
}

[numthreads(DGB_2D_THREADGROUP_SIZE, DGB_2D_THREADGROUP_SIZE, 1)]
void DenoiseSpecHitT(uint2 DispatchThreadID : SV_DispatchThreadID)
{
    const int2 PixelPos = DispatchThreadID.xy;
    if (any(DispatchThreadID.xy >= g_DenoisingGuidesBakerConstants.RenderResolution))
        return;

    if (g_DenoisingGuidesBakerConstants.Ping != 0u)
        u_ScratchFloat1[PixelPos] = SpecHitTNeighbourhood(u_SpecularHitT, PixelPos);
    else
        u_SpecularHitT[PixelPos] = SpecHitTNeighbourhood(u_ScratchFloat1, PixelPos);
}

[numthreads(DGB_2D_THREADGROUP_SIZE, DGB_2D_THREADGROUP_SIZE, 1)]
void ComputeAvgLayerRadiance(uint2 DispatchThreadID : SV_DispatchThreadID)
{
    const uint2 HalfResPos = DispatchThreadID.xy;
    const uint2 HalfRes    = (g_DenoisingGuidesBakerConstants.RenderResolution + 1u) / 2u;
    if (any(HalfResPos >= HalfRes))
        return;

    const uint2  RenderMax          = max(g_DenoisingGuidesBakerConstants.RenderResolution, uint2(1u, 1u)) - 1u;
    const uint2  BasePixel          = min(HalfResPos * 2u, RenderMax);
    const float2 ScreenSpaceMotion  = t_MotionVectors[min(BasePixel + (HalfResPos & 1u), RenderMax)].xy;
    const int2   HistoricPixel      = int2(float2(BasePixel) + ScreenSpaceMotion + 0.5.xx);
    const uint2  HistoricHalfResPos = min(uint2(max(HistoricPixel, int2(0, 0))) / 2u, HalfRes - 1u);

    const float ExponentialFalloffK = saturate(0.05);
    float4      OutVal              = clamp(u_DenoiserAvgLayerRadianceHalfRes[HistoricHalfResPos], 0.0, HLF_MAX) *
        (1.0 - ExponentialFalloffK);

    DenoisingLayersContext DenoisingLayers = DenoisingLayersContext::make(u_DenoisingLayersHeader,
                                                                  u_DenoisingLayersBuffer,
                                                                  u_StableRadiance,
                                                                  g_Const.ptConsts);

    for (uint DenoisingLayerIndex = 0;
         DenoisingLayerIndex < min(g_Const.ptConsts.GetActiveDenoisingLayerCount(), cDenoisingLayerCount);
         ++DenoisingLayerIndex)
    {
        float Avg   = 0.0;
        float Count = 1e-7;
        for (uint X = 0; X < 2; ++X)
        {
            for (uint Y = 0; Y < 2; ++Y)
            {
                const uint2 PixelPos = min(BasePixel + uint2(X, Y), RenderMax);
                const uint  BranchID = DenoisingLayers.GetBranchID(PixelPos, DenoisingLayerIndex);
                if (BranchID == cDenoisingLayerInvalidBranchID)
                    continue;

                DenoisingLayer SP       = DenoisingLayers.LoadDenoisingLayer(PixelPos, DenoisingLayerIndex);
                float       Radiance = max(1e-5, Average(SP.GetNoisyRadiance()));
                Radiance             = min(Radiance, g_Const.ptConsts.preExposedGrayLuminance * 2.0);
                Avg += log(Radiance + 1.0);
                Count += 1.0;
            }
        }

        Avg = exp(Avg / Count) - 1.0;
        OutVal[DenoisingLayerIndex] += Avg * ExponentialFalloffK;
    }

    OutVal.w = OutVal.x + OutVal.y + OutVal.z;
    u_DenoiserAvgLayerRadianceHalfRes[HalfResPos] = OutVal;
}

uint SelectPrimaryLayer(float4 AvgLayerRadiance)
{
    uint  PrimaryLayer = 0u;
    float BestValue    = AvgLayerRadiance.x;
    if (AvgLayerRadiance.y > BestValue)
    {
        PrimaryLayer = 1u;
        BestValue    = AvgLayerRadiance.y;
    }
    if (AvgLayerRadiance.z > BestValue)
        PrimaryLayer = 2u;
    return PrimaryLayer;
}

[numthreads(DGB_2D_THREADGROUP_SIZE, DGB_2D_THREADGROUP_SIZE, 1)]
void DebugViz(uint2 DispatchThreadID : SV_DispatchThreadID)
{
    const uint2 PixelPos = DispatchThreadID.xy;
    if (any(PixelPos >= g_DenoisingGuidesBakerConstants.RenderResolution))
        return;

    float4       Color             = float4(0.0, 0.0, 0.0, 1.0);
    const int    DebugView         = g_DenoisingGuidesBakerConstants.DebugView;
    const uint2  HalfRes           = (g_DenoisingGuidesBakerConstants.RenderResolution + 1u) / 2u;
    const uint2  HalfResPos        = min(PixelPos / 2u, HalfRes - 1u);
    const float4 AvgLayerRadiance  = u_DenoiserAvgLayerRadianceHalfRes[HalfResPos];

    if (DebugView == RTXPT_DENOISING_GUIDE_DEBUG_DEPTH)
    {
        Color = float4(saturate(t_Depth[PixelPos] * 100.0).xxx, 1.0);
    }
    else if (DebugView == RTXPT_DENOISING_GUIDE_DEBUG_MOTION_VECTORS)
    {
        const float2 Motion = t_MotionVectors[PixelPos].xy;
        Color               = float4(0.5.xx + Motion * 0.2, 0.0, 1.0);
    }
    else if (DebugView == RTXPT_DENOISING_GUIDE_DEBUG_SPECULAR_HIT_T)
    {
        Color = float4(DenoisingGuidesHeatMap(u_SpecularHitT[PixelPos] / 50.0), 1.0);
    }
    else if (DebugView == RTXPT_DENOISING_GUIDE_DEBUG_AVG_LAYER_RADIANCE)
    {
        Color = float4(sqrt(ReinhardMax(max(AvgLayerRadiance.xyz, 0.0.xxx))), 1.0);
    }
    else if (DebugView == RTXPT_DENOISING_GUIDE_DEBUG_PRIMARY_LAYER)
    {
        const uint PrimaryLayer = SelectPrimaryLayer(AvgLayerRadiance);
        Color                   = float4(PrimaryLayer == 0u, PrimaryLayer == 1u, PrimaryLayer == 2u, 1.0);
    }

    u_DebugOutput[PixelPos] = Color;
}

#endif // RTXPT_PREPARE_DLSS_RR
#endif

#endif // __DENOISING_GUIDES_BAKER__
