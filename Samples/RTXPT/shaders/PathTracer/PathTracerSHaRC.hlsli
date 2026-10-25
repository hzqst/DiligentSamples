/*
 * Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto. Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */
// Diligent resource/surface adapter for RTXPT-fork 9503910 PathTracerSHaRC.hlsli.
#ifndef __PATH_TRACER_SHARC_HLSLI__
#define __PATH_TRACER_SHARC_HLSLI__
#if RTXPT_SHARC_TRAIN || RTXPT_SHARC_CONSUME
#include "SharcBindings.hlsli"
namespace PathTracer
{
#if RTXPT_SHARC_TRAIN
    uint SharcPathStateOffset()
    {
        return (DispatchRaysIndex().y * DispatchRaysDimensions().x + DispatchRaysIndex().x) * RTXPT_SHARC_PATH_STATE_STRIDE;
    }
    SharcState SharcLoadState()
    {
        SharcState s;
        uint offset = SharcPathStateOffset();
        s.pathLength = u_SharcPathState.Load(offset);
        [unroll] for (uint i = 0; i < SHARC_PROPAGATION_DEPTH; ++i)
        {
            uint4 v = u_SharcPathState.Load4(offset + 16 + 16 * i);
            s.cacheIndices[i] = v.x;
            s.sampleWeights[i] = float3(asfloat(v.y), asfloat(v.z), asfloat(v.w));
        }
        return s;
    }
    void SharcStoreState(SharcState s)
    {
        uint offset = SharcPathStateOffset();
        u_SharcPathState.Store(offset, s.pathLength);
        [unroll] for (uint i = 0; i < SHARC_PROPAGATION_DEPTH; ++i)
            u_SharcPathState.Store4(offset + 16 + 16 * i, uint4(s.cacheIndices[i], asuint(s.sampleWeights[i])));
    }
    void SharcThroughputCheckpoint(inout PathState path)
    {
        SharcState s = SharcLoadState();
        SharcSetThroughput(s, path.GetThp());
        SharcStoreState(s);
        path.SetThp(1.0.xxx);
    }
    void SharcBackPropagate(float3 radiance)
    {
        SharcUpdateMiss(GetSharcParameters(), SharcLoadState(), radiance);
    }
#endif
    SharcHitData SharcPrepareHit(inout SurfaceData surface, float3 emission)
    {
        SharcHitData hit = (SharcHitData)0;
        hit.positionWorld = surface.shadingData.posW;
        hit.normalWorld = surface.shadingData.faceNCorrected;
        float3 specular = surface.bsdf.standardData.Specular();
        hit.materialDemodulation = max((float3)surface.bsdf.standardData.Diffuse(), .05) +
            max(specular, .02) * luminance(specular + (1.0 - specular) * .047619);
        hit.emissive = emission;
#if RTXPT_SHARC_TRAIN
        if ((FalcorBSDF::getLobes(surface.bsdf.standardData) & kLobeTypeNonDelta) != 0)
            surface.bsdf.standardData.SetRoughness(max(g_Sharc.MinRoughnessOverride, (float)surface.bsdf.standardData.Roughness()));
#endif
        return hit;
    }
#if RTXPT_SHARC_CONSUME
    bool SharcQueryHit(SharcHitData hit, SurfaceData surface, float rayT, PathState path, out float3 radiance)
    {
        SharcParameters p = GetSharcParameters();
        uint level = HashGridGetLevel(hit.positionWorld, p.hashGridParameters);
        float voxel = HashGridGetVoxelSize(level, p.hashGridParameters);
        radiance = 0.0.xxx;
        // BUILD already owns the deterministic branch: consuming there would discard radiance in FILL.
        bool valid = !path.hasFlag(PathFlags::denoisingLayerOnBranch) && rayT > voxel * sqrt(3.0) &&
            path.rayCone.getWidth() > voxel * g_Sharc.TerminateThreshold &&
            (FalcorBSDF::getLobes(surface.bsdf.standardData) & kLobeTypeNonDelta) != 0;
        bool found = valid && SharcGetCachedRadiance(p, hit, radiance, false);
        if (g_Sharc.DebugMode != 0 && !path.hasFlag(PathFlags::denoisingLayerOnBranch))
        {
            if (g_Sharc.DebugMode == 1) { radiance = 0.0.xxx; SharcGetCachedRadiance(p, hit, radiance, false); }
            if (g_Sharc.DebugMode == 2) radiance = HashGridDebugColoredHash(hit.positionWorld, hit.normalWorld, p.hashGridParameters);
            if (g_Sharc.DebugMode == 3) radiance = HashGridDebugOccupancy(path.GetPixelPos(), g_Const.ptConsts.camera.ViewportSize, p.hashGridData);
            if (g_Sharc.DebugMode == 4) radiance = HashGridDebugHashCollisions(hit.positionWorld, hit.normalWorld, p.hashGridParameters, p.hashGridData);
            if (g_Sharc.DebugMode == 5) radiance = voxel.xxx;
            if (g_Sharc.DebugMode == 6) radiance = float(level).xxx;
            found = true;
        }
        return found;
    }
#endif
}
#endif
#endif
