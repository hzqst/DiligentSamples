// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
// Standalone GPU regression test. See README.md for build/run instructions.
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "../../../../DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "../../../../DiligentCore/Common/interface/Float16.hpp"
#include "../src/RTXPTFrameConstants.hpp"
#include "../src/RTXPTRenderTargets.hpp"

using namespace Diligent;

namespace
{
std::atomic<Uint32> ValidationErrors{0};

void DILIGENT_CALL_TYPE TestMessageCallback(DEBUG_MESSAGE_SEVERITY Severity, const Char* Message, const Char*, const Char*, int)
{
    if (Severity >= DEBUG_MESSAGE_SEVERITY_ERROR)
        ++ValidationErrors;
    if (Severity >= DEBUG_MESSAGE_SEVERITY_WARNING)
        std::cerr << (Severity >= DEBUG_MESSAGE_SEVERITY_ERROR ? "ERROR: " : "Warning: ") << Message << '\n';
}

void Require(bool Condition, const char* Message)
{
    if (!Condition)
        throw std::runtime_error{Message};
}

void Near(float Expected, float Actual)
{
    if (std::abs(Expected - Actual) >= 0.003f)
        std::cerr << "Expected " << Expected << ", got " << Actual << '\n';
    Require(std::abs(Expected - Actual) < 0.003f, "Unexpected GPU output");
}

// Frozen M5 input gates: R32F RR depth within 1e-5 relative, RGBA16F unit normals within 2e-3 of length 1.
constexpr float kRRDepthRelativeTolerance = 1e-5f;
constexpr float kRRNormalLengthTolerance  = 2e-3f;

void NearRelative(float Expected, float Actual, float Tolerance, const char* Message)
{
    if (std::abs(Expected - Actual) > Tolerance * std::abs(Expected))
        std::cerr << "Expected " << Expected << ", got " << Actual << '\n';
    Require(std::abs(Expected - Actual) <= Tolerance * std::abs(Expected), Message);
}

Uint32 Pack(float Low, float High)
{
    return Float16{Low}.Raw() | (Uint32{Float16{High}.Raw()} << 16);
}

using Plane = RTXPTDenoisingLayerData;
Plane Surface(float Roughness, float Radiance)
{
    Plane Data                            = {};
    Data.SceneLength                      = 1;
    Data.PackedThpAndMVs                  = uint3{Pack(0, 1)}; // motion / throughput
    Data.VertexIndexAndRoughness          = (1u << 16) | Float16{Roughness}.Raw();
    Data.DenoiserPackedBSDFEstimate       = uint3{Pack(0.5f, 0.25f)}; // specular / diffuse
    Data.PackedNormal                     = 0x80008000;               // oct-encoded +Z
    Data.PackedNoisyRadianceAndSpecAvg[0] = Pack(Radiance, Radiance);
    Data.PackedNoisyRadianceAndSpecAvg[1] = Pack(Radiance, 0);
    return Data;
}

// RG16F outputs are compared with unquantized expectations within one half-precision ULP.
constexpr int   kHalfMantissaBits = 10;
constexpr float kHalfMinNormal    = 6.103515625e-5f;

void NearHalf(float Expected, float Actual)
{
    const float Ulp = std::ldexp(1.0f, std::ilogb(std::max(std::abs(Expected), kHalfMinNormal)) - kHalfMantissaBits);
    if (std::abs(Expected - Actual) > Ulp)
        std::cerr << "Expected " << Expected << ", got " << Actual << '\n';
    Require(std::abs(Expected - Actual) <= Ulp, "Unexpected half-precision GPU output");
}

constexpr float kTestNearZ = 0.1f;

// Cases from kFirstPerspectiveCase use a static scene seen by a 1280x720 render view (motion is in render pixels) with
// a 1 rad horizontal FOV. The camera moved from x = -0.05 to the origin; before the failed-projection case it instead
// stood at z = 3.5 facing -Z. The jitter moves the single test pixel's sample onto the optical axis.
constexpr int    kFirstPerspectiveCase          = 14;
constexpr int    kDirectSurfaceCase             = 14; // layer 0 at vertex 1
constexpr int    kPrimarySurfaceReplacementCase = 15; // issue #288: layer 0 at vertex 2 behind a mirror
constexpr int    kDominantLayerCase             = 16; // dominant layer 1; layer 0 deliberately differs
constexpr int    kBlockedAnchorCase             = 17; // depth stops at an earlier motion-blocking surface
constexpr int    kFailedProjectionCase          = 18; // the specular image is behind the previous camera
constexpr Uint32 kRigWidth                      = 1280;
constexpr Uint32 kRigHeight                     = 720;
constexpr float  kRigHorizontalFovRadians       = 1.0f;
constexpr float  kRigPreviousCameraX            = -0.05f;
constexpr float  kRigReversedCameraZ            = 3.5f;
constexpr float  kRigRoughness                  = 0.1f; // below the 0.25 planar-reflector gate
constexpr float  kRigRadiance                   = 4.0f;

void SetPerspectiveRig(SampleConstants& Frame, bool bRightHanded, bool bPreviousCameraReversed)
{
    const float           AspectRatio = static_cast<float>(kRigWidth) / static_cast<float>(kRigHeight);
    const float           TanHalfFovX = std::tan(kRigHorizontalFovRadians * 0.5f);
    PathTracerCameraData& Camera      = Frame.ptConsts.camera;
    Camera.CameraU                    = float3{TanHalfFovX, 0, 0};
    Camera.CameraV                    = float3{0, TanHalfFovX / AspectRatio, 0};
    Camera.CameraW                    = float3{0, 0, 1};
    Camera.AspectRatio                = AspectRatio;
    Camera.ViewportWidth              = kRigWidth;
    Camera.ViewportHeight             = kRigHeight;
    Camera.Jitter                     = float2{0.5f - kRigWidth * 0.5f, kRigHeight * 0.5f - 0.5f};

    // float4x4::Projection takes the vertical FOV. The motion helper reads MatWorldToClipNoOffset, so every clip
    // matrix is the product of the view and projection the depth conversion reads.
    const float    VerticalFov    = 2.0f * std::atan(TanHalfFovX / AspectRatio);
    const float4x4 Projection     = float4x4::Projection(VerticalFov, AspectRatio, Camera.NearZ, Camera.FarZ, false);
    const float4x4 FlipZ          = float4x4::Scale(1, 1, -1);
    const float4x4 ViewBasis      = bRightHanded ? FlipZ : float4x4::Identity();
    const float4x4 ViewToClip     = bRightHanded ? FlipZ * Projection : Projection;
    const float4x4 PreviousCamera = bPreviousCameraReversed ?
        float4x4::Translation(0, 0, -kRigReversedCameraZ) * float4x4::Scale(-1, 1, -1) :
        float4x4::Translation(-kRigPreviousCameraX, 0, 0);

    const auto SetView = [&](PathTracerViewData& View, const float4x4& WorldToView) {
        View.MatWorldToView         = WorldToView;
        View.MatViewToClip          = ViewToClip;
        View.MatWorldToClip         = WorldToView * ViewToClip;
        View.MatWorldToClipNoOffset = View.MatWorldToClip;
        View.ViewportSize           = float2{static_cast<float>(kRigWidth), static_cast<float>(kRigHeight)};
        View.ClipToWindowScale      = float2{kRigWidth * 0.5f, -(kRigHeight * 0.5f)};
    };
    SetView(Frame.view, ViewBasis);
    SetView(Frame.previousView, PreviousCamera * ViewBasis);
}

// Surface motion as BUILD exports it, for a point that projects in both frames.
float2 SurfaceMotion(const SampleConstants& Frame, const float3& Pos, const float3& PreviousPos)
{
    const float4 Clip         = float4{Pos, 1} * Frame.view.MatWorldToClipNoOffset;
    const float4 PreviousClip = float4{PreviousPos, 1} * Frame.previousView.MatWorldToClipNoOffset;
    Require(Clip.w > 0 && PreviousClip.w > 0, "Perspective rig surface must project in both frames");
    return float2{(PreviousClip.x / PreviousClip.w - Clip.x / Clip.w) * Frame.view.ClipToWindowScale.x,
                  (PreviousClip.y / PreviousClip.w - Clip.y / Clip.w) * Frame.view.ClipToWindowScale.y};
}

// Independent of the rig matrices: the pixel motion of an on-axis point at view depth Z that moved by MotionX.
float AxisMotionX(float Z, float MotionX)
{
    return kRigWidth * 0.5f * (MotionX - kRigPreviousCameraX) / (Z * std::tan(kRigHorizontalFovRadians * 0.5f));
}

// A layer as BUILD stores it: its last physical ray, the length accumulated from the near plane and the motion of
// its virtual point.
struct RigLayer
{
    Uint32 VertexIndex = 0;
    float3 RayOrigin   = {};
    float3 RayDir      = {};
    float  LastRayT    = 0;
    float  SceneLength = 0;
    float3 WorldMotion = {};
};

struct PerspectiveCase
{
    RigLayer Layer0;
    RigLayer Layer1;
    Uint32   DominantIndex  = 0;
    float    ExportedDepth  = 0; // the dominant ray distance, or the earlier blocking surface's
    float    HitT           = 0;
    bool     bSpecularImage = false; // otherwise the surface motion must be kept
};

PerspectiveCase GetPerspectiveCase(int Test)
{
    // Issue #288: the primary ray meets a mirror at z = 2 and reflects towards a surface at (1, 0, 1).
    const float    Sqrt2          = std::sqrt(2.0f);
    const float3   MirrorPoint    = float3{0, 0, 2};
    const float3   ReflectedDir   = float3{1, 0, -1} / Sqrt2;
    const float    MirrorDistance = MirrorPoint.z - kTestNearZ;
    const float3   AxisOrigin     = float3{0, 0, kTestNearZ};
    const float3   AxisDir        = float3{0, 0, 1};
    const float    WallDistance   = 2.9f; // a wall at z = 3
    const RigLayer Mirrored       = {2, MirrorPoint, ReflectedDir, Sqrt2, MirrorDistance + Sqrt2, float3{0, 0, 0}};

    PerspectiveCase Case;
    Case.HitT = 1.0f;
    switch (Test)
    {
        case kDirectSurfaceCase:
            Case.Layer0         = {1, AxisOrigin, AxisDir, WallDistance, WallDistance, float3{0.25f, 0, 0}};
            Case.ExportedDepth  = WallDistance;
            Case.bSpecularImage = true;
            break;

        case kPrimarySurfaceReplacementCase:
            Case.Layer0         = Mirrored;
            Case.ExportedDepth  = Mirrored.SceneLength;
            Case.bSpecularImage = true;
            break;

        case kDominantLayerCase:
            Case.Layer0         = {1, AxisOrigin, AxisDir, 1.4f, 1.4f, float3{1.5f, 0, 0}};
            Case.Layer1         = {2, MirrorPoint, ReflectedDir, 0.5f, MirrorDistance + 0.5f, float3{-0.125f, 0, 0}};
            Case.DominantIndex  = 1;
            Case.ExportedDepth  = Case.Layer1.SceneLength;
            Case.HitT           = 0.75f;
            Case.bSpecularImage = true;
            break;

        case kBlockedAnchorCase:
            // Layer 0 is the blocking surface itself, so its distance matches the exported depth.
            Case.Layer0        = {1, AxisOrigin, AxisDir, MirrorDistance, MirrorDistance, float3{0, 0, 0}};
            Case.Layer1        = Mirrored;
            Case.DominantIndex = 1;
            Case.ExportedDepth = MirrorDistance;
            break;

        default:
            Require(Test == kFailedProjectionCase, "Unknown perspective case");
            Case.Layer0        = {1, AxisOrigin, AxisDir, WallDistance, WallDistance, float3{0.125f, 0, 0}};
            Case.ExportedDepth = WallDistance;
            break;
    }
    return Case;
}

Plane MakeRigLayer(const RigLayer& Layer)
{
    Plane Data                   = Surface(kRigRoughness, kRigRadiance);
    Data.RayOrigin               = Layer.RayOrigin;
    Data.RayDir                  = Layer.RayDir;
    Data.LastRayTCurrent         = Layer.LastRayT;
    Data.SceneLength             = Layer.SceneLength;
    Data.VertexIndexAndRoughness = (Layer.VertexIndex << 16) | Float16{kRigRoughness}.Raw();
    Data.PackedThpAndMVs         = uint3{Pack(Layer.WorldMotion.x, 1), Pack(Layer.WorldMotion.y, 1), Pack(Layer.WorldMotion.z, 1)};
    return Data;
}
} // namespace

