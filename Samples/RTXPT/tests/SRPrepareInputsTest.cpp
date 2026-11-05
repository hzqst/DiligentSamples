// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
// Standalone GPU regression test of the realtime super resolution inputs. See README.md for build/run instructions.
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "../../../../DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "../../../../DiligentCore/Common/interface/Float16.hpp"
#include "../../../../DiligentCore/Graphics/GraphicsAccessories/interface/GraphicsAccessories.hpp"
#include "../src/RTXPTShaderSource.hpp"
#include "../src/RTXPTSuperResolutionInputs.hpp"

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

void Require(bool Condition, const std::string& Message)
{
    if (!Condition)
        throw std::runtime_error{Message};
}

// Render size of the GPU cases: odd, smaller than one 8x8 group, and not the display size.
constexpr Uint32 kRenderWidth   = 7;
constexpr Uint32 kRenderHeight  = 5;
constexpr Uint32 kDisplayWidth  = 11;
constexpr Uint32 kDisplayHeight = 8;
constexpr float  kNearZ         = 0.1f;
constexpr float  kFarZ          = 100.0f;
constexpr float  kFovY          = 1.0f;
// R32F depth after the GPU reconstructs the point from its ray distance: the plan's starting threshold.
constexpr float kDepthTolerance = 1e-5f;
// FP16 motion: the tolerance of the existing host motion regression.
constexpr float kMotionTolerance = 0.25f;

// The camera of a display-size view traced at render size: the field of view and aspect follow the display.
void MakeFrame(const float2& Jitter, SampleConstants& Frame)
{
    Frame                     = {};
    const float TanHalfFovY   = std::tan(kFovY * 0.5f);
    const float DisplayAspect = static_cast<float>(kDisplayWidth) / kDisplayHeight;
    auto&       Camera        = Frame.ptConsts.camera;
    Camera.PosW               = float3{0, 0, 0};
    Camera.DirectionW         = float3{0, 0, 1};
    Camera.CameraW            = float3{0, 0, 1};
    Camera.CameraU            = float3{TanHalfFovY * DisplayAspect, 0, 0};
    Camera.CameraV            = float3{0, TanHalfFovY, 0};
    Camera.NearZ              = kNearZ;
    Camera.FarZ               = kFarZ;
    Camera.FocalDistance      = 1;
    Camera.AspectRatio        = DisplayAspect;
    Camera.ViewportWidth      = kRenderWidth;
    Camera.ViewportHeight     = kRenderHeight;

    auto& View                  = Frame.view;
    View.MatViewToClip          = float4x4::Projection(kFovY, DisplayAspect, kNearZ, kFarZ, false);
    View.MatWorldToView         = float4x4::Identity();
    View.MatWorldToClip         = View.MatViewToClip;
    View.MatWorldToClipNoOffset = View.MatViewToClip;
    View.MatClipToWorldNoOffset = View.MatViewToClip.Inverse();
    View.ViewportSize           = float2{float(kRenderWidth), float(kRenderHeight)};
    View.ViewportSizeInv        = float2{1.0f / kRenderWidth, 1.0f / kRenderHeight};
    View.ClipToWindowScale      = float2{kRenderWidth * 0.5f, -(kRenderHeight * 0.5f)};
    View.ClipToWindowBias       = float2{kRenderWidth * 0.5f, kRenderHeight * 0.5f};
    Frame.previousView          = View;

    Frame.ptConsts.imageWidth            = kRenderWidth;
    Frame.ptConsts.imageHeight           = kRenderHeight;
    Frame.ptConsts.perPixelJitterAAScale = 0;
    RTXPTSuperResolutionInputs::ApplyJitter(Jitter, Frame.ptConsts.camera, Frame.view);
}

// Independent CPU oracle: the pinhole ray through the jittered sample position of a pixel.
float3 CameraRayDir(const SampleConstants& Frame, Uint32 X, Uint32 Y)
{
    const auto&  Camera = Frame.ptConsts.camera;
    const float2 Sample = float2{(X + 0.5f - Camera.Jitter.x) / Camera.ViewportWidth, (Y + 0.5f + Camera.Jitter.y) / Camera.ViewportHeight};
    return Camera.CameraU * (2 * Sample.x - 1) + Camera.CameraV * (1 - 2 * Sample.y) + Camera.CameraW;
}

float3 IntersectPlane(const SampleConstants& Frame, const float4& Plane, Uint32 X, Uint32 Y)
{
    const float3 Normal = float3{Plane.x, Plane.y, Plane.z};
    const float3 Dir    = CameraRayDir(Frame, X, Y);
    const float3 Origin = Frame.ptConsts.camera.PosW;
    return Origin + Dir * ((Plane.w - dot(Normal, Origin)) / dot(Normal, Dir));
}

