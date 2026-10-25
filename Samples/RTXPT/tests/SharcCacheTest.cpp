// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// Exercise the production bindings/clear/resolve with independent radiance expectations.
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "RefCntAutoPtr.hpp"
#include "BasicMath.hpp"
using namespace Diligent;
using uint = Uint32;
#include "../shaders/PathTracer/SharcConstants.h"
static std::atomic<unsigned>   Errors{0};
static void DILIGENT_CALL_TYPE Message(DEBUG_MESSAGE_SEVERITY Severity, const Char* Text, const Char*, const Char*, int)
{
    if (Severity >= DEBUG_MESSAGE_SEVERITY_ERROR) ++Errors;
    if (Severity >= DEBUG_MESSAGE_SEVERITY_WARNING) std::cerr << Text << '\n';
}
static void Require(bool Ok, const char* Text)
{
    if (!Ok) throw std::runtime_error{Text};
}
int main(int argc, char** argv)
try
{
    Require(argc == 3, "SharcCacheTest <D3D12 engine DLL> <deployed shader root>");
    std::string DLL = argv[1];
    SetDllDirectoryA(DLL.substr(0, DLL.find_last_of("/\\")).c_str());
    auto Library = LoadLibraryA(argv[1]);
    Require(Library != nullptr, "engine load");
    auto GetFactory = reinterpret_cast<IEngineFactoryD3D12* (*)()>(GetProcAddress(Library, "GetEngineFactoryD3D12"));
    Require(GetFactory != nullptr, "D3D12 factory");
    auto* Factory = GetFactory();
    Factory->SetMessageCallback(Message);
    EngineD3D12CreateInfo CI;
    CI.EnableValidation                   = true;
    CI.Features.ShaderFloat16             = DEVICE_FEATURE_STATE_ENABLED;
    CI.Features.RayTracing                = DEVICE_FEATURE_STATE_ENABLED;
    CI.Features.ResourceBuffer16BitAccess = DEVICE_FEATURE_STATE_ENABLED;
    RefCntAutoPtr<IRenderDevice>  Device;
    RefCntAutoPtr<IDeviceContext> Context;
    Factory->CreateDeviceAndContextsD3D12(CI, &Device, &Context);
    Require(Device && Context, "device creation");
    auto Buffer = [&](const char* Name, Uint32 Size, Uint32 Stride, bool Uniform = false) {
        BufferDesc D;
        D.Name      = Name;
        D.Size      = Size;
        D.BindFlags = Uniform ? BIND_UNIFORM_BUFFER : BIND_UNORDERED_ACCESS;
        if (!Uniform)
        {
            D.Mode              = BUFFER_MODE_STRUCTURED;
            D.ElementByteStride = Stride;
        }
        RefCntAutoPtr<IBuffer> B;
        Device->CreateBuffer(D, nullptr, &B);
        Require(B != nullptr, "buffer creation");
        return B;
    };
    const Uint32   Capacity = RTXPT_SHARC_GROUP_SIZE * (RTXPT_SHARC_GROUPS_PER_ROW + 1);
    auto           Hash     = Buffer("Hash", Capacity * 8, 8);
    auto           Accum    = Buffer("Accum", Capacity * 16, 16);
    auto           Resolved = Buffer("Resolved", Capacity * 16, 16);
    auto           Result   = Buffer("Query result", 16, 16);
    auto           CB       = Buffer("Constants", sizeof(SharcConstants), 0, true);
    SharcConstants Constants{};
    Constants.EntriesNum           = Capacity;
    Constants.SceneScale           = 35.f;
    Constants.AccumulationFrameNum = 1;
    Constants.StaleFrameNum        = 8;
    Context->UpdateBuffer(CB, 0, sizeof(Constants), &Constants, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    const std::string                              Search = std::string{argv[2]} + ";" + argv[2] + "/PathTracer;" + argv[2] + "/SHARC";
    Factory->CreateDefaultShaderSourceStreamFactory(Search.c_str(), &Source);
    // Regression: the separate Reference hit implementation must not swallow training.
    // A syntactically valid RT shader with no cache writes is not a training shader.
    for (bool Train : {true, false})
    {
        ShaderMacro      Macros[] = {{"RTXPT_SHARC_TRAIN", Train ? "1" : "0"},
                                {"RTXPT_SHARC_CONSUME", Train ? "0" : "1"},
                                {"PATH_TRACER_MODE", Train ? "0" : "2"}};
        ShaderCreateInfo S;
        S.Desc                       = {"SHaRC production hit regression", SHADER_TYPE_RAY_CLOSEST_HIT, false};
        S.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        S.ShaderCompiler             = SHADER_COMPILER_DXC;
        S.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
        S.CompileFlags               = SHADER_COMPILE_FLAG_ENABLE_16BIT_TYPES;
        S.HLSLVersion                = {6, 6};
        S.Macros                     = {Macros, 3};
        S.FilePath                   = "PathTracer/PathTracerClosestHit.rchit";
        S.EntryPoint                 = "main";
        S.pShaderSourceStreamFactory = Source;
        RefCntAutoPtr<IShader> Shader;
        Device->CreateShader(S, &Shader);
        Require(Shader != nullptr, "production hit compilation");
        for (const char* Name : {"cbSharc", "u_SharcHash", "u_SharcResolved", "u_SharcAccumulation", "u_SharcPathState"})
        {
            if (!Train && (std::strcmp(Name, "u_SharcAccumulation") == 0 || std::strcmp(Name, "u_SharcPathState") == 0)) continue;
            bool Found = false;
            for (Uint32 I = 0; I < Shader->GetResourceCount(); ++I)
            {
                ShaderResourceDesc Resource;
                Shader->GetResourceDesc(I, Resource);
                Found |= std::strcmp(Name, Resource.Name) == 0;
            }
            Require(Found, Name);
        }
    }
    const char* TestSource = R"(
#define RTXPT_SHARC_TRAIN 1
#include "PathTracer/SharcBindings.hlsli"
RWStructuredBuffer<float4> u_Result;
SharcHitData Hit()
{
    SharcHitData hit = (SharcHitData)0;
    hit.positionWorld = float3(0, 0, 2);
    hit.normalWorld = float3(0, 0, -1);
    hit.materialDemodulation = 1.0.xxx;
    return hit;
}
[numthreads(1,1,1)] void Deposit()
{
    SharcState s; SharcInit(s);
    SharcUpdateHit(GetSharcParameters(), s, Hit(), float3(2, 3, 4), .5);
    SharcSetThroughput(s, float3(.5, .25, .125));
    SharcUpdateMiss(GetSharcParameters(), s, float3(4, 8, 16));
}
[numthreads(1,1,1)] void Query()
{
    float3 radiance = 0.0.xxx;
    bool found = SharcGetCachedRadiance(GetSharcParameters(), Hit(), radiance, false);
    u_Result[0] = float4(radiance, found ? 1.0 : 0.0);
}
[numthreads(1,1,1)] void SetTail()
{
    uint index = g_Sharc.EntriesNum - 1;
    u_SharcHash[index] = 1;
    SharcAccumulationData a = (SharcAccumulationData)0;
    a.data.x = 1;
    u_SharcAccumulation[index] = a;
    SharcPackedData r = (SharcPackedData)0;
    r.sampleData = 1;
    u_SharcResolved[index] = r;
}
[numthreads(1,1,1)] void ReadTail()
{
    uint index = g_Sharc.EntriesNum - 1;
    u_Result[0] = float4(u_SharcHash[index] != 0, u_SharcAccumulation[index].data.x != 0,
                        u_SharcResolved[index].sampleData != 0, 1);
})";
    auto        Barrier    = [&]() {
        for (IBuffer* B : {Hash.RawPtr(), Accum.RawPtr(), Resolved.RawPtr(), Result.RawPtr()})
        {
            StateTransitionDesc D{B, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE};
            Context->TransitionResourceStates(1, &D);
        }
    };
    auto Dispatch = [&](const char* Entry, bool Production) {
        ShaderCreateInfo S;
        S.Desc                       = {Entry, SHADER_TYPE_COMPUTE, false};
        S.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        S.ShaderCompiler             = SHADER_COMPILER_DXC;
        S.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
        S.HLSLVersion                = {6, 6};
        S.CompileFlags               = SHADER_COMPILE_FLAG_ENABLE_16BIT_TYPES;
        S.EntryPoint                 = Entry;
        S.pShaderSourceStreamFactory = Source;
        if (Production) S.FilePath = "PathTracer/SharcResolve.hlsl";
        else
            S.Source = TestSource;
        RefCntAutoPtr<IShader> Shader;
        Device->CreateShader(S, &Shader);
        Require(Shader != nullptr, "shader compile");
        ComputePipelineStateCreateInfo P;
        P.PSODesc.Name                               = Entry;
        P.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
        P.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
        P.pCS                                        = Shader;
        RefCntAutoPtr<IPipelineState> PSO;
        Device->CreateComputePipelineState(P, &PSO);
        Require(PSO != nullptr, "PSO creation");
        RefCntAutoPtr<IShaderResourceBinding> SRB;
        PSO->CreateShaderResourceBinding(&SRB, true);
        auto Set = [&](const char* Name, IDeviceObject* Object) {
            if (auto* V = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name)) V->Set(Object);
        };
        Set("cbSharc", CB);
        Set("u_SharcHash", Hash->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Set("u_SharcAccumulation", Accum->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Set("u_SharcResolved", Resolved->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Set("u_Result", Result->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Barrier();
        Context->SetPipelineState(PSO);
        Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        const Uint32 Groups = (Capacity + RTXPT_SHARC_GROUP_SIZE - 1) / RTXPT_SHARC_GROUP_SIZE;
        Context->DispatchCompute(Production ?
                                     DispatchComputeAttribs{std::min(Groups, Uint32{RTXPT_SHARC_GROUPS_PER_ROW}),
                                                            (Groups + RTXPT_SHARC_GROUPS_PER_ROW - 1) / RTXPT_SHARC_GROUPS_PER_ROW, 1} :
                                     DispatchComputeAttribs{1, 1, 1});
        Barrier();
    };
    auto Read = [&]() {
        BufferDesc D;
        D.Name           = "readback";
        D.Size           = 16;
        D.Usage          = USAGE_STAGING;
        D.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<IBuffer> B;
        Device->CreateBuffer(D, nullptr, &B);
        Context->CopyBuffer(Result, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, B, 0, 16, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->Flush();
        Context->WaitForIdle();
        void* Ptr = nullptr;
        Context->MapBuffer(B, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Ptr);
        Require(Ptr != nullptr, "readback");
        float4 Value;
        std::memcpy(&Value, Ptr, 16);
        Context->UnmapBuffer(B, MAP_READ);
        return Value;
    };
    Dispatch("Clear", true);
    Dispatch("Query", false);
    Require(Read().w == 0, "empty cache must miss");
    Dispatch("Deposit", false);
    Dispatch("Resolve", true);
    Dispatch("Query", false);
    const float4 Value = Read();
    std::cout << "Resolved radiance: " << Value.x << ',' << Value.y << ',' << Value.z << " hit=" << Value.w << '\n';
    Require(Value.w == 1 && std::abs(Value.x - 4) < .02f && std::abs(Value.y - 5) < .02f && std::abs(Value.z - 6) < .02f,
            "direct plus throughput-weighted environment radiance");
    for (int I = 0; I < 16; ++I) Dispatch("Resolve", true);
    Dispatch("Query", false);
    Require(Read().w == 0, "stale cache eviction");
    Dispatch("Deposit", false);
    Dispatch("Resolve", true);
    Dispatch("Clear", true);
    Dispatch("Query", false);
    Require(Read().w == 0, "explicit invalidation");
    Dispatch("SetTail", false);
    Dispatch("ReadTail", false);
    const auto Tail = Read();
    Require(Tail.x == 1 && Tail.y == 1 && Tail.z == 1, "tail test setup");
    Dispatch("Clear", true);
    Dispatch("ReadTail", false);
    const auto ClearedTail = Read();
    Require(ClearedTail.x == 0 && ClearedTail.y == 0 && ClearedTail.z == 0, "clear covers the last entry of the second dispatch row");
    Require(Errors == 0, "graphics validation errors");
    std::cout << "PASS: RT training/query resources, empty cache, propagation, resolve, stale eviction, invalidation, dispatch tail\n";
    return 0;
}
catch (const std::exception& E)
{
    std::cerr << "FAIL: " << E.what() << '\n';
    return 1;
}
