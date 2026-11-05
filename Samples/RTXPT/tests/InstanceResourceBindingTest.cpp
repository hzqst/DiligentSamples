// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
// Real components sharing a render state cache, compared with independently cached instances.
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "RenderStateCache.h"
#include "ArchiverFactoryLoader.h"
#include "DebugOutput.h"
#include "Float16.hpp"
#include "GraphicsAccessories.hpp"
#include "PlatformDebug.hpp"
#include "RTXPTAccumulationPass.hpp"
#include "RTXPTComputePass.hpp"
#include "RTXPTDenoisingGuidesBaker.hpp"
#include "RTXPTEmissiveTrianglePass.hpp"
#include "RTXPTLightPacking.hpp"
#include "RTXPTMaterialData.hpp"
#include "RTXPTPostProcessPass.hpp"
#include "RTXPTShaderSource.hpp"
#include "RTXPTSkinnedGeometry.hpp"
#include "RTXPTSubInstanceData.hpp"
#include "RTXPTSuperResolutionInputPass.hpp"
#include "RTXPTTemporalAAPass.hpp"
#include "RTXPTToneMappingPass.hpp"

using namespace Diligent;

namespace
{
std::atomic<Uint32> ValidationErrors{0};
constexpr Uint32    kPrimaryCount   = 2;
constexpr Uint32    kNeighborCount  = 1;
constexpr Uint32    kAlternations   = 3;
constexpr float     kPrimaryOffset  = 2;
constexpr float     kNeighborOffset = 7;
constexpr Uint32    kViewWidth      = 8;
constexpr Uint32    kViewHeight     = 8;
constexpr Uint32    kViewFrames     = 4;

void Require(bool Condition, const std::string& Message)
{
    if (!Condition)
        throw std::runtime_error{Message};
}

void DILIGENT_CALL_TYPE TestMessageCallback(DEBUG_MESSAGE_SEVERITY Severity, const Char* Message, const Char*, const Char*, int)
{
    if (Severity >= DEBUG_MESSAGE_SEVERITY_ERROR)
        ++ValidationErrors;
    if (Severity >= DEBUG_MESSAGE_SEVERITY_WARNING)
        std::cerr << Message << '\n';
}

struct GPU
{
    IEngineFactory*               Factory = nullptr;
    RefCntAutoPtr<IRenderDevice>  Device;
    RefCntAutoPtr<IDeviceContext> Context;

