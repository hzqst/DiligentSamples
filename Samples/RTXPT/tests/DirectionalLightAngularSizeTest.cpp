// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#include "../src/RTXPTLightPacking.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

using namespace Diligent;

static void Require(bool Condition, const char* Message)
{
    if (!Condition)
    {
        std::cerr << "FAIL " << Message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

int main()
{
    constexpr float   DegreesToRadians = PI_F / 180.0f;
    constexpr float   OriginalDiameter = 0.53f * DegreesToRadians;
    constexpr float   Tolerance        = 1.0e-5f;
    constexpr size_t  ColorChannels    = 3;
    const float3      Illuminance{14.0f, 7.0f, 3.5f};
    const float       OriginalSolidAngle = ComputeRTXPTDirectionalLightSolidAngle(OriginalDiameter);
    AnalyticLightData Original;
    Original.colorType      = float4{Illuminance.x / OriginalSolidAngle, Illuminance.y / OriginalSolidAngle, Illuminance.z / OriginalSolidAngle, kPolymorphicLightTypeDirectional};
    Original.shaping.w      = OriginalSolidAngle;
    Original.directionRange = float4{0, 0, -1, 0};

    // Up to about 0.03 degrees, 2 * PI * (1 - cos(half angle)) cancels in float, and the old 1e-8 floor packed as 0.
    constexpr float DiametersDegrees[] = {0.0f, 0.01f, 0.02f, 0.03f, 0.53f, 2.0f, 5.0f, 180.0f};
    constexpr float Fp16MantissaStep   = 0x1p-10f;
    constexpr float RadiusTolerance    = 1.0e-5f;
    constexpr float PackedTolerance    = 0.01f;
    float           PreviousSolidAngle = 0;
    const float     LargestIlluminance = std::max({Illuminance.x, Illuminance.y, Illuminance.z});
    for (float Degrees : DiametersDegrees)
    {
        auto Light = Original;
        Require(SetRTXPTDirectionalLightAngularSize(Light, Degrees * DegreesToRadians), "supported angular diameter");
        Require(Light.shaping.w >= PreviousSolidAngle && Light.shaping.w > 0, "disk solid angle grows with diameter, including the hard-shadow floor");
        Require(Light.directionRange == Original.directionRange && Light.colorType.w == Original.colorType.w, "direction and type stay unchanged");
        for (size_t Channel = 0; Channel < ColorChannels; ++Channel)
            Require(std::abs(Illuminance[Channel] - Light.colorType[Channel] * Light.shaping.w) <= Illuminance[Channel] * Tolerance, "illuminance and color are preserved");
        const auto Once = Light;
        Require(SetRTXPTDirectionalLightAngularSize(Light, Degrees * DegreesToRadians) && std::memcmp(&Once, &Light, sizeof(Light)) == 0, "reapplying an angle is stable");
        PreviousSolidAngle = Light.shaping.w;

        const double QuarterAngle = static_cast<double>(Degrees) * DegreesToRadians * 0.25;
        const double ExactAngle   = 4.0 * PI * std::sin(QuarterAngle) * std::sin(QuarterAngle);
        const double Fp16Step     = std::max(static_cast<double>(kRTXPTDirectionalLightMinSolidAngle), ExactAngle * Fp16MantissaStep);
        const bool   bFloor       = ExactAngle < kRTXPTDirectionalLightMinSolidAngle;
        Require(bFloor ? Light.shaping.w == kRTXPTDirectionalLightMinSolidAngle : (Light.shaping.w <= ExactAngle && Light.shaping.w > ExactAngle - Fp16Step),
                "solid angle is the disk's, raised to the hard-light floor and truncated to FP16");

        const auto Packed = PackAnalyticLight(Light);
        Require(UnpackLightFloat16(Packed.Base.Scalars >> 16u) == Light.shaping.w, "the packed solid angle is the one the radiance was derived from");
        Require(std::abs(ComputeRTXPTDirectionalLightIlluminance(Packed.Base) - LargestIlluminance) <= LargestIlluminance * PackedTolerance, "the packed light keeps its illuminance");
        const float  Radius      = ComputeRTXPTDirectionalLightAngularRadius(Light.shaping.w);
        const double ExactRadius = 2.0 * std::asin(std::sqrt(Light.shaping.w / (4.0 * PI)));
        Require(Radius > 0 && std::abs(Radius - ExactRadius) <= ExactRadius * RadiusTolerance, "the angular radius inverts the solid angle without cancellation");
    }

    constexpr float InvalidDiameters[] = {-1.0f, PI_F + 1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
    for (float Diameter : InvalidDiameters)
    {
        auto Light = Original;
        Require(!SetRTXPTDirectionalLightAngularSize(Light, Diameter) && std::memcmp(&Original, &Light, sizeof(Light)) == 0, "invalid diameter leaves the light unchanged");
    }
    auto Sphere               = Original;
    Sphere.colorType.w        = kPolymorphicLightTypeSphere;
    const auto OriginalSphere = Sphere;
    Require(!SetRTXPTDirectionalLightAngularSize(Sphere, OriginalDiameter) && std::memcmp(&OriginalSphere, &Sphere, sizeof(Sphere)) == 0, "non-directional light is not modified");

    constexpr float BrightMultiplier = 1.0e6f;
    auto            Bright           = Original;
    for (size_t Channel = 0; Channel < ColorChannels; ++Channel)
        Bright.colorType[Channel] *= BrightMultiplier;
    const auto OriginalBright = Bright;
    Require(!SetRTXPTDirectionalLightAngularSize(Bright, 0) && std::memcmp(&OriginalBright, &Bright, sizeof(Bright)) == 0, "a small disk exceeding packed radiance range leaves the light unchanged");

    // The minimum log-radiance scale is not a minimum channel value: the RGB mantissa still represents dim suns.
    constexpr float  DimIlluminance = 0.01f;
    constexpr float  ChannelMax     = 255.0f;
    constexpr Uint32 ChannelMask    = 0xffu;
    auto             Dim            = Original;
    for (size_t Channel = 0; Channel < ColorChannels; ++Channel)
        Dim.colorType[Channel] = DimIlluminance / OriginalSolidAngle;
    Require(SetRTXPTDirectionalLightAngularSize(Dim, PI_F), "a dim hemispherical sun is representable");
    const auto  Packed    = PackAnalyticLight(Dim);
    const float PackedRed = static_cast<float>(Packed.Base.ColorTypeAndFlags & ChannelMask) / ChannelMax * UnpackLightRadianceScale(Packed.Base.LogRadiance & kLightLogRadianceMask);
    Require(std::abs(PackedRed * Dim.shaping.w - DimIlluminance) <= DimIlluminance * PackedTolerance, "RGB mantissa preserves dim solar energy below the minimum log-radiance scale");
    std::cout << "PASS directional light angular size\n";
    return EXIT_SUCCESS;
}