float ProjectDepth(const float4x4& WorldToClip, const float3& Point)
{
    const float4 Clip = float4{Point, 1} * WorldToClip;
    if (!(Clip.w > 0))
        return 1;
    return std::min(std::max(Clip.z / Clip.w, 0.0f), 1.0f);
}

float2 ExpectedMotion(const SampleConstants& Frame, const float3& Current, const float3& Previous)
{
    float4 Clip     = float4{Current, 1} * Frame.view.MatWorldToClipNoOffset;
    float4 PrevClip = float4{Previous, 1} * Frame.previousView.MatWorldToClipNoOffset;
    return (float2{PrevClip.x, PrevClip.y} / PrevClip.w - float2{Clip.x, Clip.y} / Clip.w) * Frame.view.ClipToWindowScale;
}

RefCntAutoPtr<IBuffer> CreateConstants(IRenderDevice* Device, const char* Name, Uint64 Size)
{
    BufferDesc Desc;
    Desc.Name           = Name;
    Desc.Size           = Size;
    Desc.BindFlags      = BIND_UNIFORM_BUFFER;
    Desc.Usage          = USAGE_DYNAMIC;
    Desc.CPUAccessFlags = CPU_ACCESS_WRITE;
    RefCntAutoPtr<IBuffer> Buffer;
    Device->CreateBuffer(Desc, nullptr, &Buffer);
    Require(Buffer != nullptr, std::string{"Cannot create "} + Name);
    return Buffer;
}

void Upload(IDeviceContext* Context, IBuffer* Buffer, const void* Data, size_t Size)
{
    void* Mapped = nullptr;
    Context->MapBuffer(Buffer, MAP_WRITE, MAP_FLAG_DISCARD, Mapped);
    Require(Mapped != nullptr, "Cannot map constants");
    std::memcpy(Mapped, Data, Size);
    Context->UnmapBuffer(Buffer, MAP_WRITE);
}

RefCntAutoPtr<ITexture> CreateTexture(IRenderDevice* Device, const char* Name, Uint32 Width, Uint32 Height, TEXTURE_FORMAT Format, Diligent::BIND_FLAGS Flags)
{
    TextureDesc Desc;
    Desc.Name      = Name;
    Desc.Type      = RESOURCE_DIM_TEX_2D;
    Desc.Width     = Width;
    Desc.Height    = Height;
    Desc.Format    = Format;
    Desc.BindFlags = Flags;
    RefCntAutoPtr<ITexture> Texture;
    Device->CreateTexture(Desc, nullptr, &Texture);
    Require(Texture != nullptr, std::string{"Cannot create "} + Name);
    return Texture;
}

// Tightly packed rows of the whole texture.
std::vector<Uint8> Readback(IRenderDevice* Device, IDeviceContext* Context, ITexture* Texture)
{
    TextureDesc Desc    = Texture->GetDesc();
    Desc.Name           = "SR input readback";
    Desc.BindFlags      = BIND_NONE;
    Desc.Usage          = USAGE_STAGING;
    Desc.CPUAccessFlags = CPU_ACCESS_READ;
    RefCntAutoPtr<ITexture> Staging;
    Device->CreateTexture(Desc, nullptr, &Staging);
    Require(Staging != nullptr, "Cannot create readback texture");
    CopyTextureAttribs Copy;
    Copy.pSrcTexture              = Texture;
    Copy.pDstTexture              = Staging;
    Copy.SrcTextureTransitionMode = Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    Context->CopyTexture(Copy);
    Context->WaitForIdle();
    MappedTextureSubresource Mapped;
    Context->MapTextureSubresource(Staging, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Mapped);
    Require(Mapped.pData != nullptr, "Readback failed");
    const size_t       RowBytes = size_t{Desc.Width} * GetTextureFormatAttribs(Desc.Format).GetElementSize();
    std::vector<Uint8> Data(RowBytes * Desc.Height);
    for (Uint32 Row = 0; Row < Desc.Height; ++Row)
        std::memcpy(&Data[Row * RowBytes], static_cast<const Uint8*>(Mapped.pData) + Row * Mapped.Stride, RowBytes);
    Context->UnmapTextureSubresource(Staging, 0, 0);
    return Data;
}

float HalfAt(const std::vector<Uint8>& Data, size_t Index)
{
    Uint16 Bits = 0;
    std::memcpy(&Bits, &Data[Index * sizeof(Bits)], sizeof(Bits));
    return Float16::HalfBitsToFloat(Bits);
}

