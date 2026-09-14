// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "../src/RTXPTFrameConstants.hpp"
#include "../src/RTXPTEnvironmentLights.hpp"
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
void Require(bool Condition, const char* Text)
{
    if (!Condition) throw std::runtime_error{Text};
}
struct Quad
{
    Uint32 X, Y, Dim, Padding;
    float4 RadianceAndWeight;
};
static_assert(sizeof(Quad) == 32, "EnvironmentQuadLight buffer layout");
} // namespace

int main(int Argc, char** Argv)
try
{
    Require(Argc == 2, "Run from the RTXPT sample directory: EnvironmentLightSamplingTest <engine DLL>");
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
    RTXPTEnvironmentLights Baker;
    Require(Baker.Initialize(Device, Factory, nullptr), "Cannot initialize production environment baker");
    constexpr Uint32 Resolution = 128, MipCount = 8;
    for (Uint32 Case = 0; Case < 3; ++Case)
    {
        std::vector<std::vector<float4>> Mips(MipCount);
        Mips[0].resize(Resolution * Resolution);
        for (Uint32 Y = 0; Y < Resolution; ++Y)
            for (Uint32 X = 0; X < Resolution; ++X)
            {
                const float Value           = Case == 0 ? 1.0f : Case == 2 ? 0.0f : (X == 91 && Y == 17 ? 10000.0f : 0.01f);
                Mips[0][Y * Resolution + X] = float4{Value, Value, Value, Value};
            }
        for (Uint32 M = 1; M < MipCount; ++M)
        {
            const Uint32 D = Resolution >> M;
            Mips[M].resize(D * D);
            for (Uint32 Y = 0; Y < D; ++Y)
                for (Uint32 X = 0; X < D; ++X)
                {
                    float4 Sum{};
                    for (Uint32 DY = 0; DY < 2; ++DY)
                        for (Uint32 DX = 0; DX < 2; ++DX)
                            Sum += Mips[M - 1][(Y * 2 + DY) * D * 2 + X * 2 + DX];
                    Mips[M][Y * D + X] = Sum * 0.25f;
                }
        }
        std::array<TextureSubResData, MipCount> Subresources{};
        for (Uint32 M = 0; M < MipCount; ++M)
            Subresources[M] = TextureSubResData{Mips[M].data(), Uint64{Resolution >> M} * sizeof(float4)};
        TextureDesc TexDesc;
        TexDesc.Name  = "Environment test radiance pyramid";
        TexDesc.Type  = RESOURCE_DIM_TEX_2D;
        TexDesc.Width = TexDesc.Height = Resolution;
        TexDesc.MipLevels              = MipCount;
        TexDesc.Format                 = TEX_FORMAT_RGBA32_FLOAT;
        TexDesc.BindFlags              = BIND_SHADER_RESOURCE;
        TextureData             Data{Subresources.data(), MipCount};
        RefCntAutoPtr<ITexture> Radiance;
        Device->CreateTexture(TexDesc, &Data, &Radiance);
        Require(Radiance != nullptr, "Cannot create radiance pyramid");
        LightingControlData Control{};
        Control.EnvmapQuadNodeCount                             = RTXPT_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT;
        Control.BakerConstants.EnvMapImportanceMapResolution    = Resolution;
        Control.BakerConstants.EnvMapImportanceMapMIPCount      = MipCount;
        Control.BakerConstants.DistantVsLocalRelativeImportance = 0.0002f;
        Control.BakerConstants.EnvMapParams.Enabled             = 1;
        Control.BakerConstants.EnvMapParams.ColorMultiplier     = float3{1, 0.5f, 2};
        BufferDesc ControlDesc;
        ControlDesc.Name = "Environment test control";
        ControlDesc.Size = ControlDesc.ElementByteStride = sizeof(Control);
        ControlDesc.Mode                                 = BUFFER_MODE_STRUCTURED;
        ControlDesc.BindFlags                            = BIND_SHADER_RESOURCE;
        BufferData             ControlData{&Control, sizeof(Control)};
        RefCntAutoPtr<IBuffer> ControlBuffer;
        Device->CreateBuffer(ControlDesc, &ControlData, &ControlBuffer);
        Require(Baker.CreateResources(Device, Resolution), "Cannot allocate environment lights");
        Require(Baker.Build(Context, ControlBuffer, Radiance->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE)), "Environment build failed");

        BufferDesc ReadDesc;
        ReadDesc.Name           = "Environment quad readback";
        ReadDesc.Size           = Baker.GetLights()->GetDesc().Size;
        ReadDesc.Usage          = USAGE_STAGING;
        ReadDesc.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<IBuffer> ReadBuffer;
        Device->CreateBuffer(ReadDesc, nullptr, &ReadBuffer);
        Context->CopyBuffer(Baker.GetLights(), 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, ReadBuffer, 0, ReadDesc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        TextureDesc ReadTexDesc    = Baker.GetLookupSRV()->GetTexture()->GetDesc();
        ReadTexDesc.Name           = "Environment lookup readback";
        ReadTexDesc.Usage          = USAGE_STAGING;
        ReadTexDesc.BindFlags      = BIND_NONE;
        ReadTexDesc.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<ITexture> ReadTexture;
        Device->CreateTexture(ReadTexDesc, nullptr, &ReadTexture);
        CopyTextureAttribs Copy;
        Copy.pSrcTexture              = Baker.GetLookupSRV()->GetTexture();
        Copy.pDstTexture              = ReadTexture;
        Copy.SrcTextureTransitionMode = Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Context->CopyTexture(Copy);
        Context->Flush();
        Context->WaitForIdle();
        void* QuadData = nullptr;
        Context->MapBuffer(ReadBuffer, MAP_READ, MAP_FLAG_DO_NOT_WAIT, QuadData);
        Require(QuadData != nullptr, "Quad readback unavailable");
        std::vector<Quad> Actual(RTXPT_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT);
        std::memcpy(Actual.data(), QuadData, static_cast<size_t>(ReadDesc.Size));
        Context->UnmapBuffer(ReadBuffer, MAP_READ);
        MappedTextureSubresource LookupData;
        Context->MapTextureSubresource(ReadTexture, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, LookupData);
        Require(LookupData.pData != nullptr, "Lookup readback unavailable");

        // Independent serial priority splitting: compare every GPU node to the
        // upstream two-level construction, including quantized-weight ties.
        auto MipFor = [=](Uint32 Dim) {
            Uint32 M = 0;
            for (Uint32 D = Resolution; D > Dim; D >>= 1) ++M;
            return M;
        };
        auto Priority = [&](const Quad& Q, Uint32 Index, Uint32 Depth) {
            const Uint32 Mip    = MipFor(Q.Dim);
            float        Weight = std::max(0.0001f * Mip, float(1u << (Mip * 2)) * Mips[Mip][Q.Y * Q.Dim + Q.X].w);
            if (Mip <= Depth) Weight = 0;
            Uint32 Bits;
            std::memcpy(&Bits, &Weight, 4);
            Bits = 0x1fbd1df5u + (Bits >> 1);
            float Root;
            std::memcpy(&Root, &Bits, 4);
            return (std::min(Uint32(Root * 100 + 0.5f), 0xfffffu) << 12) | Index;
        };
        auto Split = [&](std::vector<Quad> Nodes, Uint32 Count, Uint32 Depth) {
            for (Uint32 S = 0; S < Count; ++S)
            {
                Uint32 Best = 0, BestPriority = 0;
                for (Uint32 I = 0; I < Nodes.size(); ++I)
                    if (Priority(Nodes[I], I, Depth) >= BestPriority)
                    {
                        Best         = I;
                        BestPriority = Priority(Nodes[I], I, Depth);
                    }
                const Quad Parent = Nodes[Best];
                for (Uint32 Child = 0; Child < 4; ++Child)
                {
                    Quad Q{Parent.X * 2 + Child % 2, Parent.Y * 2 + Child / 2, Parent.Dim * 2, 0, {}};
                    if (Child == 0) Nodes[Best] = Q;
                    else
                        Nodes.push_back(Q);
                }
            }
            return Nodes;
        };
        std::vector<Quad> Base;
        for (Uint32 I = 0; I < 16; ++I) Base.push_back(Quad{I / 4, I % 4, 4, 0, {}});
        Base = Split(Base, 24, 3);
        std::vector<Quad> Expected;
        for (const Quad& Q : Base)
        {
            const auto Leaves = Split(std::vector<Quad>{Q}, 20, 0);
            Expected.insert(Expected.end(), Leaves.begin(), Leaves.end());
        }
        std::vector<Uint32> Coverage(Resolution * Resolution, 0);
        double              Area = 0;
        for (Uint32 I = 0; I < Actual.size(); ++I)
        {
            const Quad& Q = Actual[I];
            Require(Expected[I].X == Q.X && Expected[I].Y == Q.Y && Expected[I].Dim == Q.Dim, "Quadtree split differs from upstream oracle");
            Require(Q.Dim <= Resolution && Q.Dim > 0 && Q.X < Q.Dim && Q.Y < Q.Dim, "Invalid environment node");
            const Uint32 Mip            = MipFor(Q.Dim);
            const float4 V              = Mips[Mip][Q.Y * Q.Dim + Q.X];
            const float  ExpectedWeight = float(1u << (Mip * 2)) * V.w * (3.5f / 3.0f) * 0.0002f;
            Require(std::abs(V.x - Q.RadianceAndWeight.x) < 1e-5f && std::abs(V.y * 0.5f - Q.RadianceAndWeight.y) < 1e-5f && std::abs(V.z * 2 - Q.RadianceAndWeight.z) < 1e-5f, "Environment radiance/color multiplier");
            Require(std::abs(ExpectedWeight - Q.RadianceAndWeight.w) <= 1e-5f * std::max(1.0f, ExpectedWeight), "Environment power weight");
            Area += 1.0 / double(Q.Dim * Q.Dim);
            const Uint32 Scale = Resolution / Q.Dim;
            for (Uint32 Y = Q.Y * Scale; Y < (Q.Y + 1) * Scale; ++Y)
                for (Uint32 X = Q.X * Scale; X < (Q.X + 1) * Scale; ++X)
                {
                    const auto* Row = reinterpret_cast<const Uint32*>(static_cast<const Uint8*>(LookupData.pData) + Y * LookupData.Stride);
                    Require(I == Row[X], "Environment direction lookup does not match the sampled quad");
                    ++Coverage[Y * Resolution + X];
                }
        }
        Context->UnmapTextureSubresource(ReadTexture, 0, 0);
        Require(std::abs(1.0 - Area) < 1e-12, "Quad solid angles do not cover the sphere");
        for (Uint32 Count : Coverage) Require(1 == Count, "Environment lookup has an overlap or a hole");
        std::cout << "Environment case " << Case << ": all 5368 nodes, power/radiance and lookup coverage passed\n";
    }
    Require(Baker.CreateResources(Device, 0) && Baker.GetLightCount() == 0, "Disabled environment must have no light candidates");
    Require(Errors == 0, "Graphics validation errors");
    return 0;
}
catch (const std::exception& Error)
{
    std::cerr << "FAILED: " << Error.what() << '\n';
    return 1;
}
