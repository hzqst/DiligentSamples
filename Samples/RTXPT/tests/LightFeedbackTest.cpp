// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "../src/RTXPTFrameConstants.hpp"
#include "../src/RTXPTLightFeedback.hpp"
#include "../src/RTXPTLightProxyBuildPass.hpp"
#include "../src/RTXPTLightHistory.hpp"
using namespace Diligent;
using uint     = Uint32;
using float3x4 = std::array<float4, 3>;
#include "../shaders/PathTracer/Lighting/LightingTypes.hlsli"

namespace
{
std::atomic<unsigned>   Errors{0};
void DILIGENT_CALL_TYPE Message(DEBUG_MESSAGE_SEVERITY Severity, const Char* Text, const Char*, const Char*, int)
{
    if (Severity >= DEBUG_MESSAGE_SEVERITY_ERROR) ++Errors;
    if (Severity >= DEBUG_MESSAGE_SEVERITY_WARNING) std::cerr << Text << '\n';
}
void Require(bool Value, const char* Text)
{
    if (!Value) throw std::runtime_error{Text};
}
} // namespace

int main(int Argc, char** Argv)
try
{
    Require(Argc == 2, "Run from the RTXPT directory: LightFeedbackTest <engine DLL>");
    const std::string DLL   = Argv[1];
    const auto        Slash = DLL.find_last_of("/\\");
    if (Slash != std::string::npos) SetDllDirectoryA(DLL.substr(0, Slash).c_str());
    HMODULE Library = LoadLibraryA(Argv[1]);
    Require(Library != nullptr, "Cannot load graphics engine");
    RefCntAutoPtr<IRenderDevice>  Device;
    RefCntAutoPtr<IDeviceContext> Context;
    IEngineFactory*               Factory = nullptr;
    auto                          D3D12   = reinterpret_cast<IEngineFactoryD3D12* (*)()>(GetProcAddress(Library, "GetEngineFactoryD3D12"));
    if (D3D12)
    {
        Factory = D3D12();
        Factory->SetMessageCallback(Message);
        EngineD3D12CreateInfo CI;
        CI.EnableValidation = true;
        D3D12()->CreateDeviceAndContextsD3D12(CI, &Device, &Context);
    }
    else
    {
        auto Vk = reinterpret_cast<IEngineFactoryVk* (*)()>(GetProcAddress(Library, "GetEngineFactoryVk"));
        Require(Vk != nullptr, "Cannot find Vulkan factory");
        Factory = Vk();
        Factory->SetMessageCallback(Message);
        EngineVkCreateInfo CI;
        CI.EnableValidation = true;
        Vk()->CreateDeviceAndContextsVk(CI, &Device, &Context);
    }
    Require(Device && Context, "Cannot create device");
    constexpr Uint32 Width = 13, Height = 11, Pixels = Width * Height, Tiles = 9, Entries = Tiles * 128;
    auto             Buffer = [&](Uint32 Size, Uint32 Stride, const void* Data) {
        BufferDesc Desc;
        Desc.Name              = "Feedback test buffer";
        Desc.Size              = Size;
        Desc.ElementByteStride = Stride;
        Desc.Mode              = BUFFER_MODE_STRUCTURED;
        Desc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        BufferData             Initial{Data, Size};
        RefCntAutoPtr<IBuffer> Result;
        Device->CreateBuffer(Desc, Data ? &Initial : nullptr, &Result);
        Require(Result != nullptr, "Cannot create buffer");
        return Result;
    };
    auto Texture = [&](TEXTURE_FORMAT Format, Uint32 Stride, const void* Data) {
        TextureDesc Desc;
        Desc.Name      = "Feedback test texture";
        Desc.Type      = RESOURCE_DIM_TEX_2D;
        Desc.Width     = Width;
        Desc.Height    = Height;
        Desc.Format    = Format;
        Desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        TextureSubResData       Sub{Data, Uint64{Width} * Stride};
        TextureData             Initial{&Sub, 1};
        RefCntAutoPtr<ITexture> Result;
        Device->CreateTexture(Desc, Data ? &Initial : nullptr, &Result);
        Require(Result != nullptr, "Cannot create texture");
        return Result;
    };
    auto Read = [&](IBuffer* Source) {
        BufferDesc Desc        = Source->GetDesc();
        Desc.Usage             = USAGE_STAGING;
        Desc.CPUAccessFlags    = CPU_ACCESS_READ;
        Desc.BindFlags         = BIND_NONE;
        Desc.Mode              = BUFFER_MODE_UNDEFINED;
        Desc.ElementByteStride = 0;
        RefCntAutoPtr<IBuffer> Staging;
        Device->CreateBuffer(Desc, nullptr, &Staging);
        Context->CopyBuffer(Source, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Staging, 0, Desc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->WaitForIdle();
        void* Mapped = nullptr;
        Context->MapBuffer(Staging, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Mapped);
        Require(Mapped != nullptr, "Cannot map results");
        std::vector<Uint32> Result(static_cast<size_t>(Desc.Size / 4));
        std::memcpy(Result.data(), Mapped, static_cast<size_t>(Desc.Size));
        Context->UnmapBuffer(Staging, MAP_READ);
        return Result;
    };
    RTXPTLightFeedback Feedback;
    Require(Feedback.Initialize(Device, Factory, nullptr, Width, Height), "Cannot compile production feedback stages");
    Require(Feedback.SetLightCount(Device, 8), "Cannot allocate feedback counts");
    const Uint32               GlobalLight = 0;
    auto                       Proxies     = Buffer(4, 4, &GlobalLight);
    std::array<float, Pixels>  Weights{}, Depths{};
    std::array<Uint32, Pixels> Candidates{};
    std::array<float4, Pixels> Motions{};
    Weights.fill(1.0f);
    Depths.fill(1.0f);
    auto Depth  = Texture(TEX_FORMAT_R32_FLOAT, 4, Depths.data());
    auto Motion = Texture(TEX_FORMAT_RGBA32_FLOAT, 16, Motions.data());
    for (Uint32 Case = 0; Case < 8; ++Case)
    {
        LightingControlData Control{};
        Control.TotalLightCount         = 8;
        Control.HistoricTotalLightCount = 8;
        Control.SamplingProxyCount      = 1;
        Control.LocalSamplingResolution = uint2{3, 3};
        auto ControlBuffer              = Buffer(sizeof(Control), sizeof(Control), &Control);
        auto Local                      = Buffer(Entries * 4, 4, nullptr);
        Candidates.fill(RTXPT_INVALID_LIGHT_INDEX);
        auto RawWeights    = Texture(TEX_FORMAT_R32_FLOAT, 4, Weights.data());
        auto RawCandidates = Texture(TEX_FORMAT_R32_UINT, 4, Candidates.data());
        Require(Feedback.Prepare(Context, ControlBuffer, Proxies, Local, RawWeights, RawCandidates,
                                 Depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), Motion->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), true),
                "Reset prepare");
        Require(Feedback.Finish(Context, Local), "Reset finish");
        for (Uint32 Packed : Read(Local)) Require(Packed == 127u, "Reset must fill the only global light with multiplicity 128");
        Candidates.fill(7u | (Case == 1 ? 0u : 0x80000000u));
        RawWeights    = Texture(TEX_FORMAT_R32_FLOAT, 4, Weights.data());
        RawCandidates = Texture(TEX_FORMAT_R32_UINT, 4, Candidates.data());
        Depths.fill(Case == 2 ? 4.0f : 1.0f);
        auto CurrentDepth = Texture(TEX_FORMAT_R32_FLOAT, 4, Depths.data());
        Motions.fill(Case == 3 ? float4{100, 0, 0, 0} : float4{});
        auto CurrentMotion = Texture(TEX_FORMAT_RGBA32_FLOAT, 16, Motions.data());
        std::array<Uint32, 8> Remap{0, 1, 2, 3, 4, 5, 6, Case == 6 ? ~0u : 3u};
        auto                  RemapBuffer = Buffer(sizeof(Remap), 4, Remap.data());
        if (Case == 7)
        {
            Candidates.fill(RTXPT_INVALID_LIGHT_INDEX);
            RawCandidates = Texture(TEX_FORMAT_R32_UINT, 4, Candidates.data());
            std::vector<Uint32> OldLocal(Entries, (7u << 9u) | 127u);
            Context->UpdateBuffer(Local, 0, OldLocal.size() * 4u, OldLocal.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        if (Case == 4)
        {
            Control.SamplingProxyCount = 0;
            Context->UpdateBuffer(ControlBuffer, 0, sizeof(Control), &Control, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        Require(Feedback.Prepare(Context, ControlBuffer, Proxies, Local, RawWeights, RawCandidates,
                                 CurrentDepth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), CurrentMotion->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE), false, Case >= 5 ? RemapBuffer.RawPtr() : nullptr),
                "History prepare");
        const auto Counts = Read(Feedback.GetCounts());
        const Uint32 CountIndex = Case < 5 ? 7u : Case == 5 ? 3u : 8u;
        Require(Counts[CountIndex] == Pixels, "Feedback counts must follow source remapping and reject removed IDs");
        Require(Feedback.Finish(Context, Local), "History finish");
        const Uint32 Expected      = (Case == 0 ? 7u << 9 : Case == 5 ? 3u << 9 : 0u) | 127u;
        bool         FoundRemapped = false;
        const auto   LocalEntries  = Read(Local);
        for (Uint32 Tile = 0; Tile < Tiles; ++Tile)
        {
            std::array<Uint32, 8> Histogram{};
            for (Uint32 I = 0; I < 128; ++I)
            {
                const Uint32 Index = LocalEntries[Tile * 128 + I] >> 9;
                Require(Index < Histogram.size(), "Local table contains an invalid current light ID");
                ++Histogram[Index];
                if (I != 0) Require((LocalEntries[Tile * 128 + I - 1] >> 9) <= Index, "Local table must be sorted");
            }
            for (Uint32 I = 0; I < 128; ++I)
            {
                const Uint32 Packed = LocalEntries[Tile * 128 + I];
                Require(Histogram[Packed >> 9] == (Packed & 511u) + 1u, "Local table multiplicity must equal the histogram used by PDF queries");
            }
        }
        for (Uint32 Packed : LocalEntries)
        {
            if (Case != 7) Require(Packed == Expected, "Local feedback / disocclusion / empty table result differs from expected");
            else
            {
                const Uint32 Index = Packed >> 9;
                Require(Index == 0 || Index == 3, "Historic local-table fallback used an unremapped light ID");
                FoundRemapped |= Index == 3;
            }
        }
        if (Case == 7) Require(FoundRemapped, "Historic local-table fallback was not exercised");
        std::cout << "Feedback case " << Case << " passed\n";
    }
    RTXPTLightProxyBuildPass ProxyBuild;
    Require(ProxyBuild.Initialize(Device, Factory, nullptr), "Cannot initialize proxy jobs");
    for (Uint32 Case = 0; Case < 8; ++Case)
    {
        const Uint32 LightCount = Case == 4 ? 70000u : 8u;
        const Uint32 Budget     = Case == 4 ? LightCount : 400000u;
        struct Quad
        {
            Uint32 X, Y, Dim, Padding;
            float4 RadianceAndWeight;
        };
        std::vector<Quad> Quads(LightCount);
        struct AnalyticLight
        {
            float4 ColorType, PositionRadius, DirectionRange, Shaping;
        };
        std::vector<AnalyticLight> AnalyticLights(LightCount);
        float                      WeightSum = 0.0f;
        std::vector<float>         BaseWeights(LightCount);
        for (Uint32 I = 0; I < LightCount; ++I)
        {
            float Power = Case == 0 ? 0.0f : Case == 2 && I == 0 ? 10000.0f : 1.0f;
            if (Case >= 5)
            {
                const float Radiance = float(I + 1);
                AnalyticLights[I]    = {float4{Radiance, Radiance, Radiance, Case == 5 ? 0.0f : 4.0f},
                                     float4{0, 0, 0, 0.5f}, float4{0, 0, 1, 0},
                                     Case == 7 ? float4{0.5f, 0.25f, 0, 0} : float4{-1, 0, 0, 0}};
                constexpr float Pi   = 3.14159265358979323846f;
                Power                = (Case == 5 ? Pi * Pi : 4.0f * Pi) * Radiance * (Case == 7 ? 0.21875f : 1.0f);
            }
            Quads[I].RadianceAndWeight.w = Power;
            BaseWeights[I]               = std::pow(Power, 0.8f);
            WeightSum += BaseWeights[I];
        }
        LightingControlData Control{};
        Control.TotalLightCount                    = LightCount;
        Control.EnvmapQuadNodeCount                = Case >= 5 ? 0u : LightCount;
        Control.AnalyticLightCount                 = Case >= 5 ? LightCount : 0u;
        Control.TotalMaxFeedbackCount              = Pixels;
        Control.LastFrameTemporalFeedbackAvailable = Case == 3;
        Control.GlobalFeedbackUseWeight            = 0.9f;
        auto                ControlBuffer          = Buffer(sizeof(Control), sizeof(Control), &Control);
        auto                Env                    = Buffer(LightCount * sizeof(Quad), sizeof(Quad), Quads.data());
        auto                Dummy                  = Buffer(64, 64, nullptr);
        auto                Analytic               = Buffer(LightCount * sizeof(AnalyticLight), sizeof(AnalyticLight), AnalyticLights.data());
        auto                LightWeights           = Buffer(LightCount * 4, 4, nullptr);
        auto                Counts                 = Buffer(LightCount * 4, 4, nullptr);
        auto                Table                  = Buffer(Budget * 4, 4, nullptr);
        std::vector<Uint32> FeedbackCounts(LightCount + 1u);
        FeedbackCounts[7]  = Pixels;
        auto HistoryCounts = Buffer(static_cast<Uint32>(FeedbackCounts.size()) * 4, 4, FeedbackCounts.data());
        Require(ProxyBuild.Build(Context, ControlBuffer, Analytic, Dummy, Env, HistoryCounts, LightWeights, Counts, Table,
                                 LightCount, Control.AnalyticLightCount, Budget, Case == 4 ? 0 : 2),
                "Proxy build failed");
        const auto   ActualControl = Read(ControlBuffer);
        const Uint32 ProxyCount    = ActualControl[4];
        Require(ProxyCount <= Budget, "Proxy capacity exceeded");
        const auto          ActualCounts = Read(Counts);
        const auto          ProxiesOut   = Read(Table);
        std::vector<Uint32> Histogram(LightCount);
        for (Uint32 I = 0; I < ProxyCount; ++I)
        {
            Require(ProxiesOut[I] < LightCount, "Invalid proxy index or unwritten job");
            ++Histogram[ProxiesOut[I]];
        }
        for (Uint32 I = 0; I < LightCount; ++I)
        {
            Require(Histogram[I] == ActualCounts[I], "Proxy histogram disagrees with sampling PDF counters");
            float Weight = BaseWeights[I];
            if (Case == 3) Weight = 0.1f * Weight + 0.9f * (I == 7 ? WeightSum : 0.0f);
            const Uint32 Expected = Weight <= 0.0f ? 0u : Case == 4 ? 1u : std::min(262143u, static_cast<Uint32>(std::ceil(float(Budget - LightCount) * Weight / WeightSum)));
            Require(Expected == 0u || Case == 4 ? Expected == ActualCounts[I] : std::abs(double(Expected) - ActualCounts[I]) <= 1.0,
                    "Proxy weight / feedback mixture differs");
        }
        std::cout << "Proxy job case " << Case << " passed\n";
    }
    {
        RTXPTLightProxyBuildPass Boost;
        Require(Boost.Initialize(Device, Factory, nullptr), "Cannot initialize importance boost");
        struct Quad
        {
            Uint32 X, Y, Dim, Pad;
            float4 RadianceAndWeight;
        };
        Quad Node{0, 0, 1, 0, float4{1, 1, 1, 1}};
        struct Light
        {
            float4 Color, Position, Direction, Shaping;
        };
        constexpr float     Pi               = 3.14159265358979323846f;
        const float         Radiance         = 1.0f / (4.0f * Pi);
        Light               Lights[]         = {{float4{Radiance, Radiance, Radiance, 4}, {}, {}, float4{-1, 0, 0, 0}},
                          {float4{Radiance, Radiance, Radiance, 4}, float4{-2.5f, 0, 0, 0}, {}, float4{-1, 0, 0, 0}}};
        const float         TriangleRadiance = 2.0f / Pi;
        const float4        Triangle[]       = {{0, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {TriangleRadiance, TriangleRadiance, TriangleRadiance, 0}};
        auto                Env              = Buffer(sizeof(Node), sizeof(Node), &Node);
        auto                Analytic         = Buffer(sizeof(Lights), sizeof(Light), Lights);
        auto                Triangles        = Buffer(sizeof(Triangle), sizeof(Triangle), Triangle);
        auto                WeightsBuffer = Buffer(16, 4, nullptr), Counts = Buffer(16, 4, nullptr), Table = Buffer(40000, 4, nullptr);
        const Uint32        Zeros[5]       = {};
        auto                FeedbackCounts = Buffer(sizeof(Zeros), 4, Zeros);
        Uint32              Map[]          = {0, 2, ~0u, 3};
        auto                Remap          = Buffer(sizeof(Map), 4, Map);
        LightingControlData Control{};
        Control.TotalLightCount                                   = 4;
        Control.EnvmapQuadNodeCount                               = 1;
        Control.AnalyticLightCount                                = 2;
        Control.TriangleLightCount                                = 1;
        Control.BakerConstants.ImportanceBoostFrustumMul          = 8;
        Control.BakerConstants.ImportanceBoostFrustumFadeDistance = 5;
        Control.BakerConstants.ImportanceBoostIntensityDelta      = 64;
        Control.BakerConstants.FrustumPlanes[0]                   = float4{1, 0, 0, 0};
        const float4 FramePlanes[5]                               = {float4{1, 0, 0, 0}, {}, {}, {}, {}};
        auto         ControlBuffer                                = Buffer(sizeof(Control), sizeof(Control), &Control);
        for (Uint32 Frame = 0; Frame < 3; ++Frame)
        {
            if (Frame == 1)
            {
                Node.RadianceAndWeight.w = std::pow(2.0f, 1.0f / 0.8f);
                Lights[0].Position.x     = -10.0f;
                Lights[1].Position.x     = 0.0f;
                Context->UpdateBuffer(Env, 0, sizeof(Node), &Node, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                Context->UpdateBuffer(Analytic, 0, sizeof(Lights), Lights, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                Control.LastFrameTemporalFeedbackAvailable = 1;
            }
            Control.BakerConstants.FrustumPlanes[0] = Frame == 1 ? float4{} : FramePlanes[0];
            Context->UpdateBuffer(ControlBuffer, 0, sizeof(Control), &Control, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            Require(Boost.Build(Context, ControlBuffer, Analytic, Triangles, Env, FeedbackCounts, WeightsBuffer, Counts, Table, 4, 2, 10000, 2, Frame == 1 ? Remap.RawPtr() : nullptr, Frame == 1 ? FramePlanes : nullptr), "Importance boost build");
            const auto Bits = Read(WeightsBuffer);
            float      Actual[4];
            std::memcpy(Actual, Bits.data(), sizeof(Actual));
            const float Expected[3][4] = {{5, 9, 5, 9}, {298, 1, 585, 9}, {10, 1, 9, 9}};
            for (Uint32 I = 0; I < 4; ++I)
                Require(std::abs(Expected[Frame][I] - Actual[I]) < 0.002f, "Frustum / intensity delta / historical boosted weight mismatch");
        }
        std::cout << "Frustum and intensity boost checks passed\n";
    }
    // Source identity survives table reordering and motion, but not deletion/rebirth
    // or primitive topology changes. Environment nodes use the production GPU LUT map.
    {
        std::vector<RTXPTLightSourceRange> Old = {{{1, 10, 0, 0}, 2, 2}, {{1, 20, 0, 0}, 4, 1}, {{1, 30, 0, 0}, 5, 1}};
        std::vector<RTXPTLightSourceRange> Now = {{{1, 20, 0, 0}, 2, 1}, {{1, 10, 0, 0}, 3, 2}, {{1, 40, 0, 0}, 5, 1}};
        std::vector<Uint32>                Forward, Reverse;
        BuildLightHistoryMaps(Now, Old, 6, 6, Forward, Reverse);
        Require(Forward == std::vector<Uint32>({~0u, ~0u, 4, 2, 3, ~0u}), "CPU source forward mapping");
        Require(Reverse == std::vector<Uint32>({~0u, ~0u, 3, 4, 2, ~0u}), "CPU source reverse mapping");
        auto ChangedTopology     = Now;
        ChangedTopology[1].Count = 1;
        BuildLightHistoryMaps(ChangedTopology, Old, 6, 6, Forward, Reverse);
        Require(Forward[3] == ~0u && Reverse[2] == ~0u && Reverse[3] == ~0u, "Changed topology must invalidate the old triangle range");
        struct Quad
        {
            Uint32 X, Y, Dim, Pad;
            float4 Radiance;
        };
        const Quad                 Nodes[]   = {{0, 0, 2, 0, {}}, {1, 0, 2, 0, {}}};
        const Quad                 Swapped[] = {Nodes[1], Nodes[0]};
        auto                       Env       = Buffer(sizeof(Nodes), sizeof(Quad), Nodes);
        auto                       NewEnv    = Buffer(sizeof(Swapped), sizeof(Quad), Swapped);
        std::array<Uint32, Pixels> Lookup{}, NewLookup{};
        for (Uint32 I = 0; I < Pixels; ++I)
        {
            Lookup[I]    = I % Width < Width / 2 ? 0 : 1;
            NewLookup[I] = 1 - Lookup[I];
        }
        auto                Map    = Texture(TEX_FORMAT_R32_UINT, 4, Lookup.data());
        auto                NewMap = Texture(TEX_FORMAT_R32_UINT, 4, NewLookup.data());
        LightingControlData Control{};
        auto                ControlBuffer = Buffer(sizeof(Control), sizeof(Control), &Control);
        RTXPTLightHistory   History;
        Require(History.Initialize(Device, Factory, nullptr), "Cannot initialize history mapping");
        History.SetSources(Old, 6, 2);
        Require(History.Prepare(Context, ControlBuffer, Env, Map->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)), "Initial history mapping");
        for (Uint32 Index : Read(History.GetCurrentToPast())) Require(Index == ~0u, "First frame must not read undefined history");
        Require(History.Finish(Context, Env, Map->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)), "Snapshot history");
        History.SetSources(Now, 6, 2);
        Require(History.Prepare(Context, ControlBuffer, NewEnv, NewMap->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)), "Changed history mapping");
        Require(Read(History.GetCurrentToPast()) == std::vector<Uint32>({1, 0, 4, 2, 3, ~0u}), "GPU source/environment forward mapping");
        Require(Read(History.GetPastToCurrent()) == std::vector<Uint32>({1, 0, 3, 4, 2, ~0u}), "GPU source/environment reverse mapping");
        Require(History.Finish(Context, NewEnv, NewMap->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)), "Snapshot changed history");
        Require(History.Prepare(Context, ControlBuffer, NewEnv, NewMap->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)), "Stable frame mapping");
        Require(Read(History.GetPastToCurrent()) == std::vector<Uint32>({0, 1, 2, 3, 4, 5}), "Mapping must be consumed once, then become identity");
        Require(History.Finish(Context, NewEnv, NewMap->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)), "Snapshot stable history");
        History.SetSources(Old, 6, 2);
        Require(History.Prepare(Context, ControlBuffer, Env, Map->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)), "Reappearing source mapping");
        Require(Read(History.GetCurrentToPast())[5] == ~0u, "Reappearing source must not use stale history from two frames ago");
        std::cout << "Source and environment history checks passed\n";
    }
    Context->WaitForIdle();
    Require(Errors == 0, "Graphics validation reported errors");
    std::cout << "All production feedback checks passed\n";
    return 0;
}
catch (const std::exception& Error)
{
    std::cerr << Error.what() << '\n';
    return 1;
}