float FloatAt(const std::vector<Uint8>& Data, size_t Index)
{
    float Value = 0;
    std::memcpy(&Value, &Data[Index * sizeof(Value)], sizeof(Value));
    return Value;
}
} // namespace

namespace
{
// The synthetic BUILD output of a case: a plane hit by the production camera ray, or a marker the realtime guides use.
enum class SceneMode : Uint32
{
    Plane,
    Sky,
    InfiniteDistance,
    NaNDistance,
    NegativeDistance,
    NaNMotion,
    InfiniteMotion,
};

struct SceneConstants
{
    float4 Plane;
    float4 ObjectDelta;
    // x: SceneMode, y: primary motion w.
    float4 Values;
};

struct GpuCase
{
    const char* Name;
    float2      Jitter;
    float4      Plane;
    float3      ObjectDelta;
    float4x4    PreviousWorldToClip;
    SceneMode   Mode;
    float       HistoryValid;
};

// Writes the ray distance and primary motion BUILD would export for a plane, using the production camera ray and
// motion helpers. The sky marker is a zero distance with the motion of a distant point along the ray.
constexpr char kSceneShader[] = R"(
#include "PathTracer/PathTracerCamera.hlsli"
cbuffer g_Const { SampleConstants g_Frame; };
cbuffer g_Case { float4 g_Plane; float4 g_ObjectDelta; float4 g_Values; };
#ifndef VK_IMAGE_FORMAT
#define VK_IMAGE_FORMAT(format)
#endif
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> u_Depth;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_Motion;
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_Frame.ptConsts.imageWidth || tid.y >= g_Frame.ptConsts.imageHeight)
        return;
    const uint mode = uint(g_Values.x);
    CameraRay ray = ComputePathTracerCameraRay(g_Frame.ptConsts, tid.xy, g_Frame.ptConsts.sampleBaseIndex);
    float distance = (g_Plane.w - dot(g_Plane.xyz, ray.origin)) / dot(g_Plane.xyz, ray.dir);
    float3 worldPos = ray.origin + ray.dir * distance;
    if (mode == 1)
    {
        worldPos = ray.origin + ray.dir * 10000.0;
        distance = 0.0;
    }
    float3 motion = ComputePathTracerMotionVector(worldPos, worldPos + g_ObjectDelta.xyz, g_Frame.view, g_Frame.previousView);
    if (mode == 2) distance = asfloat(0x7f800000u);
    if (mode == 3) distance = asfloat(0x7fc00000u);
    if (mode == 4) distance = -1.0;
    if (mode == 5) motion.x = asfloat(0x7fc00000u);
    if (mode == 6) motion.y = asfloat(0x7f800000u);
    u_Depth[tid.xy] = distance;
    u_Motion[tid.xy] = float4(motion, g_Values.y);
})";

bool IsSurfaceMode(SceneMode Mode)
{
    return Mode == SceneMode::Plane || Mode == SceneMode::NaNMotion || Mode == SceneMode::InfiniteMotion;
}

