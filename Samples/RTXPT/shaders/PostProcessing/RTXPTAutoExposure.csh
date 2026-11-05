// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#ifndef VK_IMAGE_FORMAT
#    define VK_IMAGE_FORMAT(format)
#endif

#include "RTXPTAutoExposureShared.h"

cbuffer g_AutoExposureConstants
{
    RTXPTAutoExposureConstants g_Const;
};

Texture2D<float4>                               t_HDR;
Texture2D<float4>                               t_Committed;
StructuredBuffer<RTXPTAutoExposureTile>         t_Tiles;
RWStructuredBuffer<RTXPTAutoExposureTile>       u_Tiles;
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> u_Pending;
VK_IMAGE_FORMAT("r32f") RWTexture2D<float>     u_Used;

groupshared float s_Sums[RTXPT_AUTO_EXPOSURE_REDUCE_GROUP];
groupshared uint  s_Counts[RTXPT_AUTO_EXPOSURE_REDUCE_GROUP];

// The base exposure the frame starts from: the seed, or the committed state.
float GetStartBase()
{
    return g_Const.Seed != 0 ? g_Const.SeedEV : t_Committed.Load(int3(0, 0, 0)).x;
}

// The exposure the frame uses: the base with the compensation, within the limits of the final exposure.
float GetUsedEV(float Base)
{
    return clamp(Base + g_Const.CompensationEV, g_Const.MinEV, g_Const.MaxEV);
}

// Whether every component is finite: an exponent of all ones is an infinity or a NaN.
bool IsFiniteColor(float3 Color)
{
    const uint3 ExponentMask = 0x7F800000u;
    return all((asuint(Color) & ExponentMask) != ExponentMask);
}

// Sums Sum and Count of the group's threads into thread 0 (Index 0).
void ReduceGroup(uint Index, float Sum, uint Count)
{
    s_Sums[Index]   = Sum;
    s_Counts[Index] = Count;
    GroupMemoryBarrierWithGroupSync();
    for (uint Stride = RTXPT_AUTO_EXPOSURE_REDUCE_GROUP / 2; Stride > 0; Stride /= 2)
    {
        if (Index < Stride)
        {
            s_Sums[Index] += s_Sums[Index + Stride];
            s_Counts[Index] += s_Counts[Index + Stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

// Writes the exposure the frame uses, before anything reads it: ray reconstruction and tone mapping.
[numthreads(1, 1, 1)]
void BeginFrame()
{
    u_Used[uint2(0, 0)] = exp2(GetUsedEV(GetStartBase()));
}

// Sums the log2 luminance of the valid pixels of one tile of the Width x Height top left of t_HDR. A pixel with an
// infinite or NaN component is left out; a negative or black luminance counts as RTXPT_AUTO_EXPOSURE_MIN_LUMINANCE.
[numthreads(RTXPT_AUTO_EXPOSURE_TILE_SIZE, RTXPT_AUTO_EXPOSURE_TILE_SIZE, 1)]
void MeterLuminance(uint3 Tile : SV_GroupID, uint3 Pixel : SV_DispatchThreadID, uint Index : SV_GroupIndex)
{
    float Sum   = 0;
    uint  Count = 0;
    if (Pixel.x < g_Const.Width && Pixel.y < g_Const.Height)
    {
        const float3 Color = t_HDR.Load(int3(Pixel.xy, 0)).rgb;
        if (IsFiniteColor(Color))
        {
            Sum   = log2(max(dot(Color, RTXPT_AUTO_EXPOSURE_LUMINANCE_WEIGHTS), RTXPT_AUTO_EXPOSURE_MIN_LUMINANCE));
            Count = 1;
        }
    }
    ReduceGroup(Index, Sum, Count);
    if (Index == 0)
    {
        RTXPTAutoExposureTile Result;
        Result.Sum                                     = s_Sums[0];
        Result.Count                                   = s_Counts[0];
        u_Tiles[Tile.y * g_Const.TilesX + Tile.x] = Result;
    }
}

// Reduces the tiles to the mean log2 luminance and writes the pending state (RTXPTAutoExposureShared.h). The base
// starts within the limits less the compensation, so it never accumulates beyond them. Without valid samples it holds,
// and an initialization waits for the first frame with some. The first valid meter after a seed sets the base to its
// target; later ones move it towards the target by 1 - exp(-DeltaSeconds / tau), tau the brighten time constant when
// the target is brighter and the darken one otherwise, a 0 reaching it at once and an invalid delta holding.
[numthreads(RTXPT_AUTO_EXPOSURE_REDUCE_GROUP, 1, 1)]
void Adapt(uint Index : SV_GroupIndex)
{
    float Sum   = 0;
    uint  Count = 0;
    for (uint I = Index; I < g_Const.TileCount; I += RTXPT_AUTO_EXPOSURE_REDUCE_GROUP)
    {
        const RTXPTAutoExposureTile Tile = t_Tiles[I];
        Sum += Tile.Sum;
        Count += Tile.Count;
    }
    ReduceGroup(Index, Sum, Count);
    if (Index != 0)
        return;

    const float4 Committed   = t_Committed.Load(int3(0, 0, 0));
    const float  Base        = GetStartBase();
    const bool   Initialized = g_Const.Seed == 0 && Committed.y != 0;
    const float  Low         = g_Const.MinEV - g_Const.CompensationEV;
    const float  High        = g_Const.MaxEV - g_Const.CompensationEV;
    const float  Start       = clamp(Base, Low, High);
    const uint   Samples     = s_Counts[0];

    float Next     = Start;
    float Target   = 0;
    float MeanLog  = 0;
    float Alpha    = 0;
    bool  BaseSet  = Initialized;
    uint  Event    = RTXPT_AUTO_EXPOSURE_EVENT_NO_SAMPLES;
    if (Samples != 0)
    {
        MeanLog = s_Sums[0] / float(Samples);
        Target  = clamp(g_Const.Log2Key - MeanLog, Low, High);
        if (!Initialized)
        {
            Next    = Target;
            Alpha   = 1;
            BaseSet = true;
            Event   = RTXPT_AUTO_EXPOSURE_EVENT_INITIALIZED;
        }
        else
        {
            const float Tau = Target > Start ? g_Const.BrightenSeconds : g_Const.DarkenSeconds;
            if (g_Const.ValidDelta == 0)
                Alpha = 0;
            else if (Tau == 0)
                Alpha = 1;
            else
                Alpha = 1 - exp(-g_Const.DeltaSeconds / Tau);
            Next  = clamp(Start + Alpha * (Target - Start), Low, High);
            Event = g_Const.ValidDelta != 0 ? RTXPT_AUTO_EXPOSURE_EVENT_ADAPTED : RTXPT_AUTO_EXPOSURE_EVENT_TIME_HELD;
        }
    }
    u_Pending[uint2(0, 0)] = float4(Next, BaseSet ? 1 : 0, Target, float(Event));
    u_Pending[uint2(1, 0)] = float4(MeanLog, float(Samples), Alpha, GetUsedEV(Base));
}
