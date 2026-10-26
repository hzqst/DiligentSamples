// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#include <Windows.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "../src/AsyncPrefix.hpp"
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
    Require(argc == 3, "AsyncPrefixTest <D3D12|Vk> <engine DLL>");
    std::string DLL = argv[2];
    SetDllDirectoryA(DLL.substr(0, DLL.find_last_of("/\\")).c_str());
    auto Lib = LoadLibraryA(argv[2]);
    Require(Lib != nullptr, "engine load");
    RefCntAutoPtr<IRenderDevice>            Device;
    IDeviceContext*                         Raw[2] = {};
    std::vector<ImmediateContextCreateInfo> Contexts;
    if (std::string{argv[1]} == "Vk")
    {
        auto Get = reinterpret_cast<IEngineFactoryVk* (*)()>(GetProcAddress(Lib, "GetEngineFactoryVk"));
        Require(Get != nullptr, "Vk factory");
        auto* F = Get();
        F->SetMessageCallback(Message);
        EngineVkCreateInfo CI;
        CI.EnableValidation = true;
        CI.SetValidationLevel(VALIDATION_LEVEL_2);
        Require(AsyncPrefix::Configure(F, CI, Contexts, true), "second same-family queue unavailable");
        F->CreateDeviceAndContextsVk(CI, &Device, Raw);
    }
    else
    {
        auto Get = reinterpret_cast<IEngineFactoryD3D12* (*)()>(GetProcAddress(Lib, "GetEngineFactoryD3D12"));
        Require(Get != nullptr, "D3D12 factory");
        auto* F = Get();
        F->SetMessageCallback(Message);
        EngineD3D12CreateInfo CI;
        CI.GraphicsAPIVersion = {12, 0};
        Require(F->LoadD3D12(), "load D3D12");
        CI.EnableValidation = true;
        CI.SetValidationLevel(VALIDATION_LEVEL_2);
        Require(AsyncPrefix::Configure(F, CI, Contexts), "dedicated compute queue unavailable");
        F->CreateDeviceAndContextsD3D12(CI, &Device, Raw);
    }
    RefCntAutoPtr<IDeviceContext> Graphics, Compute;
    Graphics.Attach(Raw[0]);
    Compute.Attach(Raw[1]);
    Require(Device && Graphics && Compute, "device creation");
    AsyncPrefix Scheduler;
    Scheduler.Initialize(Device, Graphics, Compute);
    Require(Scheduler.IsAvailable(), "fences");
    auto Allocate = [&](const char* Name, Uint64 Size, bool Readback = false) {
        BufferDesc D;
        D.Name                 = Name;
        D.Size                 = Size;
        D.ImmediateContextMask = kRTXPTImmediateContextMask;
        D.Usage                = Readback ? USAGE_STAGING : USAGE_DEFAULT;
        D.CPUAccessFlags       = Readback ? CPU_ACCESS_READ : CPU_ACCESS_NONE;
        RefCntAutoPtr<IBuffer> B;
        Device->CreateBuffer(D, nullptr, &B);
        Require(B != nullptr, "buffer allocation");
        return B;
    };
    auto Early    = Allocate("prefix scratch", 4);
    auto Shared   = Allocate("late RR input", 4);
    auto Output   = Allocate("consumer history", 4 * 128);
    auto Readback = Allocate("result", 4 * 128, true);
    auto Common   = [](IDeviceContext* C, IBuffer* B) {
        StateTransitionDesc Barrier{B, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COMMON, STATE_TRANSITION_FLAG_UPDATE_STATE};
        C->TransitionResourceStates(1, &Barrier);
    };
    // A shared single-slot input must not be overwritten until the previous
    // graphics consumer finishes. No CPU wait occurs inside each 32-frame run.
    for (Uint32 Frame = 0; Frame < 128; ++Frame)
    {
        Scheduler.Begin();
        Uint32 Value = Frame * 7919 + 17;
        Compute->UpdateBuffer(Early, 0, 4, &Value, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Scheduler.JoinPreviousGraphics();
        Compute->CopyBuffer(Early, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Shared, 0, 4, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Common(Compute, Shared);
        Scheduler.Submit();
        Graphics->CopyBuffer(Shared, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Output, Frame * 4, 4, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Common(Graphics, Shared);
        Scheduler.EndGraphics();
        // Exercise switching away from async and back without resetting fences.
        if ((Frame + 1) % 32 == 0) Scheduler.Drain();
    }
    Graphics->CopyBuffer(Output, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Readback, 0, 4 * 128, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Graphics->Flush();
    Graphics->WaitForIdle();
    void* Data = nullptr;
    Graphics->MapBuffer(Readback, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
    Require(Data != nullptr, "map result");
    for (Uint32 Frame = 0; Frame < 128; ++Frame)
        Require(Frame * 7919 + 17 == static_cast<Uint32*>(Data)[Frame], "stale or overwritten cross-queue input");
    Graphics->UnmapBuffer(Readback, MAP_READ);
    Require(Errors == 0, "validation errors");
    std::cout << "PASS: 128 cross-queue frames, four drain/re-entry cycles\n";
    return 0;
}
catch (const std::exception& E)
{
    std::cerr << "FAIL: " << E.what() << '\n';
    return 1;
}
