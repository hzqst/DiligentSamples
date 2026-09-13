// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#ifndef __PATH_TRACER_CAMERA_HLSLI__
#define __PATH_TRACER_CAMERA_HLSLI__

#include "PathTracerHelpers.hlsli"
#include "Utils/SampleGenerators.hlsli"

// Shared by BUILD and RR depth conversion: includes micro-jitter, lens sampling
// and the near-plane origin offset, so ray distance reconstructs the same point.
CameraRay ComputePathTracerCameraRay(PathTracerConstants constants, uint2 pixelPos, uint sampleIndex)
{
    SampleGenerator sg = SampleGenerator_makeStateless(pixelPos, 0u, sampleIndex, kSampleEffect_Base);
    const float2 subPixelOffset = constants.camera.Jitter +
        (sampleNext2D(sg) - 0.5.xx) * constants.perPixelJitterAAScale;
    const float2 cameraDoFSample = sampleNext2D(sg);
    return ComputeRayThinlens(constants.camera, pixelPos, subPixelOffset, cameraDoFSample);
}

float3 ComputePathTracerMotionVector(float3 posW, float3 prevPosW, PathTracerViewData view, PathTracerViewData previousView)
{
    float4 clipPos = mul(float4(posW, 1.0), view.MatWorldToClipNoOffset);
    float4 prevClipPos = mul(float4(prevPosW, 1.0), previousView.MatWorldToClipNoOffset);
    clipPos.xyz /= clipPos.w;
    prevClipPos.xyz /= prevClipPos.w;
    if (clipPos.w <= 0.0 || prevClipPos.w <= 0.0)
        return float3(0.0, 0.0, 0.0);
    float3 motion;
    motion.xy = (prevClipPos.xy - clipPos.xy) * view.ClipToWindowScale;
    motion.z = prevClipPos.w - clipPos.w;
    return motion;
}

#endif // __PATH_TRACER_CAMERA_HLSLI__
