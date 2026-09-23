#ifndef __LIGHT_PACKING_HLSLI__
#define __LIGHT_PACKING_HLSLI__

#include "PolymorphicLight.h"
#include "../PathTracerShared.h"

uint DecodeLightType(PolymorphicLightInfo light)
{
    return (light.ColorTypeAndFlags >> kPolymorphicLightTypeShift) & kPolymorphicLightTypeMask;
}

float UnpackLightRadiance(uint packed)
{
    return packed == 0u ? 0.0 : exp2(float(packed - 1u) / 65534.0 *
        (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance) + kPolymorphicLightMinLog2Radiance);
}

float3 UnpackLightColor(PolymorphicLightInfo light)
{
    const float3 color = float3(light.ColorTypeAndFlags & 255u,
        (light.ColorTypeAndFlags >> 8u) & 255u, (light.ColorTypeAndFlags >> 16u) & 255u) / 255.0;
    return color * UnpackLightRadiance(light.LogRadiance & 0xffffu);
}

void PackLightColor(float3 radiance, inout PolymorphicLightInfo light)
{
    const float intensity = max(radiance.x, max(radiance.y, radiance.z));
    if (intensity > 0.0)
    {
        const float logRadiance = saturate((log2(intensity) - kPolymorphicLightMinLog2Radiance) /
            (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance));
        const uint packed = min(uint(ceil(logRadiance * 65534.0)) + 1u, 0xffffu);
        const uint3 rgb = uint3(floor(saturate(radiance / UnpackLightRadiance(packed)) * 255.0 + 0.5));
        light.LogRadiance |= packed;
        light.ColorTypeAndFlags |= rgb.x | (rgb.y << 8u) | (rgb.z << 16u);
    }
}

float3 UnpackLightDirection(uint packed)
{
    float2 p = saturate(float2(packed & 0xffffu, packed >> 16u) / 65534.0);
    p = p * 2.0 - 1.0;
    p = p * 2.0 - 1.0; // Match upstream OctToNDirUnorm32 / Decode_Oct.
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    const float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

AnalyticLightData UnpackAnalyticLight(PolymorphicLightInfoFull light)
{
    AnalyticLightData result = (AnalyticLightData)0;
    const uint type = DecodeLightType(light.Base);
    result.colorType = float4(UnpackLightColor(light.Base), float(type));
    result.positionRadius = float4(light.Base.Center, f16tof32(light.Base.Scalars & 0xffffu));
    result.shaping = float4(-1.0, 0.0, 0.0, 0.0);
    if (type == kPolymorphicLightTypeDirectional)
    {
        result.directionRange.xyz = UnpackLightDirection(light.Base.Direction1);
        result.shaping.w = f16tof32(light.Base.Scalars >> 16u);
    }
    if (light.Base.HasLightShaping())
    {
        result.directionRange.xyz = UnpackLightDirection(light.Extended.PrimaryAxis);
        result.shaping.xy = f16tof32(uint2(light.Extended.CosConeAngleAndSoftness & 0xffffu,
            light.Extended.CosConeAngleAndSoftness >> 16u));
        result.shaping.z = (light.Base.ColorTypeAndFlags & kPolymorphicLightShapingUseMinFalloff) != 0u ? 0.0001 : 0.0;
    }
    return result;
}

PolymorphicLightInfo PackTriangleLight(EmissiveTriangle tri)
{
    PolymorphicLightInfo result = (PolymorphicLightInfo)0;
    result.Center = tri.base.xyz + (tri.edge1.xyz + tri.edge2.xyz) / 3.0;
    result.Direction1 = f32tof16(tri.edge1.x) | (f32tof16(tri.edge2.x) << 16u);
    result.Direction2 = f32tof16(tri.edge1.y) | (f32tof16(tri.edge2.y) << 16u);
    result.Scalars = f32tof16(tri.edge1.z) | (f32tof16(tri.edge2.z) << 16u);
    result.ColorTypeAndFlags = kPolymorphicLightTypeTriangle << kPolymorphicLightTypeShift;
    PackLightColor(tri.radiance.xyz, result);
    return result;
}

EmissiveTriangle UnpackTriangleLight(PolymorphicLightInfo light)
{
    EmissiveTriangle result;
    const uint3 edges = uint3(light.Direction1, light.Direction2, light.Scalars);
    result.edge1 = float4(f16tof32(edges & 0xffffu), 0.0);
    result.edge2 = float4(f16tof32(edges >> 16u), 0.0);
    result.base = float4(light.Center - (result.edge1.xyz + result.edge2.xyz) / 3.0, 0.0);
    result.radiance = float4(UnpackLightColor(light), 0.0);
    return result;
}

#endif