    RefCntAutoPtr<IRenderStateCache> MakeCache() const
    {
        RenderStateCacheCreateInfo CI;
        CI.pDevice          = Device;
        CI.pArchiverFactory = LoadAndGetArchiverFactory();
        RefCntAutoPtr<IRenderStateCache> Cache;
        CreateRenderStateCache(CI, &Cache);
        Require(Cache != nullptr, "Cannot create render state cache");
        return Cache;
    }
};

template <typename T, size_t Count>
RefCntAutoPtr<IBuffer> MakeBuffer(const GPU& Gpu, const char* Name, const std::array<T, Count>& Data, Diligent::BIND_FLAGS Flags, BUFFER_MODE Mode = BUFFER_MODE_STRUCTURED)
{
    BufferDesc Desc;
    Desc.Name              = Name;
    Desc.Size              = sizeof(Data);
    Desc.ElementByteStride = sizeof(T);
    Desc.Mode              = Mode;
    Desc.BindFlags         = Flags;
    Desc.Usage             = USAGE_DEFAULT;
    BufferData             Init{Data.data(), sizeof(Data)};
    RefCntAutoPtr<IBuffer> Buffer;
    Gpu.Device->CreateBuffer(Desc, &Init, &Buffer);
    Require(Buffer != nullptr, std::string{"Cannot create "} + Name);
    return Buffer;
}

std::vector<Uint8> Readback(const GPU& Gpu, IBuffer* Buffer)
{
    BufferDesc Desc;
    Desc.Name           = "Instance resource readback";
    Desc.Size           = Buffer->GetDesc().Size;
    Desc.Usage          = USAGE_STAGING;
    Desc.CPUAccessFlags = CPU_ACCESS_READ;
    RefCntAutoPtr<IBuffer> Staging;
    Gpu.Device->CreateBuffer(Desc, nullptr, &Staging);
    Require(Staging != nullptr, "Cannot create buffer readback");
    Gpu.Context->CopyBuffer(Buffer, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Staging, 0, Desc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Gpu.Context->Flush();
    Gpu.Context->WaitForIdle();
    void* Mapped = nullptr;
    Gpu.Context->MapBuffer(Staging, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Mapped);
    Require(Mapped != nullptr, "Cannot map buffer readback");
    std::vector<Uint8> Bytes(static_cast<size_t>(Desc.Size));
    std::memcpy(Bytes.data(), Mapped, Bytes.size());
    Gpu.Context->UnmapBuffer(Staging, MAP_READ);
    return Bytes;
}

std::vector<Uint8> Readback(const GPU& Gpu, ITextureView* View)
{
    ITexture*   Texture = View->GetTexture();
    TextureDesc Desc    = Texture->GetDesc();
    Desc.Name           = "Instance texture readback";
    Desc.BindFlags      = BIND_NONE;
    Desc.Usage          = USAGE_STAGING;
    Desc.CPUAccessFlags = CPU_ACCESS_READ;
    RefCntAutoPtr<ITexture> Staging;
    Gpu.Device->CreateTexture(Desc, nullptr, &Staging);
    Require(Staging != nullptr, "Cannot create texture readback");
    CopyTextureAttribs Copy;
    Copy.pSrcTexture              = Texture;
    Copy.pDstTexture              = Staging;
    Copy.SrcTextureTransitionMode = Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    Gpu.Context->CopyTexture(Copy);
    Gpu.Context->Flush();
    Gpu.Context->WaitForIdle();
    MappedTextureSubresource Mapped;
    Gpu.Context->MapTextureSubresource(Staging, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Mapped);
    Require(Mapped.pData != nullptr, "Cannot map texture readback");
    const size_t       RowBytes = size_t{Desc.Width} * GetTextureFormatAttribs(Desc.Format).GetElementSize();
    std::vector<Uint8> Bytes(RowBytes * Desc.Height);
    for (Uint32 Row = 0; Row < Desc.Height; ++Row)
        std::memcpy(Bytes.data() + Row * RowBytes, static_cast<const Uint8*>(Mapped.pData) + Row * Mapped.Stride, RowBytes);
    Gpu.Context->UnmapTextureSubresource(Staging, 0, 0);
    return Bytes;
}

std::vector<Uint8> Snapshot(const GPU& Gpu, std::initializer_list<ITextureView*> Views)
{
    std::vector<Uint8> Bytes;
    for (auto* View : Views)
    {
        Require(View != nullptr, "Missing output view");
        const auto Part = Readback(Gpu, View);
        Bytes.insert(Bytes.end(), Part.begin(), Part.end());
    }
    return Bytes;
}

template <typename T>
void UploadTexture(const GPU& Gpu, ITextureView* View, const std::vector<T>& Pixels)
{
    ITexture*    Texture  = View->GetTexture();
    const auto&  Desc     = Texture->GetDesc();
    const size_t RowBytes = size_t{Desc.Width} * GetTextureFormatAttribs(Desc.Format).GetElementSize();
    Require(Pixels.size() * sizeof(T) == RowBytes * Desc.Height, "Wrong texture upload size");
    const TextureSubResData Data{Pixels.data(), RowBytes};
    for (Uint32 Slice = 0; Slice < Desc.ArraySize; ++Slice)
        Gpu.Context->UpdateTexture(Texture, 0, Slice, Box{0, Desc.Width, 0, Desc.Height}, Data,
                                   RESOURCE_STATE_TRANSITION_MODE_TRANSITION, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

struct ViewFixture
{
    Uint32                 Seed;
    SampleConstants        Frame;
    RTXPTRenderTargets     Targets;
    RefCntAutoPtr<IBuffer> Constants;

    ViewFixture(const GPU& Gpu, Uint32 Variant) :
        Seed{Variant}
    {
        RTXPTRenderTargetCreateInfo CI;
        CI.Dimensions.RenderWidth = CI.Dimensions.DisplayWidth = kViewWidth;
        CI.Dimensions.RenderHeight = CI.Dimensions.DisplayHeight = kViewHeight;
        CI.CreateComputeOutput = CI.CreateAccumulatedRadiance = CI.CreateRealtimeResources = true;
        CI.CreateDenoiserValidation                                                        = true;
        Require(Targets.Resize(Gpu.Device, CI), "Cannot create view targets");
        BufferDesc Desc;
        Desc.Name           = "Instance frame constants";
        Desc.Size           = sizeof(Frame);
        Desc.BindFlags      = BIND_UNIFORM_BUFFER;
        Desc.Usage          = USAGE_DYNAMIC;
        Desc.CPUAccessFlags = CPU_ACCESS_WRITE;
        Gpu.Device->CreateBuffer(Desc, nullptr, &Constants);
        Require(Constants != nullptr, "Cannot create view constants");

        constexpr float kNear = 0.1f, kFar = 100;
        const float     Fov    = 1 + Seed * 0.1f;
        auto&           Camera = Frame.ptConsts.camera;
        Camera.CameraU         = float3{std::tan(Fov * 0.5f), 0, 0};
        Camera.CameraV         = float3{0, std::tan(Fov * 0.5f), 0};
        Camera.CameraW = Camera.DirectionW = float3{0, 0, 1};
        Camera.NearZ                       = kNear;
        Camera.FarZ                        = kFar;
        Camera.AspectRatio = Camera.FocalDistance = 1;
        Camera.ViewportWidth                      = kViewWidth;
        Camera.ViewportHeight                     = kViewHeight;
        Frame.ptConsts.prevCamera                 = Camera;
        auto& View                                = Frame.view;
        View.MatWorldToView                       = float4x4::Identity();
        View.MatViewToClip                        = float4x4::Projection(Fov, 1, kNear, kFar, Gpu.Device->GetDeviceInfo().NDC.MinZ < 0);
        View.MatWorldToClip = View.MatWorldToClipNoOffset = View.MatViewToClip;
        View.MatClipToWorldNoOffset                       = View.MatViewToClip.Inverse();
        View.ViewportSize                                 = float2{float(kViewWidth), float(kViewHeight)};
        View.ViewportSizeInv                              = float2{1.0f / kViewWidth, 1.0f / kViewHeight};
        View.ClipToWindowScale                            = float2{kViewWidth * 0.5f, kViewHeight * -0.5f};
        View.ClipToWindowBias                             = float2{kViewWidth * 0.5f, kViewHeight * 0.5f};
        Frame.previousView                                = View;
        Frame.camera                                      = Camera;
        Frame.ptConsts.imageWidth                         = kViewWidth;
        Frame.ptConsts.imageHeight                        = kViewHeight;
        Frame.ptConsts._activeDenoisingLayerCount         = Seed == 1 ? kPrimaryCount : kNeighborCount;
        Frame.viewportSizeAndFrameIndex                   = float4{float(kViewWidth), float(kViewHeight), 0, 0};

        constexpr Uint32    kChannels  = 4;
        const size_t        PixelCount = size_t{kViewWidth} * kViewHeight;
        std::vector<Uint16> Color(PixelCount * kChannels), Motion(Color.size());
        for (size_t Pixel = 0; Pixel < PixelCount; ++Pixel)
        {
            const float Value = Seed * 0.1f + float(Pixel) * 0.001f;
            for (Uint32 Channel = 0; Channel < kChannels; ++Channel)
                Color[Pixel * kChannels + Channel] = Float16{Channel == kChannels - 1 ? 1 : Value}.Raw();
            Motion[Pixel * kChannels]                 = Float16{Seed * 0.125f}.Raw();
            Motion[Pixel * kChannels + kChannels - 1] = Float16{1.0f}.Raw();
        }
        UploadTexture(Gpu, Targets.GetOutputColorSRV(), Color);
        UploadTexture(Gpu, Targets.GetProcessedOutputColorSRV(), Color);
        UploadTexture(Gpu, Targets.GetStableRadianceUAV(), Color);
        UploadTexture(Gpu, Targets.GetDepthSRV(), std::vector<float>(PixelCount, float(Seed + 2)));
        UploadTexture(Gpu, Targets.GetSpecularHitTUAV(), std::vector<float>(PixelCount, float(Seed + 1)));
        UploadTexture(Gpu, Targets.GetScreenMotionVectorsSRV(), Motion);
        UploadTexture(Gpu, Targets.GetDenoisingLayersHeaderUAV(), std::vector<Uint32>(PixelCount, std::numeric_limits<Uint32>::max()));
    }

    void Upload(const GPU& Gpu, Uint32 Step)
    {
        Frame.viewportSizeAndFrameIndex.w = float(Seed * 32 + Step);
        Frame.ptConsts.frameIndex         = Step;
        void* Mapped                      = nullptr;
        Gpu.Context->MapBuffer(Constants, MAP_WRITE, MAP_FLAG_DISCARD, Mapped);
        Require(Mapped != nullptr, "Cannot map view constants");
        std::memcpy(Mapped, &Frame, sizeof(Frame));
        Gpu.Context->UnmapBuffer(Constants, MAP_WRITE);
    }
};

template <typename Pass, typename Initialize, typename Render>
void CheckViews(const GPU& Gpu, const char* Name, Initialize Init, Render Draw)
{
    auto        Shared    = Gpu.MakeCache();
    auto        IsolatedA = Gpu.MakeCache();
    auto        IsolatedB = Gpu.MakeCache();
    Pass        A, B, ReferenceA, ReferenceB;
    ViewFixture DataA{Gpu, 1}, DataB{Gpu, 2}, RefA{Gpu, 1}, RefB{Gpu, 2};
    Init(A, DataA, Shared.RawPtr());
    Init(B, DataB, Shared.RawPtr());
    Init(ReferenceA, RefA, IsolatedA.RawPtr());
    Init(ReferenceB, RefB, IsolatedB.RawPtr());
    for (Uint32 Step = 0; Step < kViewFrames; ++Step)
    {
        const auto Compare = [&](Pass& ActualPass, ViewFixture& Actual, Pass& ExpectedPass, ViewFixture& Expected) {
            Expected.Upload(Gpu, Step);
            const auto ExpectedBytes = Draw(ExpectedPass, Expected, Step);
            Actual.Upload(Gpu, Step);
            const auto ActualBytes = Draw(ActualPass, Actual, Step);
            Require(ExpectedBytes == ActualBytes, std::string{Name} + ": shared-cache output differs from independent instance");
        };
        if (Step % 2 == 0)
        {
            Compare(A, DataA, ReferenceA, RefA);
            Compare(B, DataB, ReferenceB, RefB);
        }
        else
        {
            Compare(B, DataB, ReferenceB, RefB);
            Compare(A, DataA, ReferenceA, RefA);
        }
    }
}

void CheckViewPasses(const GPU& Gpu, const std::string& Filter, const auto& Run)
{
    const auto Selected = [&](const char* Name) {
        return Filter == "all" || Filter == Name;
    };
    if (Selected("accumulation"))
        Run("accumulation views", [&] {
            CheckViews<RTXPTAccumulationPass>(
                Gpu, "accumulation",
                [&](auto& Pass, auto&, auto* Cache) { Require(Pass.Initialize(Gpu.Device, Gpu.Factory, Cache, nullptr, true), "Cannot initialize accumulation"); },
                [&](auto& Pass, auto& Data, Uint32 Step) {
                    RTXPTAccumulationDispatch Dispatch;
                    Dispatch.pInputColorSRV          = Data.Targets.GetOutputColorSRV();
                    Dispatch.pAccumulatedRadianceUAV = Data.Targets.GetAccumulatedRadianceUAV();
                    Dispatch.pProcessedOutputUAV     = Data.Targets.GetProcessedOutputColorUAV();
                    Dispatch.InputWidth = Dispatch.OutputWidth = kViewWidth;
                    Dispatch.InputHeight = Dispatch.OutputHeight = kViewHeight;
                    Dispatch.BlendFactor                         = Step == 0 ? 1 : 1.0f / (Data.Seed + Step);
                    Require(Pass.Render(Gpu.Context, Dispatch), "Cannot execute accumulation");
                    return Snapshot(Gpu, {Data.Targets.GetProcessedOutputColorSRV(), Data.Targets.GetAccumulatedRadianceSRV()});
                });
        });
    if (Selected("compute"))
        Run("compute views", [&] {
            CheckViews<RTXPTComputePass>(
                Gpu, "compute",
                [&](auto& Pass, auto& Data, auto* Cache) {
                    RTXPTComputePassCreateInfo CI;
                    CI.pEngineFactory   = Gpu.Factory;
                    CI.pStateCache      = Cache;
                    CI.pFrameConstants  = Data.Constants;
                    CI.PassName         = "Instance compute regression";
                    CI.ShaderFilePath   = "RTXPTDebugCompute.csh";
                    CI.ComputeSupported = true;
                    Require(Pass.Initialize(Gpu.Device, CI), "Cannot initialize compute pass");
                },
                [&](auto& Pass, auto& Data, Uint32) {
                    Require(Pass.Dispatch(Gpu.Context, Data.Targets.GetOutputColorSRV(), Data.Targets.GetComputeColorUAV(), kViewWidth, kViewHeight), "Cannot execute compute pass");
                    return Snapshot(Gpu, {Data.Targets.GetComputeColorSRV()});
                });
        });
    if (Selected("guides"))
        Run("denoising guide views", [&] {
            CheckViews<RTXPTDenoisingGuidesBaker>(
                Gpu, "guides",
                [&](auto& Pass, auto& Data, auto* Cache) { Require(Pass.Initialize(Gpu.Device, Gpu.Factory, Cache, Data.Constants, true), "Cannot initialize guides"); },
                [&](auto& Pass, auto& Data, Uint32) {
                    const auto DebugView = Data.Seed == 1 ? RTXPTDenoisingGuideDebugView::Depth : RTXPTDenoisingGuideDebugView::MotionVectors;
                    Require(Pass.Bake(Gpu.Context, Data.Targets, DebugView, RTXPTGuideHistoryMode::CurrentFrame), "Cannot execute guides");
                    return Snapshot(Gpu, {Data.Targets.GetProcessedOutputColorSRV(), Data.Targets.GetSpecularHitTSRV(), Data.Targets.GetSpecularMotionSRV(), Data.Targets.GetDenoiserAvgLayerRadianceHalfResSRV()});
                });
        });
    if (Selected("postprocess"))
        Run("postprocess views", [&] {
            CheckViews<RTXPTPostProcessPass>(
                Gpu, "postprocess",
                [&](auto& Pass, auto& Data, auto* Cache) {
                    Require(Pass.Initialize(Gpu.Device, Gpu.Factory, Cache, Data.Constants, true), "Cannot initialize postprocess");
                    const size_t PixelCount = size_t{kViewWidth} * kViewHeight;
                    // Skip surface records and let validation color exercise both frame and mini constants.
                    UploadTexture(Gpu, Data.Targets.GetDenoiserViewspaceZUAV(), std::vector<float>(PixelCount, std::numeric_limits<float>::max()));
                    UploadTexture(Gpu, Data.Targets.GetDenoiserOutValidationUAV(), std::vector<Uint8>(PixelCount * 4, 255));
                },
                [&](auto& Pass, auto& Data, Uint32) {
                    RTXPTDenoiserPostProcessAttribs Attribs;
                    Attribs.pRenderTargets  = &Data.Targets;
                    Attribs.pMergeOutputUAV = Data.Targets.GetProcessedOutputColorUAV();
                    Require(Pass.RunNoDenoiserFinalMerge(Gpu.Context, Attribs), "Cannot execute postprocess");
                    Attribs.PlaneIndex    = Data.Seed == 1 ? 1 : 0;
                    Attribs.HasValidation = Data.Seed == 1;
                    Require(Pass.RunDenoiserFinalMerge(Gpu.Context, Attribs), "Cannot execute denoiser merge");
                    return Snapshot(Gpu, {Data.Targets.GetProcessedOutputColorSRV()});
                });
        });
    if (Selected("srinputs"))
        Run("super resolution input views", [&] {
            CheckViews<RTXPTSuperResolutionInputPass>(
                Gpu, "srinputs",
                [&](auto& Pass, auto& Data, auto* Cache) {
                    Require(Pass.Initialize(Gpu.Device, Gpu.Factory, Cache, Data.Constants), "Cannot initialize SR inputs");
                    Require(Pass.Resize(Gpu.Device, kViewWidth, kViewHeight), "Cannot resize SR inputs");
                },
                [&](auto& Pass, auto& Data, Uint32) {
                    Require(Pass.Prepare(Gpu.Context, Data.Targets.GetDepthSRV(), Data.Targets.GetScreenMotionVectorsSRV()), "Cannot execute SR inputs");
                    return Snapshot(Gpu, {Pass.GetDepthSRV(), Pass.GetMotionVectorsSRV(), Pass.GetIgnoreHistoryMaskSRV()});
                });
        });
    if (Selected("taa"))
        Run("temporal AA views", [&] {
            CheckViews<RTXPTTemporalAAPass>(
                Gpu, "taa",
                [&](auto& Pass, auto&, auto* Cache) { Require(Pass.Initialize(Gpu.Device, Cache), "Cannot initialize TAA"); },
                [&](auto& Pass, auto& Data, Uint32 Step) {
                    RTXPTTemporalAAFrameAttribs Attribs;
                    Attribs.pDevice           = Gpu.Device;
                    Attribs.pDeviceContext    = Gpu.Context;
                    Attribs.pRenderTargets    = &Data.Targets;
                    Attribs.pFrameConstants   = &Data.Frame;
                    Attribs.FrameIndex        = Step;
                    Attribs.ResetHistory      = Step == 0;
                    Attribs.PreviousViewValid = Step != 0;
                    Require(Pass.Execute(Attribs), "Cannot execute TAA");
                    return Snapshot(Gpu, {Data.Targets.GetProcessedOutputColorSRV()});
                });
        });
    if (Selected("tonemapping"))
        Run("tone mapping views", [&] {
            CheckViews<RTXPTToneMappingPass>(
                Gpu, "tonemapping",
                [&](auto& Pass, auto&, auto* Cache) { Require(Pass.Initialize(Gpu.Device, Gpu.Factory, Cache, nullptr, TEX_FORMAT_RGBA8_UNORM, true), "Cannot initialize tone mapping"); },
                [&](auto& Pass, auto& Data, Uint32 Step) {
                    RTXPTToneMappingParameters Params;
                    Params.ToneMapOperator      = RTXPTToneMapperOperator::Linear;
                    Params.ExposureCompensation = float(Data.Seed + Step);
                    RTXPTToneMappingRenderAttribs Attribs;
                    Attribs.pSourceSRV = Data.Targets.GetOutputColorSRV();
                    Attribs.pLdrRTV    = Data.Targets.GetLdrColorRTV();
                    Attribs.Width      = kViewWidth;
                    Attribs.Height     = kViewHeight;
                    Attribs.pParams    = &Params;
                    Require(Pass.Render(Gpu.Context, Attribs), "Cannot execute tone mapping");
                    return Snapshot(Gpu, {Data.Targets.GetLdrColorSRV()});
                });
        });
}

struct SkinningFixture
{
    Uint32                 Count;
    RefCntAutoPtr<IBuffer> Vertices, Skin, Joints, Output;

    SkinningFixture(const GPU& Gpu, Uint32 VertexCount, float Offset) :
        Count{VertexCount}
    {
        std::array<RTXPTGeometryVertex, kPrimaryCount> Source{};
        std::array<RTXPTSkinVertex, kPrimaryCount>     Influences{};
        for (Uint32 Index = 0; Index < kPrimaryCount; ++Index)
        {
            Source[Index].position    = float3{Offset + Index, Offset, 1};
            Source[Index].normal      = float3{0, 1, 0};
            Influences[Index].weights = float4{1, 0, 0, 0};
        }
        std::array<float4x4, 1> Palette{float4x4::Translation(Offset, Offset, Offset)};
        Vertices = MakeBuffer(Gpu, "Instance skin vertices", Source, BIND_SHADER_RESOURCE);
        Skin     = MakeBuffer(Gpu, "Instance skin weights", Influences, BIND_SHADER_RESOURCE);
        Joints   = MakeBuffer(Gpu, "Instance skin joints", Palette, BIND_SHADER_RESOURCE);
        NewOutput(Gpu);
    }

    void NewOutput(const GPU& Gpu)
    {
        Output = MakeBuffer(Gpu, "Instance skinned output", std::array<RTXPTGeometryVertex, kPrimaryCount>{}, BIND_UNORDERED_ACCESS);
    }

    void Render(const GPU& Gpu, RTXPTSkinningPass& Pass) const
    {
        RTXPTSkinningBindings Bindings{Vertices, Skin, Joints, Output};
        Require(Pass.Bind(Bindings), "Cannot bind skinning fixture");
        RTXPTSkinningDispatch Range;
        Range.VertexCount = Count;
        Require(Pass.Dispatch(Gpu.Context, Range), "Cannot dispatch skinning fixture");
    }
};

struct EmissiveFixture
{
    Uint32                 Count;
    RefCntAutoPtr<IBuffer> Vertices, Materials, Instances, Transforms, Indices, Output;

    EmissiveFixture(const GPU& Gpu, Uint32 InstanceCount, float Offset) :
        Count{InstanceCount}
    {
        constexpr Uint32                                   kTriangleVertices = 3;
        std::array<RTXPTGeometryVertex, kTriangleVertices> Triangle{};
        Triangle[0].position = float3{Offset, 0, 0};
        Triangle[1].position = float3{Offset + 1, 0, 0};
        Triangle[2].position = float3{Offset, 1, 0};
        std::array<MaterialPTData, 1> Material{};
        Material[0].emissiveFactor = float3{Offset, 1, 1};
        Material[0].flags          = kMaterialFlag_EmissiveAreaLight;
        std::array<SubInstanceData, kPrimaryCount> Rows{};
        std::array<float4x4, kPrimaryCount>        Matrices{};
        for (Uint32 Index = 0; Index < kPrimaryCount; ++Index)
        {
            Rows[Index].VertexCount            = kTriangleVertices;
            Rows[Index].EmissiveTriangleOffset = Index;
            Matrices[Index]                    = float4x4::Translation(0, static_cast<float>(Index), 0);
        }
        Vertices   = MakeBuffer(Gpu, "Instance emitter vertices", Triangle, BIND_SHADER_RESOURCE);
        Materials  = MakeBuffer(Gpu, "Instance emitter materials", Material, BIND_SHADER_RESOURCE);
        Instances  = MakeBuffer(Gpu, "Instance emitter rows", Rows, BIND_SHADER_RESOURCE);
        Transforms = MakeBuffer(Gpu, "Instance emitter transforms", Matrices, BIND_SHADER_RESOURCE);
        Indices    = MakeBuffer(Gpu, "Instance emitter indices", std::array<Uint32, kTriangleVertices>{0, 1, 2}, BIND_SHADER_RESOURCE, BUFFER_MODE_FORMATTED);
        NewOutput(Gpu);
    }

    void NewOutput(const GPU& Gpu)
    {
        Output = MakeBuffer(Gpu, "Instance emissive output", std::array<PolymorphicLightInfo, kPrimaryCount>{}, BIND_UNORDERED_ACCESS);
    }

    void Render(const GPU& Gpu, RTXPTEmissiveTrianglePass& Pass) const
    {
        RTXPTEmissiveTriangleBindings Bindings;
        Bindings.pMaterialBuffer             = Materials;
        Bindings.pSubInstanceBuffer          = Instances;
        Bindings.pSubInstanceTransformBuffer = Transforms;
        Bindings.pVertexBuffer = Bindings.pSkinnedVertexBuffer = Vertices;
        Bindings.pIndexBuffer                                  = Indices;
        Bindings.pEmissiveTriangleBuffer                       = Output;
        Require(Pass.Bind(Bindings), "Cannot bind emissive fixture");
        Require(Pass.Dispatch(Gpu.Context, Count), "Cannot dispatch emissive fixture");
    }
};

// Keep A alive while B requests the same cached PSO. Prime B immediately before A so even a wrong dynamic
// buffer is valid on this frame: the old implementation must fail the output comparison, not a mapping check.
template <typename Pass, typename Fixture, typename Initialize>
void CheckRebinding(const GPU& Gpu, const char* Name, bool DelayedFirstBind, Initialize Init)
{
    auto    Shared    = Gpu.MakeCache();
    auto    IsolatedA = Gpu.MakeCache();
    auto    IsolatedB = Gpu.MakeCache();
    Pass    A, B, ReferenceA, ReferenceB;
    Fixture DataA{Gpu, kPrimaryCount, kPrimaryOffset};
    Fixture DataB{Gpu, kNeighborCount, kNeighborOffset};
    Fixture RefA{Gpu, kPrimaryCount, kPrimaryOffset};
    Fixture RefB{Gpu, kNeighborCount, kNeighborOffset};
    Init(ReferenceA, IsolatedA);
    Init(ReferenceB, IsolatedB);
    RefA.Render(Gpu, ReferenceA);
    const auto ExpectedA = Readback(Gpu, RefA.Output);
    RefB.Render(Gpu, ReferenceB);
    const auto ExpectedB = Readback(Gpu, RefB.Output);
    Require(ExpectedA != ExpectedB, "Independent fixtures must produce different outputs");
    Require(std::any_of(ExpectedA.begin(), ExpectedA.end(), [](Uint8 Byte) { return Byte != 0; }), "Independent fixture must write data");

    Init(A, Shared);
    if (!DelayedFirstBind)
    {
        DataA.Render(Gpu, A);
        Require(ExpectedA == Readback(Gpu, DataA.Output), "First instance differs before sharing");
    }
    Init(B, Shared);
    for (Uint32 Round = 0; Round < kAlternations; ++Round)
    {
        DataB.Render(Gpu, B);
        DataA.NewOutput(Gpu);
        DataA.Render(Gpu, A);
        Require(ExpectedA == Readback(Gpu, DataA.Output), std::string{Name} + ": A read another instance's constants");
        Require(ExpectedB == Readback(Gpu, DataB.Output), std::string{Name} + ": B differs from its independent instance");

        DataA.Render(Gpu, A);
        DataB.NewOutput(Gpu);
        DataB.Render(Gpu, B);
        Require(ExpectedB == Readback(Gpu, DataB.Output), std::string{Name} + ": rebuilt B read another instance's constants");
        Require(ExpectedA == Readback(Gpu, DataA.Output), std::string{Name} + ": A changed when B ran");
    }
}
} // namespace

int main(int Argc, char** Argv)
try
{
    Require(Argc == 3 || Argc == 4, "Usage: RTXPTInstanceResourceBindingTest <engine DLL> <shader search paths> [pass name]");
    SetDebugMessageCallback(TestMessageCallback);
    // Report assertion failures through the callback and exit code during unattended runs.
    PlatformDebug::SetBreakOnError(false);
    const HMODULE Library = LoadLibraryExA(Argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Require(Library != nullptr, "Cannot load graphics engine");
    GPU  Gpu;
    auto GetD3D12 = reinterpret_cast<IEngineFactoryD3D12* (*)()>(GetProcAddress(Library, "GetEngineFactoryD3D12"));
    if (GetD3D12)
    {
        auto* Factory = GetD3D12();
        Gpu.Factory   = Factory;
        Factory->SetMessageCallback(TestMessageCallback);
        Factory->SetBreakOnError(false);
        EngineD3D12CreateInfo CI;
        CI.EnableValidation = true;
        Factory->CreateDeviceAndContextsD3D12(CI, &Gpu.Device, &Gpu.Context);
    }
    else
    {
        auto GetVulkan = reinterpret_cast<IEngineFactoryVk* (*)()>(GetProcAddress(Library, "GetEngineFactoryVk"));
        Require(GetVulkan != nullptr, "Missing engine factory");
        auto* Factory = GetVulkan();
        Gpu.Factory   = Factory;
        Factory->SetMessageCallback(TestMessageCallback);
        Factory->SetBreakOnError(false);
        EngineVkCreateInfo CI;
        CI.EnableValidation = true;
        Factory->CreateDeviceAndContextsVk(CI, &Gpu.Device, &Gpu.Context);
    }
    Require(Gpu.Device && Gpu.Context, "Cannot create GPU device");
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    Gpu.Factory->CreateDefaultShaderSourceStreamFactory(Argv[2], &Source);
    RTXPTShaderSourceScope Scope{Source};
    const std::string      Filter   = Argc == 4 ? Argv[3] : "all";
    Uint32                 Failures = 0;
    const auto             Run      = [&](const std::string& Name, const auto& Test) {
        try
        {
            Test();
            std::cout << "PASS " << Name << '\n';
        }
        catch (const std::exception& Error)
        {
            ++Failures;
            std::cerr << "FAIL " << Name << ": " << Error.what() << '\n';
        }
        Gpu.Context->Flush();
        Gpu.Context->WaitForIdle();
    };
    if (Filter == "all" || Filter == "skinning")
    {
        const auto Init = [&](RTXPTSkinningPass& Pass, IRenderStateCache* Cache) {
            Require(Pass.Initialize(Gpu.Device, Gpu.Factory, Cache, nullptr), "Cannot initialize skinning pass");
        };
        for (bool Delayed : {false, true})
            Run(Delayed ? "skinning delayed first bind" : "skinning SRB rebuild", [&] {
                CheckRebinding<RTXPTSkinningPass, SkinningFixture>(Gpu, "skinning", Delayed, Init);
            });
    }
    if (Filter == "all" || Filter == "emissive")
    {
        const auto Init = [&](RTXPTEmissiveTrianglePass& Pass, IRenderStateCache* Cache) {
            RTXPTEmissiveTrianglePassCreateInfo CI;
            CI.pEngineFactory   = Gpu.Factory;
            CI.pStateCache      = Cache;
            CI.IndexValueType   = VT_UINT32;
            CI.ComputeSupported = true;
            Require(Pass.Initialize(Gpu.Device, CI), "Cannot initialize emissive pass");
        };
        for (bool Delayed : {false, true})
            Run(Delayed ? "emissive delayed first bind" : "emissive SRB rebuild", [&] {
                CheckRebinding<RTXPTEmissiveTrianglePass, EmissiveFixture>(Gpu, "emissive", Delayed, Init);
            });
    }
    CheckViewPasses(Gpu, Filter, Run);
    const std::array<const char*, 10> Filters{{"all", "skinning", "emissive", "accumulation", "compute", "guides", "postprocess", "srinputs", "taa", "tonemapping"}};
    Require(std::any_of(Filters.begin(), Filters.end(), [&](const char* Name) { return Filter == Name; }), "Unknown test filter");
    std::cout << "Instance resource validation: failures=" << Failures << ", validation errors=" << ValidationErrors.load() << '\n';
    return Failures == 0 && ValidationErrors == 0 ? 0 : 1;
}
catch (const std::exception& Error)
{
    std::cerr << "FAIL: " << Error.what() << '\n';
    return 1;
}
