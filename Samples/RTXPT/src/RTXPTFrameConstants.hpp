/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#pragma once

#include <cstddef>

#include "BasicMath.hpp"

namespace Diligent
{

struct PathTracerCameraData
{
    float3 PosW                 = float3{0, 0, 0};
    float  NearZ                = 0.0f;
    float3 DirectionW           = float3{0, 0, 1};
    float  PixelConeSpreadAngle = 0.0f;
    float3 CameraU              = float3{1, 0, 0};
    float  FarZ                 = 0.0f;
    float3 CameraV              = float3{0, 1, 0};
    float  FocalDistance        = 10000.0f;
    float3 CameraW              = float3{0, 0, 10000.0f};
    float  AspectRatio          = 1.0f;
    Uint32 ViewportWidth        = 1;
    Uint32 ViewportHeight       = 1;
    float  ApertureRadius       = 0.0f;
    float  _padding0            = 0.0f;
    float2 Jitter               = float2{0, 0};
    float  _padding1            = 0.0f;
    float  _padding2            = 0.0f;
};
static_assert(sizeof(PathTracerCameraData) == 112, "PathTracerCameraData layout must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerCameraData, DirectionW) == 16, "PathTracerCameraData DirectionW offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerCameraData, CameraW) == 64, "PathTracerCameraData CameraW offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerCameraData, ViewportWidth) == 80, "PathTracerCameraData ViewportWidth offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerCameraData, Jitter) == 96, "PathTracerCameraData Jitter offset must match PathTracer/PathTracerShared.h");

struct PathTracerViewData
{
    float4x4 MatWorldToView         = float4x4::Identity();
    float4x4 MatViewToClip          = float4x4::Identity();
    float4x4 MatWorldToClip         = float4x4::Identity();
    float4x4 MatWorldToClipNoOffset = float4x4::Identity();
    float4x4 MatClipToWorldNoOffset = float4x4::Identity();
    float2   ViewportOrigin         = float2{0, 0};
    float2   ViewportSize           = float2{1, 1};
    float2   ViewportSizeInv        = float2{1, 1};
    float2   PixelOffset            = float2{0, 0};
    float2   ClipToWindowScale      = float2{0.5f, -0.5f};
    float2   ClipToWindowBias       = float2{0.5f, 0.5f};
};
static_assert(sizeof(PathTracerViewData) == 368, "PathTracerViewData layout must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerViewData, MatClipToWorldNoOffset) == 256, "PathTracerViewData MatClipToWorldNoOffset offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerViewData, ViewportOrigin) == 320, "PathTracerViewData ViewportOrigin offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerViewData, ViewportSizeInv) == 336, "PathTracerViewData ViewportSizeInv offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerViewData, ClipToWindowBias) == 360, "PathTracerViewData ClipToWindowBias offset must match PathTracer/PathTracerShared.h");

struct PathTracerConstants
{
    Uint32 imageWidth            = 1;
    Uint32 imageHeight           = 1;
    Uint32 sampleBaseIndex       = 0;
    float  perPixelJitterAAScale = 1.0f;

    Uint32 bounceCount                         = 20;
    Uint32 diffuseBounceCount                  = 2;
    float  EnvironmentMapDiffuseSampleMIPLevel = 0.0f;
    float  texLODBias                          = 0.0f;

    float  invSubSampleCount       = 1.0f;
    float  fireflyFilterThreshold  = 0.0f;
    float  preExposedGrayLuminance = 1.0f;
    Uint32 denoisingEnabled        = 0;

    Uint32 frameIndex        = 0;
    Uint32 useReSTIRDI       = 0;
    Uint32 useReSTIRGI       = 0;
    Uint32 resetAccumulation = 1;

    float  stablePlanesSplitStopThreshold               = 0.95f;
    float  _padding3                                    = 0.0f;
    Uint32 _padding4                                    = 0;
    float  stablePlanesSuppressPrimaryIndirectSpecularK = 0.0f;

    float  denoiserRadianceClampK              = 0.0f;
    float  DLSSRRBrightnessClampK              = 0.0f; // RR input max-channel clamp; zero disables the user clamp.
    float  stablePlanesAntiAliasingFallthrough = 0.0f;
    Uint32 _activeStablePlaneCount             = 1;

    Uint32 maxStablePlaneVertexDepth      = 0;
    Uint32 allowPrimarySurfaceReplacement = 0;
    Uint32 genericTSLineStride            = 1;
    Uint32 genericTSPlaneStride           = 1;

    Uint32 NEEEnabled          = 1;
    Uint32 NEEType             = 1;
    Uint32 NEECandidateSamples = 5;
    Uint32 NEEFullSamples      = 1;

