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

inline Uint32 PackLightFloat16(float Value)
{
    // Match the host conversion in RTXPT-fork LightsBaker.cpp (truncation).
    const float Biased = Value * std::ldexp(1.0f, -112);
    Uint32      Bits   = 0;
    std::memcpy(&Bits, &Biased, sizeof(Bits));
    return (((Bits & 0x80000000u) >> 16u) | ((Bits & 0x0fffffffu) >> 13u)) & 0xffffu;
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

inline void PackLightColor(const float3& Color, PolymorphicLightInfo& Light)
{
    const float Intensity = std::max({Color.x, Color.y, Color.z});
    if (Intensity <= 0.0f) return;
    const float  Log     = std::clamp((std::log2(Intensity) - kPolymorphicLightMinLog2Radiance) /
                                     (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance),
                                 0.0f, 1.0f);
    const Uint32 Packed  = std::min(Uint32(std::ceil(Log * 65534.0f)) + 1u, 65535u);
    const float  Scale   = std::exp2(float(Packed - 1u) / 65534.0f *
                                      (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance) +
                                  kPolymorphicLightMinLog2Radiance);
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
    if (Type == 2u)
    {
        Light.Base.Direction1 = PackLightDirection(float3{Source.directionRange.x, Source.directionRange.y, Source.directionRange.z});
        const float HalfAngle = std::acos(std::clamp(1.0f - Source.shaping.w / (2.0f * PI_F), -1.0f, 1.0f));
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
