#ifndef __RTXPT_TEMPORAL_MOTION_HLSLI__
#define __RTXPT_TEMPORAL_MOTION_HLSLI__

#include "DenoisingLayers.hlsli"
#include "PathTracerCamera.hlsli"

// This is an RTXPT guide contract, not a vendor alpha-channel convention. Projection removes camera
// jitter and uses local render pixels. Never substitute the direct primary motion for a reflected path.
float4 ComputeSpecularPathMotion(uint2 pixelPos, DenoisingLayersContext layers, SampleConstants constants)
{
    const CameraRay ray = ComputePathTracerCameraRay(constants.ptConsts, pixelPos, constants.ptConsts.sampleBaseIndex);
    float4 result = 0.0.xxxx;
    uint reflectedPaths = 0;
    bool valid = true;
    for (uint index = 0; index < min(constants.ptConsts.GetActiveDenoisingLayerCount(), cDenoisingLayerCount); ++index)
    {
        if (layers.GetBranchID(pixelPos, index) == cDenoisingLayerInvalidBranchID)
            continue;
        DenoisingLayer layer = layers.LoadDenoisingLayer(pixelPos, index);
        const uint vertex = layer.VertexIndexAndRoughness >> 16;
        if (vertex <= 1)
            continue;
        ++reflectedPaths;
        valid = valid && vertex == 2 && layer.HasValidHistory() && isfinite(layer.SceneLength);
        if (valid)
        {
            const float3 current = ray.origin + ray.dir * layer.SceneLength;
            valid = mul(float4(current, 1.0), constants.view.MatWorldToClipNoOffset).w > 0.0 &&
                mul(float4(layer.PreviousVirtualPosition.xyz, 1.0), constants.previousView.MatWorldToClipNoOffset).w > 0.0;
            result.xyz = ComputePathTracerMotionVector(current, layer.PreviousVirtualPosition.xyz,
                                                       constants.view, constants.previousView);
            valid = valid && all(isfinite(result.xyz)) && all(abs(result.xyz) <= HLF_MAX);
        }
    }
    return valid && reflectedPaths == 1 ? float4(result.xyz, 1.0) : 0.0.xxxx;
}

#endif
