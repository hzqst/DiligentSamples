/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <cstddef>

#include "BasicTypes.h"
#include "BasicMath.hpp"

namespace Diligent
{

// One entry per TLAS geometry; the shaders index it with InstanceID() + GeometryIndex().
struct SubInstanceData
{
    Uint32 MaterialID             = 0;
    Uint32 Flags                  = 0;
    Uint32 IndexOffset            = 0;
    Uint32 IndexCount             = 0;
    Uint32 VertexOffset           = 0;
    Uint32 VertexCount            = 0;
    Uint32 EmissiveTriangleOffset = 0;
    Uint32 _padding1              = 0;
    // Current triangle indices/local positions are also the previous rigid surface when the validity flag is set.
    float4x4 PreviousTransform = float4x4::Identity();
};
static_assert(sizeof(SubInstanceData) == 96, "SubInstanceData layout must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SubInstanceData, PreviousTransform) == 32, "PreviousTransform offset must match the shader");
static_assert(offsetof(SubInstanceData, IndexOffset) == 8, "SubInstanceData IndexOffset offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SubInstanceData, VertexOffset) == 16, "SubInstanceData VertexOffset offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SubInstanceData, EmissiveTriangleOffset) == 24, "SubInstanceData EmissiveTriangleOffset offset must match PathTracer/PathTracerShared.h");

// Flag bits for SubInstanceData::Flags.
constexpr Uint32 kSubInstanceFlag_Indexed              = 0x1u;
constexpr Uint32 kSubInstanceFlag_Skinned              = 0x2u;
constexpr Uint32 kSubInstanceFlag_PreviousRigidSurface = 0x4u;

} // namespace Diligent
