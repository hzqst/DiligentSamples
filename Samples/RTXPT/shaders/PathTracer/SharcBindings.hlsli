// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#ifndef RTXPT_SHARC_BINDINGS_HLSLI
#define RTXPT_SHARC_BINDINGS_HLSLI
#define SHARC_MATERIAL_DEMODULATION 1
#define SHARC_SEPARATE_EMISSIVE 1
#define SHARC_RESAMPLING_DEPTH_MIN 2
#define SHARC_PROPAGATION_DEPTH 3
#define SHARC_UPDATE RTXPT_SHARC_TRAIN
#include "SharcCommon.h"
#include "SharcConstants.h"
#if RTXPT_SHARC_PATH_STATE_STRIDE != 16 * (1 + SHARC_PROPAGATION_DEPTH)
#error SHaRC scratch stride must match propagation depth
#endif
cbuffer cbSharc { SharcConstants g_Sharc; }
RWStructuredBuffer<uint64_t> u_SharcHash;
RWStructuredBuffer<SharcAccumulationData> u_SharcAccumulation;
RWStructuredBuffer<SharcPackedData> u_SharcResolved;
RWByteAddressBuffer u_SharcPathState;
SharcParameters GetSharcParameters()
{
    SharcParameters p;
    p.hashGridParameters.cameraPosition = g_Sharc.CameraPosition.xyz;
    p.hashGridParameters.sceneScale = g_Sharc.SceneScale;
    p.hashGridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    p.hashGridParameters.levelBias = SHARC_GRID_LEVEL_BIAS;
    p.hashGridData.capacity = g_Sharc.EntriesNum;
    p.hashGridData.hashEntriesBuffer = u_SharcHash;
    p.radianceScale = 1.0e3f;
    p.accumulationBuffer = u_SharcAccumulation;
    p.resolvedBuffer = u_SharcResolved;
    return p;
}
#endif