// S01-S03: depth, motion and ignore-history mask of every render pixel against the CPU oracle.
void CheckPreparedInputs(IRenderDevice* Device, IDeviceContext* Context, IShaderSourceInputStreamFactory* Source)
{
    auto FrameCB = CreateConstants(Device, "SR test frame constants", sizeof(SampleConstants));
    auto CaseCB  = CreateConstants(Device, "SR test case constants", sizeof(SceneConstants));

    RTXPTSuperResolutionInputPass Pass;
    Require(Pass.Initialize(Device, Device->GetEngineFactory(), nullptr, FrameCB), "Super resolution input pass initialization failed");
    Require(!Pass.Prepare(Context, nullptr, nullptr), "Prepare must fail before Resize");
    Require(Pass.Resize(Device, kRenderWidth, kRenderHeight), "Super resolution input pass resize failed");

    const auto Exposure = Readback(Device, Context, Pass.GetExposureSRV()->GetTexture());
    Require(Pass.GetExposureSRV()->GetTexture()->GetDesc().Width == 1 && HalfAt(Exposure, 0) == 1.0f, "Exposure must be a 1x1 texel of 1");

    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc                       = {"SR test scene", SHADER_TYPE_COMPUTE, false};
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.Source                     = kSceneShader;
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.pShaderSourceStreamFactory = Source;
    RefCntAutoPtr<IShader> Shader;
    Device->CreateShader(ShaderCI, &Shader);
    Require(Shader != nullptr, "SR test scene shader failed");
    ComputePipelineStateCreateInfo PSOCI;
    PSOCI.PSODesc.Name                               = "SR test scene";
    PSOCI.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
    PSOCI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    PSOCI.pCS                                        = Shader;
    RefCntAutoPtr<IPipelineState> ScenePSO;
    Device->CreateComputePipelineState(PSOCI, &ScenePSO);
    Require(ScenePSO != nullptr, "SR test scene pipeline failed");

    constexpr Diligent::BIND_FLAGS        GuideFlags  = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    auto                                  RayDistance = CreateTexture(Device, "SR test ray distance", kRenderWidth, kRenderHeight, TEX_FORMAT_R32_FLOAT, GuideFlags);
    auto                                  Motion      = CreateTexture(Device, "SR test motion", kRenderWidth, kRenderHeight, TEX_FORMAT_RGBA16_FLOAT, GuideFlags);
    RefCntAutoPtr<IShaderResourceBinding> SceneSRB;
    ScenePSO->CreateShaderResourceBinding(&SceneSRB, true);
    SceneSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "g_Const")->Set(FrameCB);
    SceneSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "g_Case")->Set(CaseCB);
    SceneSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Depth")->Set(RayDistance->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    SceneSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Motion")->Set(Motion->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));

    // Sources must have the guide formats and cover the render size.
    auto Small = CreateTexture(Device, "SR test small ray distance", kRenderWidth - 1, kRenderHeight, TEX_FORMAT_R32_FLOAT, GuideFlags);
    Require(!Pass.Prepare(Context, RayDistance->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), RayDistance->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)),
            "Prepare must reject a motion source of another format");
    Require(!Pass.Prepare(Context, Small->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), Motion->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)),
            "Prepare must reject a source smaller than the render size");

    const float4x4 Projection   = float4x4::Projection(kFovY, float(kDisplayWidth) / kDisplayHeight, kNearZ, kFarZ, false);
    const float3   TiltedNormal = normalize(float3{0.4f, -0.3f, 1.0f});
    const float4   Front{0, 0, 1, 3};
    const float4   Tilted{TiltedNormal.x, TiltedNormal.y, TiltedNormal.z, 4};
    const float3   Still{0, 0, 0};
    const GpuCase  Cases[] = {
        {"front plane", {0, 0}, Front, Still, Projection, SceneMode::Plane, 1},
        {"tilted plane", {0, 0}, Tilted, Still, Projection, SceneMode::Plane, 1},
        {"tilted plane, jitter only", {0.25f, -0.125f}, Tilted, Still, Projection, SceneMode::Plane, 1},
        {"plane near the near plane", {0, 0}, {0, 0, 1, 0.15f}, Still, Projection, SceneMode::Plane, 1},
        {"plane near the far plane", {0, 0}, {0, 0, 1, 90}, Still, Projection, SceneMode::Plane, 1},
        {"plane beyond the far plane", {0, 0}, {0, 0, 1, 200}, Still, Projection, SceneMode::Plane, 1},
        {"moving object", {0, 0}, Front, {0.5f, -0.25f, 0.1f}, Projection, SceneMode::Plane, 1},
        {"moving camera with jitter", {-0.3f, 0.2f}, Tilted, Still, float4x4::Translation(0.2f, -0.1f, 0.3f) * Projection, SceneMode::Plane, 1},
        {"surface without correspondence", {0, 0}, Front, {0.5f, 0, 0}, Projection, SceneMode::Plane, 0},
        {"sky with camera rotation", {0.1f, 0.1f}, Front, Still, float4x4::RotationY(0.3f) * Projection, SceneMode::Sky, 0},
        {"infinite ray distance", {0, 0}, Front, Still, Projection, SceneMode::InfiniteDistance, 1},
        {"NaN ray distance", {0, 0}, Front, Still, Projection, SceneMode::NaNDistance, 1},
        {"negative ray distance", {0, 0}, Front, Still, Projection, SceneMode::NegativeDistance, 1},
        {"NaN motion", {0, 0}, Front, Still, Projection, SceneMode::NaNMotion, 1},
        {"infinite motion", {0, 0}, Front, Still, Projection, SceneMode::InfiniteMotion, 1},
    };
    for (const GpuCase& Case : Cases)
    {
        SampleConstants Frame;
        MakeFrame(Case.Jitter, Frame);
        Frame.previousView.MatWorldToClipNoOffset = Case.PreviousWorldToClip;
        const SceneConstants Scene{Case.Plane, float4{Case.ObjectDelta, 0}, float4{float(static_cast<Uint32>(Case.Mode)), Case.HistoryValid, 0, 0}};
        Upload(Context, FrameCB, &Frame, sizeof(Frame));
        Upload(Context, CaseCB, &Scene, sizeof(Scene));
        Context->SetPipelineState(ScenePSO);
        Context->CommitShaderResources(SceneSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});
        // One 8x8 group covers the odd render size, so the production bounds guard runs for 29 threads.
        Require(Pass.Prepare(Context, RayDistance->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), Motion->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)),
                "Super resolution input preparation failed");

        const auto      Depth     = Readback(Device, Context, Pass.GetDepthSRV()->GetTexture());
        const auto      SRMotion  = Readback(Device, Context, Pass.GetMotionVectorsSRV()->GetTexture());
        const auto      Mask      = Readback(Device, Context, Pass.GetIgnoreHistoryMaskSRV()->GetTexture());
        const auto      RawMotion = Readback(Device, Context, Motion);
        SampleConstants Unjittered;
        MakeFrame({0, 0}, Unjittered);
        float JitterDepthChange = 0;
        float SkyMotion         = 0;
        for (Uint32 Y = 0; Y < kRenderHeight; ++Y)
        {
            for (Uint32 X = 0; X < kRenderWidth; ++X)
            {
                const size_t      Pixel     = size_t{Y} * kRenderWidth + X;
                const float3      Point     = IntersectPlane(Frame, Case.Plane, X, Y);
                const bool        bSurface  = IsSurfaceMode(Case.Mode);
                const bool        bFinite   = Case.Mode != SceneMode::NaNMotion && Case.Mode != SceneMode::InfiniteMotion;
                const float       WantDepth = bSurface ? ProjectDepth(Projection, Point) : 1.0f;
                const float       GotDepth  = FloatAt(Depth, Pixel);
                const float2      GotMotion{HalfAt(SRMotion, Pixel * 2), HalfAt(SRMotion, Pixel * 2 + 1)};
                const std::string Where = std::string{Case.Name} + " at (" + std::to_string(X) + ", " + std::to_string(Y) + ")";
                Require(std::isfinite(GotDepth) && GotDepth >= 0 && GotDepth <= 1, Where + ": depth outside [0, 1]");
                Require(std::abs(WantDepth - GotDepth) < kDepthTolerance,
                        Where + ": depth " + std::to_string(GotDepth) + ", expected " + std::to_string(WantDepth));
                const Uint8 WantMask = bSurface && (!bFinite || Case.HistoryValid == 0) ? 255 : 0;
                Require(WantMask == Mask[Pixel], Where + ": ignore-history mask " + std::to_string(Mask[Pixel]));
                Require(std::isfinite(GotMotion.x) && std::isfinite(GotMotion.y), Where + ": non-finite motion");
                if (Case.Mode == SceneMode::Sky)
                {
                    // The sky keeps the camera motion the guides computed for it.
                    Require(GotMotion == float2{HalfAt(RawMotion, Pixel * 4), HalfAt(RawMotion, Pixel * 4 + 1)}, Where + ": sky motion changed");
                    SkyMotion = std::max(SkyMotion, length(GotMotion));
                    continue;
                }
                const float2 WantMotion = bFinite ? ExpectedMotion(Frame, Point, Point + Case.ObjectDelta) : float2{0, 0};
                Require(std::abs(WantMotion.x - GotMotion.x) < kMotionTolerance && std::abs(WantMotion.y - GotMotion.y) < kMotionTolerance,
                        Where + ": motion " + std::to_string(GotMotion.x) + ", " + std::to_string(GotMotion.y) +
                            ", expected " + std::to_string(WantMotion.x) + ", " + std::to_string(WantMotion.y));
                if (bSurface)
                    JitterDepthChange = std::max(JitterDepthChange, std::abs(WantDepth - ProjectDepth(Projection, IntersectPlane(Unjittered, Case.Plane, X, Y))));
            }
        }
        if (Case.Jitter != float2{0, 0} && IsSurfaceMode(Case.Mode))
            Require(JitterDepthChange > 10 * kDepthTolerance, std::string{Case.Name} + ": the jitter does not change the expected depth");
        if (Case.Mode == SceneMode::Sky)
            Require(SkyMotion > 1, "Sky case: the camera rotation produced no motion");
        if (Case.Plane == Tilted && Case.Jitter == float2{0, 0})
        {
            // A corner pixel separates projected depth from a ray distance taken for view Z.
            const float3 Corner      = IntersectPlane(Frame, Case.Plane, 0, 0);
            const float  RayT        = length(Corner - Frame.ptConsts.camera.PosW);
            const float  DepthOfRayT = ProjectDepth(Projection, float3{0, 0, RayT});
            Require(std::abs(ProjectDepth(Projection, Corner) - DepthOfRayT) > 100 * kDepthTolerance,
                    "Tilted case cannot separate projected depth from a ray distance taken for view Z");
        }
        std::cout << "SR input case '" << Case.Name << "' passed\n";
        Context->Flush();
        Context->FinishFrame();
    }
}

