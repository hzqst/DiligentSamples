// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// Production pixel mapping/compaction; independent coverage and bounds expectations.
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "RefCntAutoPtr.hpp"
#include "../src/RTXPTFrameConstants.hpp"
#include "../src/RTXPTRealtimeSettings.hpp"
using namespace Diligent;
static std::atomic<unsigned>   Errors{0};
static void DILIGENT_CALL_TYPE Message(DEBUG_MESSAGE_SEVERITY Severity, const Char* Text, const Char*, const Char*, int)
{
    if (Severity >= DEBUG_MESSAGE_SEVERITY_ERROR) ++Errors;
    if (Severity >= DEBUG_MESSAGE_SEVERITY_WARNING) std::cerr << Text << '\n';
}
static void Require(bool OK, const char* Text)
{
    if (!OK) throw std::runtime_error{Text};
}
int main(int argc, char** argv)
try
{
    Require(argc == 4, "SubSamplingTest <D3D12|Vk> <engine DLL> <shader root>");
    const bool  Vk  = std::string{argv[1]} == "Vk";
    std::string DLL = argv[2];
    SetDllDirectoryA(DLL.substr(0, DLL.find_last_of("/\\")).c_str());
    auto Lib = LoadLibraryA(argv[2]);
    Require(Lib != nullptr, "engine load");
    RefCntAutoPtr<IRenderDevice>  Device;
    RefCntAutoPtr<IDeviceContext> Context;
    IEngineFactory*               Factory = nullptr;
    if (Vk)
    {
        auto Get = reinterpret_cast<IEngineFactoryVk* (*)()>(GetProcAddress(Lib, "GetEngineFactoryVk"));
        Require(Get != nullptr, "Vk factory");
        auto* F = Get();
        Factory = F;
        F->SetMessageCallback(Message);
        EngineVkCreateInfo CI;
        CI.EnableValidation = true;
        F->CreateDeviceAndContextsVk(CI, &Device, &Context);
    }
    else
    {
        auto Get = reinterpret_cast<IEngineFactoryD3D12* (*)()>(GetProcAddress(Lib, "GetEngineFactoryD3D12"));
        Require(Get != nullptr, "D3D12 factory");
        auto* F = Get();
        Factory = F;
        F->SetMessageCallback(Message);
        EngineD3D12CreateInfo CI;
        CI.EnableValidation = true;
        F->CreateDeviceAndContextsD3D12(CI, &Device, &Context);
    }
    Require(Device && Context, "device creation");
    RTXPTRealtimeSettings Settings;
    Settings.RealtimeSubSamplingMode = 3;
    Require(Settings.ActualSubSamplingMode() == 3, "quarter mode");
    Settings.RealtimeSamplesPerPixel = 2;
    Require(Settings.ActualSubSamplingMode() == 0, "multi-spp fallback");
    Settings.RealtimeSamplesPerPixel = 1;
    Settings.RealtimeMode            = false;
    Require(Settings.ActualSubSamplingMode() == 0, "Reference fallback");
    Settings.RealtimeSubSamplingMode      = -1;
    Settings.RealtimeSubSamplingPattern   = 99;
    Settings.RealtimeSubSamplingBlockSize = 7;
    SanitizeRealtimeSettings(Settings);
    Require(Settings.RealtimeSubSamplingMode == 0 && Settings.RealtimeSubSamplingPattern == 3 && Settings.RealtimeSubSamplingBlockSize == 8, "settings bounds");
    auto Buffer = [&](Uint64 Bytes, bool CB = false) {
        BufferDesc D;
        D.Name      = "SubSampling regression buffer";
        D.Size      = Bytes;
        D.BindFlags = CB ? BIND_UNIFORM_BUFFER : BIND_UNORDERED_ACCESS;
        if (!CB) D.Mode = BUFFER_MODE_RAW;
        RefCntAutoPtr<IBuffer> B;
        Device->CreateBuffer(D, nullptr, &B);
        Require(B != nullptr, "buffer");
        return B;
    };
    auto                                           FrameCB = Buffer(sizeof(SampleConstants), true), MiniCB = Buffer(sizeof(SampleMiniConstants), true);
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    Factory->CreateDefaultShaderSourceStreamFactory(argv[3], &Source);
    const char* MappingSource = R"(
#include "PathTracer/PathTracerShared.h"
ConstantBuffer<SampleConstants> g_Const;
ConstantBuffer<SampleMiniConstants> g_MiniConst;
RWByteAddressBuffer u_SubSamplingPixelList;
RWByteAddressBuffer u_Counts;
#include "PathTracer/SubSampling.h"
[numthreads(8,8,1)] void main(uint2 tid : SV_DispatchThreadID)
{
    if (any(tid >= g_MiniConst.params3.xy)) return;
    uint2 pixel = tid;
    if (g_MiniConst.params1.z > 1 && !SubSampling_ThreadToPixel(tid, g_MiniConst.params1.xy, g_MiniConst.params1.w,
        g_MiniConst.params2.x, g_MiniConst.params2.y, SubSampling_FrameSeed(), g_Const.ptConsts.camera.ViewportSize, pixel)) return;
    if (any(pixel >= g_Const.ptConsts.camera.ViewportSize)) { u_Counts.InterlockedAdd(0, 1); return; }
    u_Counts.InterlockedAdd(4 + 4 * (pixel.x + pixel.y * g_Const.ptConsts.camera.ViewportSize.x), 1);
})";
    auto        Pipeline      = [&](bool Compact) {
        ShaderCreateInfo S;
        S.Desc                       = {"SubSampling regression", SHADER_TYPE_COMPUTE, false};
        S.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        S.ShaderCompiler             = SHADER_COMPILER_DXC;
        S.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
        S.HLSLVersion                = {6, 0};
        S.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
        S.pShaderSourceStreamFactory = Source;
        S.EntryPoint                 = "main";
        if (Compact) S.FilePath = "PathTracer/SubSamplingCompaction.hlsl";
        else
            S.Source = MappingSource;
        RefCntAutoPtr<IShader> Shader;
        Device->CreateShader(S, &Shader);
        Require(Shader != nullptr, "shader compile");
        ComputePipelineStateCreateInfo P;
        P.PSODesc.Name                               = S.Desc.Name;
        P.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
        P.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        P.pCS                                        = Shader;
        RefCntAutoPtr<IPipelineState> PSO;
        Device->CreateComputePipelineState(P, &PSO);
        Require(PSO != nullptr, "PSO");
        return PSO;
    };
    auto CompactPSO = Pipeline(true), MapPSO = Pipeline(false);
    auto Read = [&](IBuffer* B) {
        BufferDesc D;
        D.Name           = "SubSampling readback";
        D.Size           = B->GetDesc().Size;
        D.Usage          = USAGE_STAGING;
        D.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<IBuffer> Staging;
        Device->CreateBuffer(D, nullptr, &Staging);
        Context->CopyBuffer(B, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Staging, 0, D.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->Flush();
        Context->WaitForIdle();
        void* Data = nullptr;
        Context->MapBuffer(Staging, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
        Require(Data != nullptr, "readback");
        std::vector<Uint32> Result(static_cast<size_t>(D.Size / 4));
        std::memcpy(Result.data(), Data, static_cast<size_t>(D.Size));
        Context->UnmapBuffer(Staging, MAP_READ);
        return Result;
    };
    Uint32 Cases = 0;
    for (auto Size : {uint2{1, 1}, uint2{3, 2}, uint2{13, 9}, uint2{32, 32}})
        for (Uint32 Mode = 0; Mode <= 3; ++Mode)
            for (Uint32 Pattern = 0; Pattern < 4; ++Pattern)
                for (Uint32 Block : {4u, 8u, 16u})
                {
                    if ((Pattern < 2 && Block != 4) || (Mode == 0 && (Pattern != 0 || Block != 4))) continue;
                    const Uint32        W = Size.x, H = Size.y, N = W * H;
                    const Uint32        SX = Mode ? 2 : 1, SY = Mode == 1 || Mode == 3 ? 2 : 1, Num = Mode == 1 ? 3 : 1, Den = SX * SY;
                    auto                List = Buffer(16 + 4 * N + 16), Counts = Buffer(4 + 4 * N);
                    std::vector<Uint32> Poison(N + 8, 0xDEADBEEF), Zero(N + 1, 0), Sum(N, 0);
                    Context->UpdateBuffer(List, 0, Poison.size() * 4, Poison.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                    SampleConstants Frame{};
                    Frame.ptConsts.camera.ViewportWidth  = W;
                    Frame.ptConsts.camera.ViewportHeight = H;
                    for (Uint32 Seed = 0; Seed < Den; ++Seed)
                    {
                        Frame.ptConsts.sampleBaseIndex = Seed;
                        SampleMiniConstants Mini{};
                        Mini.params1 = uint4{SX, SY, Den, Num};
                        Mini.params2 = uint4{Pattern, Block, 0, 0};
                        Uint32 X = W, Y = H;
                        if (Mode && Pattern == 0)
                        {
                            X = (W + SX - 1) / SX * Num;
                            Y = (H + SY - 1) / SY;
                        }
                        if (Mode && Pattern >= 2)
                        {
                            X = (W + Block - 1) / Block * (Block * Block * Num / Den);
                            Y = (H + Block - 1) / Block;
                        }
                        Mini.params3 = uint4{X, Y, 0, 0};
                        Context->UpdateBuffer(FrameCB, 0, sizeof(Frame), &Frame, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                        Context->UpdateBuffer(MiniCB, 0, sizeof(Mini), &Mini, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                        Context->UpdateBuffer(List, 0, 4, Zero.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                        Context->UpdateBuffer(Counts, 0, Zero.size() * 4, Zero.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                        auto Dispatch = [&](IPipelineState* PSO, Uint32 DX, Uint32 DY) {
                            RefCntAutoPtr<IShaderResourceBinding> SRB;
                            PSO->CreateShaderResourceBinding(&SRB, true);
                            auto Set = [&](const char* Name, IDeviceObject* Object) {
                                if (auto* V = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name)) V->Set(Object);
                            };
                            Set("g_Const", FrameCB);
                            Set("g_MiniConst", MiniCB);
                            Set("u_SubSamplingPixelList", List->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
                            Set("u_Counts", Counts->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
                            StateTransitionDesc Barrier{List, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE};
                            Context->TransitionResourceStates(1, &Barrier);
                            Context->SetPipelineState(PSO);
                            Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                            Context->DispatchCompute(DispatchComputeAttribs{(DX + 7) / 8, (DY + 7) / 8, 1});
                        };
                        if (Mode && Pattern == 1) Dispatch(CompactPSO, W, H);
                        Dispatch(MapPSO, X, Y);
                        auto Data = Read(Counts);
                        Require(Data[0] == 0, "mapped pixel out of bounds");
                        Uint32 Count = 0;
                        for (Uint32 I = 0; I < N; ++I)
                        {
                            Require(Data[I + 1] <= 1, "duplicate pixel");
                            Sum[I] += Data[I + 1];
                            Count += Data[I + 1];
                        }
                        if (W == 32 && H == 32 && Pattern != 1) Require(Count == N * Num / Den, "exact block rate");
                        if (!Mode) Require(Count == N, "full-rate identity");
                        auto Packed = Read(List);
                        for (Uint32 I = N + 4; I < N + 8; ++I) Require(Packed[I] == 0xDEADBEEF, "compaction overrun");
                        if (Mode && Pattern == 1)
                        {
                            Require(Packed[0] == Count, "every compacted entry must be traced");
                            // Independent scalar implementation of the published per-pixel coin flip.
                            for (Uint32 I = 0; I < N; ++I)
                            {
                                Uint32 R = (((I % W) << 16) | (I / W)) ^ 0x9e3779b9u;
                                R ^= Seed + (R << 6) + (R >> 2);
                                R ^= 0x53554253u + (R << 6) + (R >> 2);
                                R ^= R >> 16;
                                R *= 0x21f0aaad;
                                R ^= R >> 15;
                                R *= 0xf35a2d97;
                                R ^= R >> 15;
                                bool Selected = double(R >> 8) * Den < double(Num) * (1u << 24);
                                Require(Data[I + 1] == Uint32(Selected), "compaction selection oracle");
                            }
                        }
                    }
                    if (Pattern == 3 || !Mode)
                        for (auto Count : Sum) Require(Count == Num, "Sobol cycle coverage/energy");
                    ++Cases;
                }
    Require(Errors == 0, "graphics validation errors");
    std::cout << "PASS: " << Cases << " rate/pattern/size cases, Sobol cycles, identity, compaction oracle, bounds, fallback\n";
    return 0;
}
catch (const std::exception& E)
{
    std::cerr << "FAIL: " << E.what() << '\n';
    return 1;
}
