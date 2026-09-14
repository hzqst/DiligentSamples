#ifndef __ENVIRONMENT_LIGHT_HLSLI__
#define __ENVIRONMENT_LIGHT_HLSLI__

#include "EnvironmentLightConfig.h"

// Same node/radiance/power semantics as RTXPT-fork EnvironmentQuadLight;
// the Diligent buffer retains full precision instead of polymorphic light packing.
struct EnvironmentQuadLight
{
    uint NodeX;
    uint NodeY;
    uint NodeDim;
    uint Padding;
    float3 Radiance;
    float Weight;
};

uint EQTNodePack(uint dim, uint x, uint y)
{
    return (uint(firstbithigh(dim)) << 28u) | (x << 14u) | y;
}

void EQTNodeUnpack(uint packed, out uint dim, out uint x, out uint y)
{
    dim = 1u << (packed >> 28u);
    x = (packed >> 14u) & 0x3fffu;
    y = packed & 0x3fffu;
}

float EnvironmentQuadSolidAnglePdf(EnvironmentQuadLight light)
{
    return float(light.NodeDim * light.NodeDim) / (4.0 * 3.14159265358979323846);
}

uint EQTNodeWeightPack(float weight, uint index)
{
    // Preserve upstream FastSqrt's IEEE-754 approximation and packed-weight ties.
    const float root = asfloat(0x1fbd1df5 + (asint(weight) >> 1));
    return (min(uint(root * 100.0 + 0.5), 0xfffffu) << 12u) | index;
}

#endif
