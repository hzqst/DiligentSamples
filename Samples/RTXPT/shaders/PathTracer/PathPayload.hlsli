#ifndef __PATH_PAYLOAD_HLSLI__
#define __PATH_PAYLOAD_HLSLI__

#include "Config.h"

// Packed and aligned representation of PathState in a pre-raytrace state.
struct PAYLOAD_QUALIFIER PathPayload
{
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
    uint4 packed[6] PAYLOAD_FIELD_RW_ALL;
#else
    uint4 packed[5] PAYLOAD_FIELD_RW_ALL;
#endif

#ifdef PATH_STATE_DEFINED
    static PathPayload pack(const PathState path);
    static PathState unpack(const PathPayload p);
#endif
};

#ifdef PATH_STATE_DEFINED

PathPayload PathPayload::pack(const PathState path)
{
    PathPayload p;

    p.packed[0] = path.PackOriginId;
    p.packed[1] = path.PackDirSceneLength;

    p.packed[2].xy = path.pack23;
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
    p.packed[2].zw = path.imageXformPacked;
    p.packed[5] = asuint(path.previousReflectionPlane);
#else
    p.packed[2].zw = path.pack45;
#endif

#if RTXPT_NESTED_DIELECTRICS_QUALITY > 0
    p.packed[3].xy = uint2(path.interiorList.slots[0], path.interiorList.slots[1]);
#else
    p.packed[3].xy = 0;
#endif
    p.packed[3].z = path.packedCounters;
    p.packed[3].w = path.stableBranchID;

    p.packed[4].x = path.rayCone.widthSpreadAngleFP16;
    p.packed[4].y = path.pack0;
    p.packed[4].z = path.pack1;
    p.packed[4].w = path.flagsAndVertexIndex;

    return p;
}

PathState PathPayload::unpack(const PathPayload p)
{
    PathState path;

    path.PackOriginId       = p.packed[0];
    path.PackDirSceneLength = p.packed[1];

    path.pack23 = p.packed[2].xy;
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
    path.imageXformPacked = p.packed[2].zw;
    path.previousReflectionPlane = asfloat(p.packed[5]);
#else
    path.pack45 = p.packed[2].zw;
#endif

#if RTXPT_NESTED_DIELECTRICS_QUALITY > 0
    path.interiorList.slots[0] = p.packed[3].x;
    path.interiorList.slots[1] = p.packed[3].y;
#endif
    path.packedCounters = p.packed[3].z;
    path.stableBranchID = p.packed[3].w;

    path.rayCone.widthSpreadAngleFP16 = p.packed[4].x;
    path.pack0                        = p.packed[4].y;
    path.pack1                        = p.packed[4].z;
    path.flagsAndVertexIndex           = p.packed[4].w;

    return path;
}

#endif

#endif // __PATH_PAYLOAD_HLSLI__
