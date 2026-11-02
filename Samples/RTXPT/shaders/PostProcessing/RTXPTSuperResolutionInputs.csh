// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma pack_matrix(row_major)

#define NUM_COMPUTE_THREADS_PER_DIM 8

#ifndef VK_IMAGE_FORMAT
#    define VK_IMAGE_FORMAT(format)
#endif

#include "PathTracer/PathTracerCamera.hlsli"

cbuffer g_Const
{
    SampleConstants g_Frame;
};

// Realtime guides: the dominant layer's ray distance (0 for sky/misses) and primary motion (w: valid history).
Texture2D<float>  t_RTXPTDepth;
Texture2D<float4> t_RTXPTMotionVectors;

VK_IMAGE_FORMAT("r32f")  RWTexture2D<float>  u_SRDepth;
VK_IMAGE_FORMAT("rg16f") RWTexture2D<float2> u_SRMotionVectors;
VK_IMAGE_FORMAT("r8")    RWTexture2D<float>  u_SRIgnoreHistoryMask;

float RTXPTNormalizedDeviceZToDepth(float NdcZ)
{
#if defined(NDC_MIN_Z) && defined(F3NDC_XYZ_TO_UVD_SCALE)
    return (NdcZ - NDC_MIN_Z) * F3NDC_XYZ_TO_UVD_SCALE.z;
#else
    return NdcZ;
#endif
}

[numthreads(NUM_COMPUTE_THREADS_PER_DIM, NUM_COMPUTE_THREADS_PER_DIM, 1)]
void main(uint3 DispatchThreadID : SV_DispatchThreadID)
{
    uint Width;
    uint Height;
    u_SRDepth.GetDimensions(Width, Height);

    const uint2 PixelPos = DispatchThreadID.xy;
    if (PixelPos.x >= Width || PixelPos.y >= Height)
        return;

    const float  RayDistance = t_RTXPTDepth[PixelPos];
    const float4 Motion      = t_RTXPTMotionVectors[PixelPos];
    const bool   bSurface    = isfinite(RayDistance) && RayDistance > 0.0;

    // The ray distance is the one the motion vectors were computed with (a virtual point behind a replaced primary
    // surface), so depth and motion describe the same point. BUILD's camera ray carries the same jitter, micro-jitter,
    // lens sample and near-plane origin offset. Jitter only offsets clip XY, so depth matches the jittered projection.
    float Depth = RTXPTNormalizedDeviceZToDepth(1.0);
    if (bSurface)
    {
        const CameraRay Ray      = ComputePathTracerCameraRay(g_Frame.ptConsts, PixelPos, g_Frame.ptConsts.sampleBaseIndex);
        const float3    WorldPos = Ray.origin + Ray.dir * RayDistance;
        const float4    Clip     = mul(float4(WorldPos, 1.0), g_Frame.view.MatWorldToClipNoOffset);
        if (all(isfinite(Clip)) && Clip.w > 0.0)
            Depth = saturate(RTXPTNormalizedDeviceZToDepth(Clip.z / Clip.w));
    }

    // Sky keeps its camera motion and its history; a surface without valid correspondence for every retained layer
    // (conservatively, not only the primary hit) discards it.
    const bool bMotionFinite = all(isfinite(Motion.xy));
    u_SRDepth[PixelPos]             = Depth;
    u_SRMotionVectors[PixelPos]     = bMotionFinite ? clamp(Motion.xy, -HLF_MAX, HLF_MAX) : float2(0.0, 0.0);
    u_SRIgnoreHistoryMask[PixelPos] = bSurface && (!bMotionFinite || !(Motion.w > 0.0)) ? 1.0 : 0.0;
}
