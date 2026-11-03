// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// GPU regression for per-layer first-segment guides and NRD missing-sample packing.
#include <Windows.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "RefCntAutoPtr.hpp"
#include "../src/RTXPTFrameConstants.hpp"
#include "../src/RTXPTRenderTargets.hpp"

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

#include "NrdPrepareInputsTest.hpp"
#include "NrdHistoryConfidenceTest.hpp"

int main(int argc, char** argv)
try
{
    Require(argc == 4, "NrdHitDistanceTest <D3D12|Vk> <engine DLL> <shader search paths>");
    const std::string DLL = argv[2];
    SetDllDirectoryA(DLL.substr(0, DLL.find_last_of("/\\")).c_str());
    auto Lib = LoadLibraryA(argv[2]);
    Require(Lib != nullptr, "engine load");
    RefCntAutoPtr<IRenderDevice>  Device;
    RefCntAutoPtr<IDeviceContext> Context;
    IEngineFactory*               Factory = nullptr;
    if (std::string{argv[1]} == "Vk")
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
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    Factory->CreateDefaultShaderSourceStreamFactory(argv[3], &Source);
    RunNrdFrameTimeTest();
    RunNrdHistoryConfidenceTest(Device, Context, Source);
    const char*      Code = R"(
#define RTXPT_HAS_NRD_HEADERS 1
#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#include "PathTracer/DenoisingLayers.hlsli"
#include "PostProcessing/RTXPTDenoiserNRD.hlsli"
RWStructuredBuffer<DenoisingLayer> u_Layers;
RWStructuredBuffer<float4> u_Result;
[numthreads(1,1,1)] void main()
{
    DenoisingLayer a = (DenoisingLayer)0;
    DenoisingLayer b = (DenoisingLayer)0;
    a.SceneLength = 1000.0; // Must never enter the outgoing hit distance.
    a.HitDistance.Begin(kDenoisingHitDistanceDiffuse);
    a.HitDistance.AddSegment(2.0); // Rejected dielectric boundary: continue the same ray.
    a.HitDistance.AddSegment(3.0);
    a.HitDistance.Complete(false);
    a.HitDistance.Begin(kDenoisingHitDistanceDiffuse);
    a.HitDistance.AddSegment(9.0);
    a.HitDistance.Complete(false);
    b.HitDistance.Begin(kDenoisingHitDistanceSpecular);
    b.HitDistance.AddSegment(12.0);
    b.HitDistance.Complete(false);
    b.HitDistance.Begin(kDenoisingHitDistanceSpecular);
    b.HitDistance.AddSegment(4.0);
    b.HitDistance.Complete(false);
    u_Layers[0] = a;
    u_Layers[1] = b;
    float expectedSpec = NRD_FrontEnd_SpecHitDistAveraging_Begin();
    NRD_FrontEnd_SpecHitDistAveraging_Add(expectedSpec, 12.0);
    NRD_FrontEnd_SpecHitDistAveraging_Add(expectedSpec, 0.0);
    NRD_FrontEnd_SpecHitDistAveraging_Add(expectedSpec, 4.0);
    NRD_FrontEnd_SpecHitDistAveraging_End(expectedSpec);
    u_Result[0] = float4(a.HitDistance.DiffuseDistance, a.HitDistance.SpecularDistance,
                         b.HitDistance.SpecularDistance, expectedSpec);
    u_Result[1] = float4(a.HitDistance.DiffuseSamples, b.HitDistance.SpecularSamples,
                         a.HitDistance.Flags, b.HitDistance.Flags);
    b = (DenoisingLayer)0;
    b.HitDistance.Begin(kDenoisingHitDistanceSpecular);
    b.HitDistance.Complete(true);
    u_Result[2] = RTXPTDenoiserRelaxPackRadianceHitDist(0.0.xxx, b.HitDistance.SpecularDistance);
    b.HitDistance.Begin(kDenoisingHitDistanceDiffuse);
    b.HitDistance.AddSegment(asfloat(0x7fc00000));
    b.HitDistance.Complete(false);
    u_Result[3] = float4(b.HitDistance.DiffuseDistance, b.HitDistance.DiffuseSamples,
                         b.HitDistance.Flags, b.HitDistance.PendingLobe);
    u_Result[4] = float4(RTXPTDenoiserReblurGetNormHitDist(0.0, 5.0, 1.0),
                         RTXPTDenoiserReblurGetNormHitDist(7.0, 5.0, 1.0),
                         REBLUR_FrontEnd_GetNormHitDist(7.0, 5.0, float3(3.0, 0.1, 20.0), 1.0), 0.0);
    // A roulette/branch-cancelled sample must not leak its partial distance.
    b.HitDistance.Begin(kDenoisingHitDistanceDiffuse);
    b.HitDistance.AddSegment(99.0);
    b.HitDistance.Begin(0u);
    b.HitDistance.Complete(false);
    u_Result[5] = float4(b.HitDistance.DiffuseDistance, b.HitDistance.PendingDistance, 0.0, 0.0);
    b = (DenoisingLayer)0;
    b.HitDistance.Begin(kDenoisingHitDistanceSpecular);
    b.HitDistance.AddSegment(100000.0);
    b.HitDistance.Complete(false);
    u_Result[6] = float4(b.HitDistance.SpecularDistance,
        RTXPTDenoiserRelaxPackRadianceHitDist(0.0.xxx, b.HitDistance.SpecularDistance).w,
        RTXPTDenoiserReblurGetNormHitDist(b.HitDistance.SpecularDistance, 1000000.0, 1.0),
        REBLUR_FrontEnd_GetNormHitDist(100000.0, 1000000.0, float3(3.0, 0.1, 20.0), 1.0));
})";
    ShaderCreateInfo S;
    S.Desc                       = {"NRD hit distance regression", SHADER_TYPE_COMPUTE, false};
    S.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    S.ShaderCompiler             = SHADER_COMPILER_DXC;
    S.HLSLVersion                = {6, 0};
    S.pShaderSourceStreamFactory = Source;
    S.EntryPoint                 = "main";
    S.Source                     = Code;
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
    constexpr Uint32 LayerStride = sizeof(RTXPTDenoisingLayerData);
    constexpr Uint32 ResultCount = 7;
    auto             Buffer      = [&](Uint32 Stride, Uint32 Count) {
        BufferDesc D;
        D.Name              = "NRD regression buffer";
        D.Size              = Stride * Count;
        D.ElementByteStride = Stride;
        D.Mode              = BUFFER_MODE_STRUCTURED;
        D.BindFlags         = BIND_UNORDERED_ACCESS;
        RefCntAutoPtr<IBuffer> B;
        Device->CreateBuffer(D, nullptr, &B);
        Require(B != nullptr, "buffer creation");
        return B;
    };
    auto                                  Layers = Buffer(LayerStride, 2), Results = Buffer(sizeof(float) * 4, ResultCount);
    RefCntAutoPtr<IShaderResourceBinding> SRB;
    PSO->CreateShaderResourceBinding(&SRB, true);
    SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Layers")->Set(Layers->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Result")->Set(Results->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    Context->SetPipelineState(PSO);
    Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});
    BufferDesc D;
    D.Name           = "NRD regression readback";
    D.Size           = Results->GetDesc().Size;
    D.Usage          = USAGE_STAGING;
    D.CPUAccessFlags = CPU_ACCESS_READ;
    RefCntAutoPtr<IBuffer> Readback;
    Device->CreateBuffer(D, nullptr, &Readback);
    Context->CopyBuffer(Results, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Readback, 0, D.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->Flush();
    Context->WaitForIdle();
    void* Data = nullptr;
    Context->MapBuffer(Readback, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
    Require(Data != nullptr, "readback");
    float R[ResultCount][4];
    std::memcpy(R, Data, sizeof(R));
    Context->UnmapBuffer(Readback, MAP_READ);
    Require(R[0][0] == 7 && R[0][1] == 0 && R[0][2] == 4 && R[0][3] == 4, "per-layer lobe distances and NRD multi-sample minimum");
    Require(R[1][0] == 2 && R[1][1] == 2 && R[1][2] == 1 && R[1][3] == 4, "sample counts and hit flags");
    Require(R[2][3] == 65504, "environment miss survives RELAX sanitization");
    Require(R[3][0] == 0 && R[3][1] == 0 && R[3][2] == 24 && R[3][3] == 0, "invalid sample remains missing");
    Require(R[4][0] == 0 && R[4][1] > 0 && std::abs(R[4][1] - R[4][2]) < 1e-6f, "REBLUR preserves missing lobe and normalizes valid samples");
    Require(R[5][0] == 0 && R[5][1] == 0, "cancelled sample");
    Require(R[6][0] == 100000 && R[6][1] == 65504 && std::abs(R[6][2] - R[6][3]) < 1e-6f, "large geometry distances normalize before format clamping");
    RunNrdPrepareInputsTest(Device, Context, Source);
    Require(Errors == 0, "GPU validation errors");
    std::cout << "NRD hit distance GPU regression passed\n";
    return 0;
}
catch (const std::exception& E)
{
    std::cerr << E.what() << '\n';
    return 1;
}
