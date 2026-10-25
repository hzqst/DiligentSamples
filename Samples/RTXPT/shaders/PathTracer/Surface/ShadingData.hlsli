#ifndef __SHADING_DATA_HLSLI__
#define __SHADING_DATA_HLSLI__

#include "../PathTracerShared.h"
#include "../PathTracerHelpers.hlsli"
#include "../BSDFs/LobeType.hlsli"

namespace PathTracer
{
    struct DenoisingLayerMaterialState
    {
        uint flags;
        uint nestedPriority;
        uint activeLobes;
        uint psdExclude;
        uint psdBlockMotionVectorsAtSurface;
        uint psdDominantDeltaLobeP1;

        bool isPSDExclude() { return psdExclude != 0u; }
        bool isPSDBlockMotionVectorsAtSurface() { return psdBlockMotionVectorsAtSurface != 0u; }
        uint getPSDDominantDeltaLobeP1() { return psdDominantDeltaLobeP1; }
        bool isThinSurface() { return (flags & kMaterialFlagThinSurface) != 0u; }
        uint getNestedPriority() { return nestedPriority; }
        uint getActiveLobes() { return activeLobes == 0u ? kLobeTypeAll : activeLobes; }
    };

    struct ShadingData
    {
        float3 posW;
        float3 N;
        float3 V;
        float3 T;
        float3 B;
        uint   materialID;
        bool   frontFacing;
        DenoisingLayerMaterialState mtl;
        float3 faceNCorrected;
        float3 vertexN;
        float  shadowNoLFadeout;
        float3 emission;
        float posErrorScale;
        lpfloat exteriorIoR;
        lpfloat interiorIoR;

        float3 computeNewRayOrigin(bool viewSide)
        {
            return ComputeRayOrigin(posW, viewSide ? faceNCorrected : -faceNCorrected, posErrorScale);
        }
    };
}
#endif
