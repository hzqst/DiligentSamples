#ifndef __PATH_TRACER_NESTED_DIELECTRICS_HLSLI__
#define __PATH_TRACER_NESTED_DIELECTRICS_HLSLI__

#include "BSDFs/MaterialBridge.hlsli"
#include "BSDFs/InteriorList.hlsli"

namespace PathTracer
{
#if RTXPT_NESTED_DIELECTRICS_QUALITY == 1
    static const uint kMaxRejectedDielectricHits = 4u;
#    define NESTED_DIELECTRICS_AVOID_TERMINATION 1
#elif RTXPT_NESTED_DIELECTRICS_QUALITY == 2
    static const uint kMaxRejectedDielectricHits = 16u;
#    define NESTED_DIELECTRICS_AVOID_TERMINATION 0
#else
#    define NESTED_DIELECTRICS_AVOID_TERMINATION 0
#endif

    float ComputeOutsideIoR(InteriorList interiorList, uint materialID, bool entering)
    {
        uint outsideMaterialID = interiorList.getTopMaterialID();
        if (!entering && outsideMaterialID == materialID)
            outsideMaterialID = interiorList.getNextMaterialID();

        if (outsideMaterialID == InteriorList::kNoMaterial)
            return 1.0;

        return Bridge::loadIoR(outsideMaterialID);
    }

    uint GetMaxRejectedDielectricHits()
    {
#if RTXPT_NESTED_DIELECTRICS_QUALITY > 0
        return kMaxRejectedDielectricHits;
#else
        return 0u;
#endif
    }

} // namespace PathTracer

#endif // __PATH_TRACER_NESTED_DIELECTRICS_HLSLI__
