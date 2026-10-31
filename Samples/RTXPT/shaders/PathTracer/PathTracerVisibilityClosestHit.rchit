#include "Config.h"
#include "PathState.hlsli"
#include "PathPayload.hlsli"

// Visibility rays always use RAY_FLAG_SKIP_CLOSEST_HIT_SHADER; the pipeline API still requires this stage.
[shader("closesthit")]
void main(inout PathPayload Payload, BuiltInTriangleIntersectionAttributes Attributes)
{
}