#include "RRResourceContractTest.hpp"
#include "GeometryVertexLayoutTest.hpp"

int main(int Argc, char** Argv)
try
{
    Require(Argc == 4, "Usage: RRPrepareInputsTest <D3D12 or Vulkan engine DLL> <RTXPT shaders directory> <RR factory DLL>");
    HMODULE Library = LoadLibraryA(Argv[1]);
    Require(Library != nullptr, "Cannot load D3D12 engine DLL");
    auto                          GetFactory = reinterpret_cast<IEngineFactoryD3D12* (*)()>(GetProcAddress(Library, "GetEngineFactoryD3D12"));
    IEngineFactory*               Factory    = nullptr;
    RefCntAutoPtr<IRenderDevice>  Device;
    RefCntAutoPtr<IDeviceContext> Context;
    if (GetFactory)
    {
        Factory = GetFactory();
        Factory->SetMessageCallback(TestMessageCallback);
        EngineD3D12CreateInfo EngineCI;
        EngineCI.EnableValidation = true;
        GetFactory()->CreateDeviceAndContextsD3D12(EngineCI, &Device, &Context);
    }
    else
    {
        auto GetVkFactory = reinterpret_cast<IEngineFactoryVk* (*)()>(GetProcAddress(Library, "GetEngineFactoryVk"));
        Require(GetVkFactory != nullptr, "Missing engine factory");
        Factory = GetVkFactory();
        Factory->SetMessageCallback(TestMessageCallback);
        EngineVkCreateInfo EngineCI;
        EngineCI.EnableValidation        = true;
        EngineCI.EnableRayReconstruction = true;
        GetVkFactory()->CreateDeviceAndContextsVk(EngineCI, &Device, &Context);
    }
    Require(Device && Context, "Cannot create graphics device");
    CheckRRResourceContract(Device, Context, Argv[3]);

    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    Factory->CreateDefaultShaderSourceStreamFactory(Argv[2], &Source);
    CheckGeometryVertexLayout(Device, Context, Source);
    ShaderMacro      Macros[] = {{"RTXPT_PREPARE_DLSS_RR", "1"}, {"RTXPT_HAS_NRD_HEADERS", "0"}};
    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc                       = {"RR input regression", SHADER_TYPE_COMPUTE, false};
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.FilePath                   = "PathTracer/DenoisingGuidesBaker.hlsl";
    ShaderCI.EntryPoint                 = "PrepareDLSSRRGuides";
    ShaderCI.Macros                     = {Macros, 2};
    ShaderCI.pShaderSourceStreamFactory = Source;
    RefCntAutoPtr<IShader> Shader;
    Device->CreateShader(ShaderCI, &Shader);
    Require(Shader != nullptr, "RR shader compilation failed");
    CheckRRPrepareWrapper(Device, Context, Factory, Argv[2]);
    ComputePipelineStateCreateInfo PSOCI;
    PSOCI.PSODesc.Name                               = "RR input regression";
    PSOCI.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
    PSOCI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    PSOCI.pCS                                        = Shader;
    RefCntAutoPtr<IPipelineState> PSO;
    Device->CreateComputePipelineState(PSOCI, &PSO);
    Require(PSO != nullptr, "RR pipeline creation failed");

    // A planar scene uses the same ray/motion producers as BUILD. Independent
    // expectations below check view Z and pixel-motion direction after prepare.
    ShaderCI.FilePath   = nullptr;
    ShaderCI.EntryPoint = "main";
    ShaderCI.Source     = R"(
#include "PathTracer/PathTracerCamera.hlsli"
cbuffer g_Const { SampleConstants g_Frame; };
#ifndef VK_IMAGE_FORMAT
#define VK_IMAGE_FORMAT(format)
#endif
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> u_Depth;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_Motion;
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    CameraRay ray = ComputePathTracerCameraRay(g_Frame.ptConsts, tid.xy, g_Frame.ptConsts.sampleBaseIndex);
    float distance = (3.0 - ray.origin.z) / ray.dir.z;
    float3 worldPos = ray.origin + ray.dir * distance;
    u_Depth[tid.xy] = distance;
    u_Motion[tid.xy] = float4(ComputePathTracerMotionVector(worldPos, worldPos + float3(0.5, 0, 0),
        g_Frame.view, g_Frame.previousView), 0);
})";
    ShaderCI.Macros     = {};
    Shader.Release();
    Device->CreateShader(ShaderCI, &Shader);
    Require(Shader != nullptr, "RR planar scene shader failed");
    PSOCI.pCS = Shader;
    RefCntAutoPtr<IPipelineState> ScenePSO;
    Device->CreateComputePipelineState(PSOCI, &ScenePSO);
    Require(ScenePSO != nullptr, "RR planar scene pipeline failed");

    // One-pixel inputs deliberately dispatch an 8x8 group to exercise bounds checks.
    // Every case runs twice: with the Sample's left-handed view (forward = +Z) and with the game's right-handed view
    // (forward = -Z), the same world, rays and clip space expressed as View * FlipZ and FlipZ * Projection. RR depth is
    // the forward distance in both, so the expectations do not depend on the convention.
    constexpr int   kInputCaseCount  = 19;
    constexpr float kTestFovRadians  = 1.0f;
    constexpr float kTestAspectRatio = 1.0f;
    for (int Run = 0; Run < 2 * kInputCaseCount; ++Run)
    {
        const int       Test         = Run % kInputCaseCount;
        const bool      bRightHanded = Run >= kInputCaseCount;
        const bool      bPerspective = Test >= kFirstPerspectiveCase;
        const char*     Convention   = bRightHanded ? " (right-handed view)" : " (left-handed view)";
        SampleConstants Frame        = {};
        Frame.ptConsts.imageWidth = Frame.ptConsts.imageHeight = 1;
        Frame.ptConsts._activeDenoisingLayerCount              = Test == 8 ? 0 : 3;
        Frame.ptConsts.genericTSLineStride                     = 8;
        Frame.ptConsts.genericTSPlaneStride                    = 64;
        Frame.ptConsts.DLSSRRBrightnessClampK                  = Test == 3 ? 2.0f : 0.0f;
        Frame.ptConsts.camera.CameraW                          = float3{0, 0, 1};
        Frame.ptConsts.camera.NearZ                            = kTestNearZ;
        Frame.ptConsts.camera.FarZ                             = 100;
        Frame.ptConsts.perPixelJitterAAScale                   = 0;
        if (Test >= 10 && !bPerspective)
        {
            Frame.ptConsts.camera.Jitter = float2{0.25f, -0.125f};
            if (Test >= 11)
                Frame.previousView.MatWorldToClipNoOffset = float4x4::Translation(-2, 4, 0);
            if (Test == 12)
                Frame.previousView.MatWorldToClipNoOffset[3][3] = -1;
            if (Test == 13)
            {
                Frame.ptConsts.perPixelJitterAAScale = 0.5f;
                Frame.ptConsts.camera.ApertureRadius = 0.25f;
                Frame.ptConsts.sampleBaseIndex       = 17;
            }
        }
        const float4x4 Projection = float4x4::Projection(kTestFovRadians, kTestAspectRatio, Frame.ptConsts.camera.NearZ,
                                                         Frame.ptConsts.camera.FarZ, false);
        const float4x4 FlipZ      = float4x4::Scale(1, 1, -1);
        Frame.view.MatWorldToView = bRightHanded ? FlipZ : float4x4::Identity();
        Frame.view.MatViewToClip  = bRightHanded ? FlipZ * Projection : Projection;
        const PerspectiveCase Rig = bPerspective ? GetPerspectiveCase(Test) : PerspectiveCase{};
        if (bPerspective)
            SetPerspectiveRig(Frame, bRightHanded, Test == kFailedProjectionCase);
        BufferDesc CBDesc;
        CBDesc.Name      = "RR test constants";
        CBDesc.Size      = sizeof(Frame);
        CBDesc.BindFlags = BIND_UNIFORM_BUFFER;
        BufferData             CBData{&Frame, sizeof(Frame)};
        RefCntAutoPtr<IBuffer> Constants;
        Device->CreateBuffer(CBDesc, &CBData, &Constants);

        std::array<Plane, 192> Planes = {};
        Planes[0]                     = Surface(Test == 5 ? 0.0f : 0.5f, 4.0f);
        Planes[64]                    = Surface(1.0f, 2.0f);
        if (bPerspective)
        {
            Planes[0]  = MakeRigLayer(Rig.Layer0);
            Planes[64] = MakeRigLayer(Rig.Layer1);
        }
        if (Test == 6)
        {
            Planes[64].PackedThpAndMVs = uint3{Pack(0, 0.625f)};
            Planes[64].PackedNormal    = 0xffffffffu; // -Z, opposite to plane zero
        }
        if (Test == 9)
            Planes[0].SceneLength = std::numeric_limits<float>::infinity(); // sky marker
        if (Test == 4)
        {
            Planes[0].PackedNoisyRadianceAndSpecAvg[0] = Pack(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
            Planes[0].PackedNoisyRadianceAndSpecAvg[1] = Pack(-1.0f, 0);
            Planes[0].VertexIndexAndRoughness          = (1u << 16) | 0x7e00u; // NaN roughness
        }
        BufferDesc SPDesc;
        SPDesc.Name              = "RR test planes";
        SPDesc.Size              = sizeof(Planes);
        SPDesc.Mode              = BUFFER_MODE_STRUCTURED;
        SPDesc.ElementByteStride = sizeof(Plane);
        SPDesc.BindFlags         = BIND_UNORDERED_ACCESS;
        BufferData             SPData{Planes.data(), sizeof(Planes)};
        RefCntAutoPtr<IBuffer> DenoisingLayers;
        Device->CreateBuffer(SPDesc, &SPData, &DenoisingLayers);

        std::array<Uint32, 4> Header = {{Test < 2 ? 0xffffffffu : 1u, Test == 5 ? 1u : 0xffffffffu, 0xffffffffu, 0u}};
        if (Test == 6)
            Header[1] = 1;
        if (Test == 7)
            Header = {{0xffffffffu, 1u, 0xffffffffu, 3u}}; // missing primary and invalid dominant
        if (bPerspective)
            Header = {{1u, Rig.DominantIndex == 1 ? 1u : 0xffffffffu, 0xffffffffu, Rig.DominantIndex}};
        std::array<TextureSubResData, 4> HeaderData;
        for (size_t Layer = 0; Layer < Header.size(); ++Layer)
            HeaderData[Layer] = TextureSubResData{&Header[Layer], sizeof(Uint32)};
        TextureData HeaderInit{HeaderData.data(), 4};
        TextureDesc TexDesc;
        TexDesc.Name  = "RR test header";
        TexDesc.Type  = RESOURCE_DIM_TEX_2D_ARRAY;
        TexDesc.Width = TexDesc.Height = 1;
        TexDesc.ArraySize              = 4;
        TexDesc.Format                 = TEX_FORMAT_R32_UINT;
        TexDesc.BindFlags              = BIND_UNORDERED_ACCESS;
        RefCntAutoPtr<ITexture> HeaderTexture;
        Device->CreateTexture(TexDesc, &HeaderInit, &HeaderTexture);

        std::array<Uint16, 4> Stable = {{Float16{Test == 1 ? 8.0f : 0.0f}.Raw(), 0, 0, 0}};
        TextureSubResData     StableData{Stable.data(), sizeof(Stable)};
        TextureData           StableInit{&StableData, 1};
        TexDesc.Type      = RESOURCE_DIM_TEX_2D;
        TexDesc.ArraySize = 1;
        TexDesc.Format    = TEX_FORMAT_RGBA16_FLOAT;
        TexDesc.Name      = "RR test stable radiance";
        RefCntAutoPtr<ITexture> StableRadiance;
        Device->CreateTexture(TexDesc, &StableInit, &StableRadiance);
        std::array<RefCntAutoPtr<ITexture>, 7> Outputs;
        for (size_t Index = 0; Index < Outputs.size(); ++Index)
        {
            TexDesc.Format = Index < 4 ? TEX_FORMAT_RGBA16_FLOAT : Index == 4 ? TEX_FORMAT_R32_FLOAT : TEX_FORMAT_RG16_FLOAT;
            Device->CreateTexture(TexDesc, nullptr, &Outputs[Index]);
        }
        const float RawDepth   = bPerspective ? Rig.ExportedDepth : Test == 0 ? 0.0f : Test == 1 ? std::numeric_limits<float>::infinity() : 2.0f;
        const float RawHitT    = bPerspective ? Rig.HitT : RawDepth;
        const auto  CreateR32F = [&](const float& Value, RefCntAutoPtr<ITexture>& pTexture) {
            TextureSubResData Data{&Value, sizeof(Value)};
            TextureData       Init{&Data, 1};
            TexDesc.Format    = TEX_FORMAT_R32_FLOAT;
            TexDesc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
            Device->CreateTexture(TexDesc, &Init, &pTexture);
        };
        RefCntAutoPtr<ITexture> Depth;
        RefCntAutoPtr<ITexture> HitT;
        CreateR32F(RawDepth, Depth);
        CreateR32F(RawHitT, HitT);
        // A perspective case exports the dominant layer's virtual motion at the depth's virtual point.
        float2 SurfaceMotionInput = float2{2.0f, -3.0f};
        if (bPerspective)
        {
            const RigLayer& Dominant        = Rig.DominantIndex == 0 ? Rig.Layer0 : Rig.Layer1;
            const float3    VirtualWorldPos = float3{0, 0, kTestNearZ + Rig.ExportedDepth};
            SurfaceMotionInput              = SurfaceMotion(Frame, VirtualWorldPos, VirtualWorldPos + Dominant.WorldMotion);
        }
        const std::array<Uint16, 4> Motion = {{Float16{SurfaceMotionInput.x}.Raw(), Float16{SurfaceMotionInput.y}.Raw(), 0, 0}};
        TextureSubResData           MotionData{Motion.data(), sizeof(Motion)};
        TextureData                 MotionInit{&MotionData, 1};
        TexDesc.Format = TEX_FORMAT_RGBA16_FLOAT;
        RefCntAutoPtr<ITexture> MotionTexture;
        Device->CreateTexture(TexDesc, &MotionInit, &MotionTexture);
        if (Test >= 10 && !bPerspective)
        {
            RefCntAutoPtr<IShaderResourceBinding> SceneSRB;
            ScenePSO->CreateShaderResourceBinding(&SceneSRB, true);
            SceneSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "g_Const")->Set(Constants);
            SceneSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Depth")->Set(Depth->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            SceneSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Motion")->Set(MotionTexture->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            Context->SetPipelineState(ScenePSO);
            Context->CommitShaderResources(SceneSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});
        }

        RefCntAutoPtr<IShaderResourceBinding> SRB;
        PSO->CreateShaderResourceBinding(&SRB, true);
        const auto Bind = [&](const char* Name, IDeviceObject* Object) {
            Require(Object != nullptr, "Missing test resource");
            auto* Variable = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name);
            Require(Variable != nullptr, "Missing shader resource");
            Variable->Set(Object);
        };
        Bind("g_Const", Constants);
        Bind("u_DenoisingLayersBuffer", DenoisingLayers->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Bind("u_DenoisingLayersHeader", HeaderTexture->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_StableRadiance", StableRadiance->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("t_Depth", Depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        Bind("t_SpecularHitT", HitT->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        Bind("t_MotionVectors", MotionTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        const char* Names[] = {"u_OutputColor", "u_RRDiffuseAlbedo", "u_RRSpecAlbedo", "u_RRNormalsAndRoughness", "u_RRDepth", "u_RRMotionVectors", "u_RRSpecMotionVectors"};
        for (size_t Index = 0; Index < Outputs.size(); ++Index)
            Bind(Names[Index], Outputs[Index]->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Context->SetPipelineState(PSO);
        Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});

        std::array<std::array<float, 4>, 7> Results = {};
        for (size_t Index = 0; Index < Outputs.size(); ++Index)
        {
            TextureDesc ReadDesc    = Outputs[Index]->GetDesc();
            ReadDesc.BindFlags      = BIND_NONE;
            ReadDesc.Usage          = USAGE_STAGING;
            ReadDesc.CPUAccessFlags = CPU_ACCESS_READ;
            RefCntAutoPtr<ITexture> Readback;
            Device->CreateTexture(ReadDesc, nullptr, &Readback);
            CopyTextureAttribs Copy;
            Copy.pSrcTexture              = Outputs[Index];
            Copy.pDstTexture              = Readback;
            Copy.SrcTextureTransitionMode = Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            Context->CopyTexture(Copy);
            Context->WaitForIdle();
            MappedTextureSubresource Mapped;
            Context->MapTextureSubresource(Readback, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Mapped);
            Require(Mapped.pData != nullptr, "Readback failed");
            for (size_t Channel = 0; Channel < (Index < 4 ? 4u : Index == 4 ? 1u : 2u); ++Channel)
            {
                float Value = Index == 4 ? *static_cast<const float*>(Mapped.pData) : Float16::HalfBitsToFloat(static_cast<const Uint16*>(Mapped.pData)[Channel]);
                Require(std::isfinite(Value), "Non-finite RR output");
                Results[Index][Channel] = Value;
            }
            Context->UnmapTextureSubresource(Readback, 0, 0);
        }
        const auto& Normal = Results[3];
        std::cout << "RR motion case " << Test << Convention << ": " << Results[5][0] << ", " << Results[5][1]
                  << "; specular " << Results[6][0] << ", " << Results[6][1] << '\n';
        const float ExpectedDepth = bPerspective ? kTestNearZ + Rig.ExportedDepth : Test < 2 ? 5000000.0f : Test < 10 ? 2.1f : 3.0f;
        NearRelative(ExpectedDepth, Results[4][0], kRRDepthRelativeTolerance, "RR depth is not the forward view distance");
        if (bPerspective)
        {
            NearHalf(SurfaceMotionInput.x, Results[5][0]);
            NearHalf(SurfaceMotionInput.y, Results[5][1]);
            if (Rig.bSpecularImage)
            {
                // The specular image lies hitT beyond the depth's virtual point on the optical axis.
                const RigLayer& Dominant   = Rig.DominantIndex == 0 ? Rig.Layer0 : Rig.Layer1;
                const float     ImageDepth = kTestNearZ + Rig.ExportedDepth + Rig.HitT;
                NearHalf(AxisMotionX(ImageDepth, Dominant.WorldMotion.x), Results[6][0]);
                Near(0, Results[6][1]);
            }
            else
            {
                Require(Results[6][0] == Results[5][0] && Results[6][1] == Results[5][1], "Specular motion must keep the surface motion");
            }
        }
        else
        {
            Near(Test < 10 ? 2.0f : Test == 10 ? 0.25f : Test == 12 ? 0.0f : -0.75f, Results[5][0]);
            Near(Test < 10 ? -3.0f : Test == 10 || Test == 12 ? 0.0f : -2.0f, Results[5][1]);
            Near(Results[5][0], Results[6][0]);
            Near(Results[5][1], Results[6][1]);
        }
        Require(std::abs(1 - std::sqrt(Normal[0] * Normal[0] + Normal[1] * Normal[1] + Normal[2] * Normal[2])) <= kRRNormalLengthTolerance,
                "RR normal is not unit length");
        Require(Normal[3] >= 0 && Normal[3] <= 1, "Roughness outside [0,1]");
        for (size_t Guide = 1; Guide <= 2; ++Guide)
            for (float Value : Results[Guide])
                Require(Value >= 0 && Value <= 1, "Albedo outside [0,1]");
        if (Test == 0)
        {
            Near(0, Results[0][0]);
            Require(std::abs(0.0001f - Results[1][0]) < 1e-6f, "RR black-albedo floor must remain nonzero and distinct from NRD's floor");
        }
        if (Test == 1) Near(8, Results[0][0]); // disabled clamp must preserve sky
        if (Test == 2)
        {
            Near(4, Results[0][0]);
            Near(0.5f, Normal[3]);
            Near(0.25f, Results[1][0]);
            Near(0.5f, Results[2][0]);
        }
        if (Test == 3) Near(2, Results[0][0]);
        if (Test == 4)
        {
            Near(0, Results[0][0]);
            Near(65504, Results[0][1]);
            Near(0, Results[0][2]);
            Near(0, Normal[3]);
        }
        if (Test == 5)
        {
            Near(6, Results[0][0]);
            Near(0.21f / 0.27f, Normal[3]);
        }
        if (Test == 6)
        {
            Near(6, Results[0][0]);
            Near(0.75f, Normal[3]);
        }
        if (Test == 7)
        {
            Near(2, Results[0][0]);
            Near(1, Normal[3]);
        }
        if (Test == 8 || Test == 9) Near(0, Results[0][0]);
        std::cout << "RR input case " << Test << Convention << " passed\n";
        Context->Flush();
        Context->FinishFrame();
    }
    Context->WaitForIdle();
    Require(ValidationErrors == 0, "Graphics validation reported errors");
    return 0;
}
catch (const std::exception& Error)
{
    std::cerr << Error.what() << '\n';
    return 1;
}
