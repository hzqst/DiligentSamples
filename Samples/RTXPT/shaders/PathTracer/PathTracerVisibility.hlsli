#ifndef __PATH_TRACER_VISIBILITY_HLSLI__
#define __PATH_TRACER_VISIBILITY_HLSLI__

#include "PathTracerBridge.hlsli"
#include "Rendering/Materials/MaterialBridge.hlsli"

namespace Bridge
{
    // Mirrors RTXPT-fork AlphaTestImpl. BLEND opacity is not a stochastic hit filter.
    bool AlphaTestImpl(SubInstanceData subInstance, uint triangleIndex, float2 rayBarycentrics)
    {
        const MaterialPTData material = getMaterial(subInstance.MaterialID);
        if ((material.flags & kMaterialFlagAlphaTested) == 0u)
            return true;

        GeometryVertexData v0, v1, v2;
        getTriangleVertices(subInstance, triangleIndex, v0, v1, v2);
        return alphaTestPasses(material, interpolateTexCoord(v0, v1, v2, rayBarycentrics));
    }

    bool AlphaTestVisibilityRay(uint instanceID, uint geometryIndex, uint triangleIndex, float2 rayBarycentrics)
    {
        const SubInstanceData subInstance = t_SubInstanceData[instanceID + geometryIndex];
        if ((getMaterial(subInstance.MaterialID).flags & kMaterialFlagExcludeFromNEE) != 0u)
            return false;
        return AlphaTestImpl(subInstance, triangleIndex, rayBarycentrics);
    }

#if RTXPT_USE_INLINE_VISIBILITY
    bool traceVisibilityRay(RayDesc ray)
    {
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rayQuery;
        rayQuery.TraceRayInline(t_SceneBVH, RAY_FLAG_NONE, 0xFF, ray);
        while (rayQuery.Proceed())
        {
            if (rayQuery.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            {
                if (AlphaTestVisibilityRay(rayQuery.CandidateInstanceID(), rayQuery.CandidateGeometryIndex(),
                                           rayQuery.CandidatePrimitiveIndex(), rayQuery.CandidateTriangleBarycentrics()))
                    rayQuery.CommitNonOpaqueTriangleHit();
            }
        }
        return rayQuery.CommittedStatus() == COMMITTED_NOTHING;
    }
#endif
}

#endif // __PATH_TRACER_VISIBILITY_HLSLI__
