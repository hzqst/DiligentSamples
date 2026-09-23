#ifndef __POLYMORPHIC_LIGHT_HLSLI__
#define __POLYMORPHIC_LIGHT_HLSLI__

#include "../PathTracerShared.h"

static const float kLightTwoPi        = 6.28318530717958647692;
static const float kAnalyticLightTMin = 1.0e-4;

// Area-measure -> solid-angle-measure pdf conversion (RTXPT-fork Utils/Geometry.hlsli:79).
float pdfAtoW(float pdfA, float distance, float cosTheta)
{
    return pdfA * (distance * distance) / max(cosTheta, 2e-9);
}

// Uniformly sampled barycentric coordinates inside a triangle (RTXPT-fork Utils/Geometry.hlsli:33).
float3 SampleTriangleUniform(float2 rnd)
{
    const float sqrtx = sqrt(rnd.x);
    return float3(1.0 - sqrtx, sqrtx * (1.0 - rnd.y), sqrtx * rnd.y);
}

struct LightSample
{
    float3 dir;
    float  distance;
    float3 radiance;
    float  solidAnglePdf;
    bool   valid;
};

LightSample LightSample_make_empty()
{
    LightSample ls;
    ls.dir      = float3(0.0, 1.0, 0.0);
    ls.distance = 0.0;
    ls.radiance = float3(0.0, 0.0, 0.0);
    ls.solidAnglePdf = 0.0;
    ls.valid    = false;
    return ls;
}

bool tryNormalize(float3 v, out float3 dir)
{
    const float lenSq = dot(v, v);
    if (lenSq <= 1e-12)
    {
        dir = float3(0.0, 0.0, 0.0);
        return false;
    }

    dir = v * rsqrt(lenSq);
    return true;
}

void MakeLightBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    tangent          = normalize(cross(up, normal));
    bitangent        = cross(normal, tangent);
}

float3 SampleConeUniform(float2 random, float3 axis, float cosThetaMax, out float pdf)
{
    const float oneMinusCosThetaMax = max(1.0 - cosThetaMax, 1.0e-8);
    const float phi                 = kLightTwoPi * random.x;
    const float cosTheta            = 1.0 - random.y * oneMinusCosThetaMax;
    const float sinTheta            = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));

    float sinPhi;
    float cosPhi;
    sincos(phi, sinPhi, cosPhi);

    float3 tangent;
    float3 bitangent;
    MakeLightBasis(axis, tangent, bitangent);

    pdf = min(kMaxSolidAnglePdf, 1.0 / (kLightTwoPi * oneMinusCosThetaMax));
    return normalize(tangent * (cosPhi * sinTheta) + bitangent * (sinPhi * sinTheta) + axis * cosTheta);
}

float EvalSpotFalloff(AnalyticLightData light, float3 dirToLight)
{
    if (light.shaping.x < -0.5)
        return 1.0;

    float3 lightDir;
    if (!tryNormalize(light.directionRange.xyz, lightDir))
        return 0.0;

    const float cosTheta = dot(lightDir, -dirToLight);
    return light.shaping.y > 0.0 ? smoothstep(light.shaping.x, light.shaping.x + light.shaping.y, cosTheta) :
                                  (cosTheta >= light.shaping.x ? 1.0 : 0.0);
}

