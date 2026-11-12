// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include "BasicMath.hpp"
#include "../shaders/PathTracer/Lighting/PolymorphicLight.h"

namespace Diligent
{

static_assert(offsetof(PolymorphicLightInfo, ColorTypeAndFlags) == 12, "Light flags offset");
static_assert(offsetof(PolymorphicLightInfo, Direction1) == 16, "Light direction offset");
static_assert(offsetof(PolymorphicLightInfo, LogRadiance) == 28, "Light radiance offset");
static_assert(offsetof(PolymorphicLightInfoEx, ClusterIndexAndCount) == 12, "Light identity offset");

// CPU light authoring values. This type is never uploaded as a shader record.
struct AnalyticLightData
{
    float4 colorType      = float4{0, 0, 0, -1};
    float4 positionRadius = float4{0, 0, 0, 0};
    float4 directionRange = float4{0, -1, 0, 0};
    float4 shaping        = float4{-1, 0, 0, 0};
};

// AnalyticLightData::colorType.w of each authored light type, the packed PolymorphicLightInfo type.
constexpr float kPolymorphicLightTypeSphere      = 0.0f;
constexpr float kPolymorphicLightTypeDirectional = 2.0f;
constexpr float kPolymorphicLightTypePoint       = 4.0f;

inline Uint32 PackLightFloat16(float Value)
{
    // Match the host conversion in RTXPT-fork LightsBaker.cpp (truncation).
    const float Biased = Value * std::ldexp(1.0f, -112);
    Uint32      Bits   = 0;
    std::memcpy(&Bits, &Biased, sizeof(Bits));
    return (((Bits & 0x80000000u) >> 16u) | ((Bits & 0x0fffffffu) >> 13u)) & 0xffffu;
}

// The inverse of PackLightFloat16, as f16tof32 decodes it in the shaders.
inline float UnpackLightFloat16(Uint32 Packed)
{
    const Uint32 Bits = ((Packed & 0x8000u) << 16u) | ((Packed & 0x7fffu) << 13u);
    float        Biased;
    std::memcpy(&Biased, &Bits, sizeof(Biased));
    return Biased * std::ldexp(1.0f, 112);
}

// The smallest solid angle PackLightFloat16 keeps, the smallest FP16 subnormal: the disk of a hard directional light.
// A smaller solid angle would be packed as 0, and the light would lose its illuminance.
constexpr float kRTXPTDirectionalLightMinSolidAngle = 0x1p-24f;

// The solid angle of a directional light's disk of the given full angular diameter in radians, as PackAnalyticLight
// stores it. 4 * PI * sin^2(diameter / 4) is 2 * PI * (1 - cos(diameter / 2)) without the float cancellation that
// rounds small disks to 0. The result is raised to the hard-light floor and rounded to its packed FP16 value, so that a
// radiance derived from it and the packed solid angle give back the same illuminance.
inline float ComputeRTXPTDirectionalLightSolidAngle(float AngularSizeRadians)
{
    const float SinQuarterAngle = std::sin(AngularSizeRadians * 0.25f);
    const float SolidAngle      = std::max(4.0f * PI_F * SinQuarterAngle * SinQuarterAngle, kRTXPTDirectionalLightMinSolidAngle);
    return UnpackLightFloat16(PackLightFloat16(SolidAngle));
}

// The angular radius of a directional light's disk from its solid angle, 2 * asin(sqrt(SolidAngle / (4 * PI))): the
// inverse of ComputeRTXPTDirectionalLightSolidAngle without the cancellation of acos(1 - SolidAngle / (2 * PI)).
inline float ComputeRTXPTDirectionalLightAngularRadius(float SolidAngle)
{
    return 2.0f * std::asin(std::sqrt(std::clamp(SolidAngle / (4.0f * PI_F), 0.0f, 1.0f)));
}

// Redistribute a directional light's radiance over a new disk without changing its illuminance or direction.
// The diameter must be finite and in [0, PI]. Invalid input or unrepresentable radiance leaves Light unchanged.
inline bool SetRTXPTDirectionalLightAngularSize(AnalyticLightData& Light, float AngularSizeRadians)
{
    constexpr size_t ColorChannels    = 3;
    const bool       bDirectional     = Light.colorType.w == kPolymorphicLightTypeDirectional;
    const bool       bValidAngle      = std::isfinite(AngularSizeRadians) && AngularSizeRadians >= 0 && AngularSizeRadians <= PI_F;
    const bool       bValidSolidAngle = std::isfinite(Light.shaping.w) && Light.shaping.w > 0;
    if (!bDirectional || !bValidAngle || !bValidSolidAngle)
        return false;

    const float       SolidAngle  = ComputeRTXPTDirectionalLightSolidAngle(AngularSizeRadians);
    const double      MaxRadiance = std::exp2(kPolymorphicLightMaxLog2Radiance);
    AnalyticLightData Updated     = Light;
    for (size_t Channel = 0; Channel < ColorChannels; ++Channel)
    {
        const double Radiance = static_cast<double>(Light.colorType[Channel]) * Light.shaping.w / SolidAngle;
        if (!std::isfinite(Radiance) || Radiance < 0 || Radiance > MaxRadiance)
            return false;
        Updated.colorType[Channel] = static_cast<float>(Radiance);
    }
    Updated.shaping.w = SolidAngle;
    Light             = Updated;
    return true;
}

inline Uint32 PackLightDirection(float3 Direction)
{
    const float Denominator = std::abs(Direction.x) + std::abs(Direction.y) + std::abs(Direction.z);
    Direction               = Denominator > 0.0f ? Direction / Denominator : float3{0, 0, 1};
    float2 P{Direction.x, Direction.y};
    if (Direction.z < 0.0f)
        P = float2{(1.0f - std::abs(Direction.y)) * (Direction.x >= 0.0f ? 1.0f : -1.0f),
                   (1.0f - std::abs(Direction.x)) * (Direction.y >= 0.0f ? 1.0f : -1.0f)};
    P = P * 0.5f + float2{0.5f, 0.5f};
    P = P * 0.5f + float2{0.5f, 0.5f};
    return Uint32(std::clamp(P.x, 0.0f, 1.0f) * 65534.0f) |
        (Uint32(std::clamp(P.y, 0.0f, 1.0f) * 65534.0f) << 16u);
}

// The inverse of PackLightDirection, as UnpackLightDirection in Lighting/LightPacking.hlsli.
inline float3 UnpackLightDirection(Uint32 Packed)
{
    float2 P{std::clamp(float(Packed & 0xffffu) / 65534.0f, 0.0f, 1.0f), std::clamp(float(Packed >> 16u) / 65534.0f, 0.0f, 1.0f)};
    P = P * 2.0f - float2{1.0f, 1.0f};
    P = P * 2.0f - float2{1.0f, 1.0f};
    float3      N{P.x, P.y, 1.0f - std::abs(P.x) - std::abs(P.y)};
    const float T = std::clamp(-N.z, 0.0f, 1.0f);
    N.x += N.x >= 0.0f ? -T : T;
    N.y += N.y >= 0.0f ? -T : T;
    return normalize(N);
}

// LogRadiance keeps the packed log2 radiance in its low 16 bits; 0 marks an unlit light.
constexpr Uint32 kLightLogRadianceMask  = 0xffffu;
constexpr float  kLightLogRadianceSteps = 65534.0f;

inline Uint32 GetPolymorphicLightType(const PolymorphicLightInfo& Light)
{
    return (Light.ColorTypeAndFlags >> kPolymorphicLightTypeShift) & kPolymorphicLightTypeMask;
}

// The radiance scale of a nonzero packed LogRadiance, the largest channel radiance the light can carry.
inline float UnpackLightRadianceScale(Uint32 Packed)
{
    return std::exp2(float(Packed - 1u) / kLightLogRadianceSteps *
                         (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance) +
                     kPolymorphicLightMinLog2Radiance);
}

// The illuminance a packed directional light gives a surface facing it in its largest channel: its largest channel
// radiance times the solid angle of its disk.
inline float ComputeRTXPTDirectionalLightIlluminance(const PolymorphicLightInfo& Light)
{
    constexpr float MaxChannel  = 255.0f;
    const Uint32    Packed      = Light.LogRadiance & kLightLogRadianceMask;
    const Uint32    LargestByte = std::max({Light.ColorTypeAndFlags & 255u, (Light.ColorTypeAndFlags >> 8u) & 255u, (Light.ColorTypeAndFlags >> 16u) & 255u});
    if (Packed == 0u)
        return 0.0f;
    return UnpackLightRadianceScale(Packed) * (float(LargestByte) / MaxChannel) * UnpackLightFloat16(Light.Scalars >> 16u);
}

inline void PackLightColor(const float3& Color, PolymorphicLightInfo& Light)
{
    const float Intensity = std::max({Color.x, Color.y, Color.z});
    if (Intensity <= 0.0f) return;
    const float  Log     = std::clamp((std::log2(Intensity) - kPolymorphicLightMinLog2Radiance) /
                                     (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance),
                                 0.0f, 1.0f);
    const Uint32 Packed  = std::min(Uint32(std::ceil(Log * kLightLogRadianceSteps)) + 1u, kLightLogRadianceMask);
    const float  Scale   = UnpackLightRadianceScale(Packed);
    const auto   Channel = [Scale](float Value) {
        return Uint32(std::floor(std::clamp(Value / Scale, 0.0f, 1.0f) * 255.0f + 0.5f));
    };
    Light.LogRadiance |= Packed;
    Light.ColorTypeAndFlags |= Channel(Color.x) | (Channel(Color.y) << 8u) | (Channel(Color.z) << 16u);
}

inline PolymorphicLightInfoFull PackAnalyticLight(const AnalyticLightData& Source)
{
    // Upstream's packed ABI has no finite range cutoff; directionRange.w is
    // authoring metadata only. Sampling follows upstream's unbounded lights.
    PolymorphicLightInfoFull Light = {};
    if (Source.colorType.w < 0.0f) return Light;
    const Uint32 Type            = Uint32(Source.colorType.w);
    Light.Base.Center            = float3{Source.positionRadius.x, Source.positionRadius.y, Source.positionRadius.z};
    Light.Base.ColorTypeAndFlags = Type << kPolymorphicLightTypeShift;
    PackLightColor(float3{Source.colorType.x, Source.colorType.y, Source.colorType.z}, Light.Base);
    if (Type == static_cast<Uint32>(kPolymorphicLightTypeDirectional))
    {
        Light.Base.Direction1 = PackLightDirection(float3{Source.directionRange.x, Source.directionRange.y, Source.directionRange.z});
        const float HalfAngle = ComputeRTXPTDirectionalLightAngularRadius(Source.shaping.w);
        Light.Base.Scalars    = PackLightFloat16(HalfAngle) | (PackLightFloat16(Source.shaping.w) << 16u);
    }
    else
    {
        Light.Base.Scalars = PackLightFloat16(Source.positionRadius.w);
        if (Source.shaping.x >= -0.5f)
        {
            Light.Base.ColorTypeAndFlags |= kPolymorphicLightShapingEnableBit;
            Light.Extended.PrimaryAxis             = PackLightDirection(float3{Source.directionRange.x, Source.directionRange.y, Source.directionRange.z});
            Light.Extended.CosConeAngleAndSoftness = PackLightFloat16(Source.shaping.x) | (PackLightFloat16(Source.shaping.y) << 16u);
        }
    }
    return Light;
}

} // namespace Diligent