// S04 and the execute contract: render-size inputs, display-size output, jitter injected once, fixed exposure.
void CheckExecuteAdapter(IRenderDevice* Device, IDeviceContext* Context)
{
    const RTXPTRenderTargetFormats       Formats;
    SuperResolutionSourceSettingsAttribs Query;
    Query.OutputWidth      = kDisplayWidth;
    Query.OutputHeight     = kDisplayHeight;
    Query.OutputFormat     = Formats.ProcessedOutputColor;
    Query.OptimizationType = SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_QUALITY;
    SuperResolutionSourceSettings Source;
    Source.OptimalInputWidth  = kRenderWidth;
    Source.OptimalInputHeight = kRenderHeight;
    Source.MinInputWidth      = 5;
    Source.MinInputHeight     = 4;
    Source.MaxInputWidth      = kDisplayWidth;
    Source.MaxInputHeight     = kDisplayHeight;

    SuperResolutionDesc Desc;
    Require(RTXPTSuperResolutionInputs::MakeDesc(Query, Source, Desc), "MakeDesc rejected a valid query result");
    Require(Desc.InputWidth == kRenderWidth && Desc.InputHeight == kRenderHeight && Desc.OutputWidth == kDisplayWidth &&
                Desc.OutputHeight == kDisplayHeight && Desc.Flags == SUPER_RESOLUTION_FLAG_NONE &&
                Desc.ColorFormat == TEX_FORMAT_RGBA16_FLOAT && Desc.OutputFormat == TEX_FORMAT_RGBA16_FLOAT &&
                Desc.DepthFormat == TEX_FORMAT_R32_FLOAT && Desc.MotionFormat == TEX_FORMAT_RG16_FLOAT &&
                Desc.ExposureFormat == TEX_FORMAT_R16_FLOAT && Desc.ReactiveMaskFormat == TEX_FORMAT_UNKNOWN &&
                Desc.OptimizationType == Query.OptimizationType,
            "MakeDesc produced an unexpected description");
    const RTXPTRenderTargetDimensions Dimensions = RTXPTSuperResolutionInputs::MakeDimensions(Desc);
    Require(Dimensions == RTXPTRenderTargetDimensions{kRenderWidth, kRenderHeight, kDisplayWidth, kDisplayHeight, true} && Dimensions.IsValid(),
            "Render targets must use the input size for guides and the output size for display");

    //Native resolution (DLAA) traces at the display size and still keeps the upscaler's input and output separate.
    auto NativeQuery                = Query;
    NativeQuery.OptimizationType    = SUPER_RESOLUTION_OPTIMIZATION_TYPE_NATIVE;
    auto NativeSource               = Source;
    NativeSource.OptimalInputWidth  = kDisplayWidth;
    NativeSource.OptimalInputHeight = kDisplayHeight;
    NativeSource.MinInputWidth      = kDisplayWidth;
    NativeSource.MinInputHeight     = kDisplayHeight;
    SuperResolutionDesc NativeDesc;
    Require(RTXPTSuperResolutionInputs::MakeDesc(NativeQuery, NativeSource, NativeDesc) &&
                NativeDesc.InputWidth == kDisplayWidth && NativeDesc.InputHeight == kDisplayHeight &&
                NativeDesc.OptimizationType == SUPER_RESOLUTION_OPTIMIZATION_TYPE_NATIVE,
            "MakeDesc must describe a native resolution upscaler at the display size");
    const RTXPTRenderTargetDimensions NativeDimensions = RTXPTSuperResolutionInputs::MakeDimensions(NativeDesc);
    Require(NativeDimensions == RTXPTRenderTargetDimensions{kDisplayWidth, kDisplayHeight, kDisplayWidth, kDisplayHeight, true} && NativeDimensions.IsValid(),
            "Native resolution must keep separate super resolution targets");

    const auto RejectsQuery = [&](const char* Name, auto&& Change) {
        auto BadQuery  = Query;
        auto BadSource = Source;
        Change(BadQuery, BadSource);
        SuperResolutionDesc BadDesc;
        BadDesc.InputWidth = 1;
        Require(!RTXPTSuperResolutionInputs::MakeDesc(BadQuery, BadSource, BadDesc) && BadDesc.InputWidth == 0,
                std::string{"MakeDesc must reject "} + Name);
    };
    RejectsQuery("auto exposure", [](auto& Q, auto&) { Q.Flags = SUPER_RESOLUTION_FLAG_AUTO_EXPOSURE; });
    RejectsQuery("sharpening", [](auto& Q, auto&) { Q.Flags = SUPER_RESOLUTION_FLAG_ENABLE_SHARPENING; });
    RejectsQuery("a zero output", [](auto& Q, auto&) { Q.OutputHeight = 0; });
    RejectsQuery("an invalid quality", [](auto& Q, auto&) { Q.OptimizationType = SUPER_RESOLUTION_OPTIMIZATION_TYPE_COUNT; });
    RejectsQuery("an LDR output", [](auto& Q, auto&) { Q.OutputFormat = TEX_FORMAT_RGBA8_UNORM; });
    RejectsQuery("a zero input", [](auto&, auto& S) { S.OptimalInputWidth = 0; });
    RejectsQuery("an input above the output", [](auto&, auto& S) { S.OptimalInputHeight = kDisplayHeight + 1; S.MaxInputHeight = kDisplayHeight + 1; });
    RejectsQuery("an input below the provider minimum", [](auto&, auto& S) { S.MinInputWidth = kRenderWidth + 1; });
    RejectsQuery("an input above the provider maximum", [](auto&, auto& S) { S.MaxInputHeight = kRenderHeight - 1; });

    auto                          FrameCB = CreateConstants(Device, "SR adapter frame constants", sizeof(SampleConstants));
    RTXPTSuperResolutionInputPass Inputs;
    Require(Inputs.Initialize(Device, Device->GetEngineFactory(), nullptr, FrameCB) && Inputs.Resize(Device, kRenderWidth, kRenderHeight),
            "Super resolution inputs unavailable");
    constexpr Diligent::BIND_FLAGS HdrFlags  = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    auto                           Color     = CreateTexture(Device, "SR adapter color", kRenderWidth, kRenderHeight, Formats.OutputColor, HdrFlags);
    auto                           Output    = CreateTexture(Device, "SR adapter output", kDisplayWidth, kDisplayHeight, Formats.ProcessedOutputColor, HdrFlags);
    auto*                          ColorSRV  = Color->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    auto*                          OutputUAV = Output->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);

    const float2    Jitter{0.375f, -0.125f};
    SampleConstants Frame;
    MakeFrame(Jitter, Frame);
    SampleConstants Plain;
    MakeFrame({0, 0}, Plain);
    Require(Frame.view.PixelOffset == Jitter && Frame.ptConsts.camera.Jitter == float2{Jitter.x, -Jitter.y} &&
                Frame.ptConsts.camera.CameraU == Plain.ptConsts.camera.CameraU && Frame.ptConsts.camera.CameraV == Plain.ptConsts.camera.CameraV &&
                Frame.ptConsts.camera.CameraW == Plain.ptConsts.camera.CameraW &&
                std::memcmp(&Frame.view.MatWorldToClipNoOffset, &Plain.view.MatWorldToClipNoOffset, sizeof(float4x4)) == 0,
            "ApplyJitter must change only the sample offset");

    ExecuteSuperResolutionAttribs Attribs;
    Require(RTXPTSuperResolutionInputs::MakeExecuteAttribs(Context, Desc, Inputs, ColorSRV, OutputUAV, Frame, true, 0.016f, Attribs),
            "MakeExecuteAttribs rejected a consistent frame");
    Require(Attribs.pContext == Context && Attribs.pColorTextureSRV == ColorSRV && Attribs.pOutputTextureView == OutputUAV &&
                Attribs.pDepthTextureSRV == Inputs.GetDepthSRV() && Attribs.pMotionVectorsSRV == Inputs.GetMotionVectorsSRV() &&
                Attribs.pExposureTextureSRV == Inputs.GetExposureSRV() &&
                Attribs.pIgnoreHistoryMaskTextureSRV == Inputs.GetIgnoreHistoryMaskSRV() && Attribs.pReactiveMaskTextureSRV == nullptr &&
                Attribs.JitterX == Jitter.x && Attribs.JitterY == Jitter.y && Attribs.MotionVectorScaleX == 1 &&
                Attribs.MotionVectorScaleY == 1 && Attribs.PreExposure == 1 && Attribs.ExposureScale == 1 && Attribs.Sharpness == 0 &&
                Attribs.CameraNear == kNearZ && Attribs.CameraFar == kFarZ && std::abs(Attribs.CameraFovAngleVert - kFovY) < 1e-6f &&
                Attribs.TimeDeltaInSeconds == 0.016f && Attribs.ResetHistory,
            "MakeExecuteAttribs produced unexpected attributes");

    const auto Rejects = [&](const char* Name, auto&& Change) {
        auto          Bad       = Frame;
        auto          BadDesc   = Desc;
        ITextureView* BadColor  = ColorSRV;
        ITextureView* BadOutput = OutputUAV;
        Change(Bad, BadDesc, BadColor, BadOutput);
        Attribs.pContext = Context;
        Require(!RTXPTSuperResolutionInputs::MakeExecuteAttribs(Context, BadDesc, Inputs, BadColor, BadOutput, Bad, false, 0, Attribs) &&
                    Attribs.pContext == nullptr,
                std::string{"MakeExecuteAttribs must reject "} + Name);
    };
    Rejects("a frame traced at display size", [](auto& F, auto&, auto&, auto&) { F.ptConsts.imageWidth = kDisplayWidth; });
    Rejects("a camera at display size", [](auto& F, auto&, auto&, auto&) { F.ptConsts.camera.ViewportHeight = kDisplayHeight; });
    Rejects("display-size motion scaling", [](auto& F, auto&, auto&, auto&) { F.view.ClipToWindowScale = float2{kDisplayWidth * 0.5f, -(kDisplayHeight * 0.5f)}; });
    Rejects("a display-size viewport", [](auto& F, auto&, auto&, auto&) { F.view.ViewportSize = float2{float(kDisplayWidth), float(kDisplayHeight)}; });
    Rejects("a jitter applied twice", [&](auto& F, auto&, auto&, auto&) { RTXPTSuperResolutionInputs::ApplyJitter(Jitter * 2.0f, F.ptConsts.camera, F.view); F.view.PixelOffset = Jitter; });
    Rejects("a jitter of the other sign", [&](auto& F, auto&, auto&, auto&) { F.ptConsts.camera.Jitter = Jitter; });
    Rejects("a jitter beyond half a pixel", [](auto& F, auto&, auto&, auto&) { RTXPTSuperResolutionInputs::ApplyJitter(float2{0.75f, 0}, F.ptConsts.camera, F.view); });
    Rejects("per-pixel micro-jitter", [](auto& F, auto&, auto&, auto&) { F.ptConsts.perPixelJitterAAScale = 0.5f; });
    Rejects("an inverted depth range", [](auto& F, auto&, auto&, auto&) { F.ptConsts.camera.FarZ = kNearZ * 0.5f; });
    Rejects("a non-finite matrix", [](auto& F, auto&, auto&, auto&) { F.view.MatWorldToClipNoOffset._11 = std::numeric_limits<float>::quiet_NaN(); });
    Rejects("auto exposure", [](auto&, auto& D, auto&, auto&) { D.Flags = SUPER_RESOLUTION_FLAG_AUTO_EXPOSURE; });
    Rejects("an output at input size", [&](auto&, auto&, auto&, auto& O) { O = Color->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS); });
    Rejects("an output aliasing the color input", [&](auto&, auto& D, auto&, auto& O) { D.OutputWidth = kRenderWidth; D.OutputHeight = kRenderHeight; O = Color->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS); });
    Rejects("a color UAV", [&](auto&, auto&, auto& C, auto&) { C = Color->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS); });
    Require(Inputs.Resize(Device, kRenderWidth + 1, kRenderHeight), "Super resolution input resize failed");
    Rejects("inputs prepared at another size", [](auto&, auto&, auto&, auto&) {});
    std::cout << "SR execute adapter cases passed\n";
}
} // namespace

int main(int Argc, char** Argv)
try
{
    Require(Argc == 3, "Usage: SRPrepareInputsTest <D3D12 or Vulkan engine DLL> <RTXPT shaders directory>");
    HMODULE Library = LoadLibraryA(Argv[1]);
    Require(Library != nullptr, "Cannot load engine DLL");
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
        EngineCI.EnableValidation = true;
        GetVkFactory()->CreateDeviceAndContextsVk(EngineCI, &Device, &Context);
    }
    Require(Device && Context, "Cannot create graphics device");

    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    Factory->CreateDefaultShaderSourceStreamFactory(Argv[2], &Source);
    Require(Source != nullptr, "Cannot open the RTXPT shaders directory");
    RTXPTShaderSourceScope ShaderScope{Source};
    CheckPreparedInputs(Device, Context, Source);
    CheckExecuteAdapter(Device, Context);
    Context->WaitForIdle();
    Require(ValidationErrors == 0, "Graphics validation reported errors");
    return 0;
}
catch (const std::exception& Error)
{
    std::cerr << Error.what() << '\n';
    return 1;
}
