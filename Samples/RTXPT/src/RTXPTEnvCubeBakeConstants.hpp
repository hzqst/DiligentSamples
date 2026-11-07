// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <array>
#include <cstddef>
#include "BasicMath.hpp"
#include "RTXPTLightPacking.hpp"
#include "../shaders/PathTracer/Lighting/EnvironmentLightConfig.h"

namespace Diligent
{

// Upstream EnvMapBaker stores the environment cube at 1/4 of its radiance and evaluates it with the reciprocal.
constexpr float kEnvironmentRadianceScale = 0.25f;

// cbEnvCubeBake of EnvMapImportanceBaker.hlsl's EnvCubeBakeCS.
struct EnvCubeBakeConstantsCPU
{
    float4x4                                                              Rotation;
    float4                                                                WorldToLocal0, WorldToLocal1, WorldToLocal2;
    float4                                                                SourceColor;
    float                                                                 RadianceScale         = kEnvironmentRadianceScale;
    Uint32                                                                DirectionalLightCount = 0;
    Uint32                                                                CubeDim               = 0;
    Uint32                                                                Padding               = 0;
    std::array<PolymorphicLightInfo, RTXPT_ENVMAP_MAX_DIRECTIONAL_LIGHTS> DirectionalLights{};
};
static_assert(sizeof(EnvCubeBakeConstantsCPU) == 656, "EnvCubeBakeConstantsCPU must match cbEnvCubeBake");
static_assert(offsetof(EnvCubeBakeConstantsCPU, DirectionalLights) == 144, "Directional-light array offset");

} // namespace Diligent