LightSample SampleSphereLight(AnalyticLightData light, float2 random, float3 surfacePos)
{
    const float3 radiance  = max(light.colorType.rgb, float3(0.0, 0.0, 0.0));
    const float  maxEnergy = max(radiance.x, max(radiance.y, radiance.z));
    if (maxEnergy <= 0.0)
        return LightSample_make_empty();

    const float3 center   = light.positionRadius.xyz;
    const float  radius   = max(light.positionRadius.w, kAnalyticLightTMin);
    const float3 toCenter = center - surfacePos;
    const float  distSq   = dot(toCenter, toCenter);
    const float  radiusSq = radius * radius;
    if (distSq <= radiusSq)
        return LightSample_make_empty();

    const float distanceToCenter = sqrt(distSq);
    const float range            = light.directionRange.w;
    if (range > 0.0 && distanceToCenter > range)
        return LightSample_make_empty();

    const float3 axis        = toCenter / distanceToCenter;
    const float  cosThetaMax = sqrt(max(0.0, 1.0 - radiusSq / distSq));
    float        solidAnglePdf;
    const float3 dir = SampleConeUniform(random, axis, cosThetaMax, solidAnglePdf);

    const float projection = dot(toCenter, dir);
    const float closestSq  = max(0.0, distSq - projection * projection);
    if (closestSq > radiusSq)
        return LightSample_make_empty();

    const float hitDistance = projection - sqrt(max(0.0, radiusSq - closestSq));
    if (hitDistance <= kAnalyticLightTMin)
        return LightSample_make_empty();

    const float falloff = EvalSpotFalloff(light, dir);
    if (falloff <= 0.0)
        return LightSample_make_empty();

    LightSample ls     = LightSample_make_empty();
    ls.dir             = dir;
    ls.distance        = hitDistance;
    ls.radiance        = radiance * falloff;
    ls.solidAnglePdf   = solidAnglePdf;
    ls.valid           = true;
    return ls;
}

LightSample SampleDirectionalLight(AnalyticLightData light, float2 random)
{
    const float3 radiance  = max(light.colorType.rgb, float3(0.0, 0.0, 0.0));
    const float  maxEnergy = max(radiance.x, max(radiance.y, radiance.z));
    if (maxEnergy <= 0.0)
        return LightSample_make_empty();

    float3 axis;
    if (!tryNormalize(-light.directionRange.xyz, axis))
        return LightSample_make_empty();

    const float solidAngle   = max(light.shaping.w, 1.0e-8);
    const float cosThetaMax  = clamp(1.0 - solidAngle / kLightTwoPi, -1.0, 1.0);
    float       sampledPdf   = 0.0;

    LightSample ls     = LightSample_make_empty();
    ls.dir             = SampleConeUniform(random, axis, cosThetaMax, sampledPdf);
    ls.distance        = 1.0e30;
    ls.radiance        = radiance;
    ls.solidAnglePdf   = sampledPdf;
    ls.valid           = true;
    return ls;
}

LightSample SamplePointLight(AnalyticLightData light, float3 surfacePos)
{
    const float3 toLight = light.positionRadius.xyz - surfacePos;
    const float  distSq  = dot(toLight, toLight);
    if (distSq <= kAnalyticLightTMin * kAnalyticLightTMin)
        return LightSample_make_empty();

    const float  dist = sqrt(distSq);
    const float3 dir  = toLight / dist;
    const float  range = light.directionRange.w;
    if (range > 0.0 && dist > range)
        return LightSample_make_empty();

    const float falloff = EvalSpotFalloff(light, dir);
    if (falloff <= 0.0)
        return LightSample_make_empty();

    LightSample ls     = LightSample_make_empty();
    ls.dir             = dir;
    ls.distance        = dist;
    ls.radiance        = max(light.colorType.rgb, float3(0.0, 0.0, 0.0)) * (falloff / max(distSq, 1.0e-6));
    ls.solidAnglePdf   = 1.0;
    ls.valid           = true;
    return ls;
}

LightSample SampleAnalyticLight(AnalyticLightData light, float2 random, float3 surfacePos)
{
    if (light.colorType.w < 0.0)
        return LightSample_make_empty();

    const uint type = uint(light.colorType.w + 0.5);
    if (type == kPolymorphicLightTypeSphere)
        return SampleSphereLight(light, random, surfacePos);
    if (type == kPolymorphicLightTypeDirectional)
        return SampleDirectionalLight(light, random);
    if (type == kPolymorphicLightTypePoint)
        return SamplePointLight(light, surfacePos);

    return LightSample_make_empty();
}

#endif // __POLYMORPHIC_LIGHT_HLSLI__
