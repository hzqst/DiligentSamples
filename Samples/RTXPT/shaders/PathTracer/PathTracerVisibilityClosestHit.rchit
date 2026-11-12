#include "Config.h"
#include "PathState.hlsli"
#include "PathPayload.hlsli"

// Visibility rays use RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, except the sun shadow ray of the sun shadow denoiser
// (PathTracer::TraceVisibilityRayHitDistance): it reads the distance to the closest occluder from the first payload
// word, which this shader writes. The path stays active, as the visibility miss shader alone terminates it.
[shader("closesthit")]
void main(inout PathPayload Payload, BuiltInTriangleIntersectionAttributes Attributes)
{
    Payload.packed[0].x = asuint(RayTCurrent());
}