    Uint32 sampleIndex           = 0; // Diligent reference compatibility; realtime uses sampleBaseIndex.
    Uint32 minBounceCount        = 0;
    Uint32 environmentNEEEnabled = 1;
    float  environmentIntensity  = 1.0f;

    float  lightIntensityScale = 1.0f;
    Uint32 maxNEEBounceCount   = 16;
    Uint32 analyticLightCount  = 0;
    Uint32 NEEMISType          = 0;

    Uint32 nestedDielectricsQuality = 1;
    Uint32 superResolutionActive    = 0;
    Uint32 _paddingR6_1             = 0;
    Uint32 _paddingR6_2             = 0;

    PathTracerCameraData camera     = {};
    PathTracerCameraData prevCamera = {};
};
static_assert(sizeof(PathTracerConstants) == 400, "PathTracerConstants layout must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, sampleBaseIndex) == 8, "PathTracerConstants sampleBaseIndex offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, invSubSampleCount) == 32, "PathTracerConstants invSubSampleCount offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, frameIndex) == 48, "PathTracerConstants frameIndex offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, resetAccumulation) == 60, "PathTracerConstants resetAccumulation offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, _activeStablePlaneCount) == 92, "PathTracerConstants active stable-plane count offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, genericTSLineStride) == 104, "PathTracerConstants genericTSLineStride offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, sampleIndex) == 128, "PathTracerConstants sampleIndex offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, environmentIntensity) == 140, "PathTracerConstants environmentIntensity offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, nestedDielectricsQuality) == 160, "PathTracerConstants nestedDielectricsQuality offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, camera) == 176, "PathTracerConstants camera offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(PathTracerConstants, prevCamera) == 288, "PathTracerConstants prevCamera offset must match PathTracer/PathTracerShared.h");

struct RTXPTEnvMapConstants
{
    float4 LocalToWorld0      = float4{1, 0, 0, 0};
    float4 LocalToWorld1      = float4{0, 1, 0, 0};
    float4 LocalToWorld2      = float4{0, 0, 1, 0};
    float4 WorldToLocal0      = float4{1, 0, 0, 0};
    float4 WorldToLocal1      = float4{0, 1, 0, 0};
    float4 WorldToLocal2      = float4{0, 0, 1, 0};
    float4 ColorEnabled       = float4{1, 1, 1, 1}; // rgb = tint * intensity, w = enabled.
    float4 ImportanceMetadata = float4{1, 1, 0, 0}; // xy = inv dim, z = base mip, w = importance enabled.
};
static_assert(sizeof(RTXPTEnvMapConstants) == 128, "RTXPTEnvMapConstants layout must match PathTracer/PathTracerShared.h");
static_assert(offsetof(RTXPTEnvMapConstants, WorldToLocal0) == 48, "RTXPTEnvMapConstants WorldToLocal0 offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(RTXPTEnvMapConstants, ColorEnabled) == 96, "RTXPTEnvMapConstants ColorEnabled offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(RTXPTEnvMapConstants, ImportanceMetadata) == 112, "RTXPTEnvMapConstants ImportanceMetadata offset must match PathTracer/PathTracerShared.h");

struct SampleConstants
{
    float4x4             viewProj                  = float4x4::Identity();
    float4x4             viewProjInv               = float4x4::Identity();
    float4               cameraPositionAndTime     = float4{0, 0, 0, 0};
    float4               viewportSizeAndFrameIndex = float4{0, 0, 0, 0};
    PathTracerViewData   view                      = {};
    PathTracerViewData   previousView              = {};
    PathTracerCameraData camera                    = {};
    PathTracerConstants  ptConsts                  = {};
    RTXPTEnvMapConstants envMap                    = {};
};
static_assert(sizeof(SampleConstants) == 1536, "SampleConstants layout must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SampleConstants, cameraPositionAndTime) == 128, "SampleConstants cameraPositionAndTime offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SampleConstants, viewportSizeAndFrameIndex) == 144, "SampleConstants viewportSizeAndFrameIndex offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SampleConstants, view) == 160, "SampleConstants view offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SampleConstants, previousView) == 528, "SampleConstants previousView offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SampleConstants, camera) == 896, "SampleConstants camera offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SampleConstants, ptConsts) == 1008, "SampleConstants ptConsts offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SampleConstants, envMap) == 1408, "SampleConstants envMap offset must match PathTracer/PathTracerShared.h");

struct SampleMiniConstants
{
    uint4 params  = {};
    uint4 params1 = {};
    uint4 params2 = {};
    uint4 params3 = {};
};
static_assert(sizeof(SampleMiniConstants) == 64, "SampleMiniConstants layout must match PathTracer/PathTracerShared.h");

} // namespace Diligent
