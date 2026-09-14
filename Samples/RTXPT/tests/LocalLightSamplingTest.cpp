// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
// Production local-light table GPU regression; see README.md.
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#include "../../../../DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "../src/RTXPTFrameConstants.hpp"

using namespace Diligent;
using uint     = Uint32;
using float3x4 = std::array<float4, 3>;
#include "../shaders/PathTracer/Lighting/LightingTypes.hlsli"
static_assert(sizeof(LightingControlData) == 576, "Production lighting control layout");

namespace
{
std::atomic<unsigned>   Errors{0};
void DILIGENT_CALL_TYPE Message(DEBUG_MESSAGE_SEVERITY Severity, const Char* Text, const Char*, const Char*, int)
{
    if (Severity >= DEBUG_MESSAGE_SEVERITY_ERROR)
        ++Errors;
    if (Severity >= DEBUG_MESSAGE_SEVERITY_WARNING)
        std::cerr << Text << '\n';
}
void Require(bool Value, const char* Text)
{
    if (!Value)
        throw std::runtime_error{Text};
}
RefCntAutoPtr<IBuffer> Buffer(IRenderDevice* Device, const char* Name, Uint64 Size, Diligent::BIND_FLAGS Bind, const void* Data = nullptr, Uint32 Stride = sizeof(Uint32))
{
    BufferDesc Desc;
    Desc.Name              = Name;
    Desc.Size              = Size;
    Desc.BindFlags         = Bind;
    Desc.Mode              = BUFFER_MODE_STRUCTURED;
    Desc.ElementByteStride = Stride;
    BufferData             Init{Data, Size};
    RefCntAutoPtr<IBuffer> Result;
    Device->CreateBuffer(Desc, Data ? &Init : nullptr, &Result);
    Require(Result != nullptr, "Cannot create test buffer");
    return Result;
}
std::vector<Uint32> Read(IDeviceContext* Context, IRenderDevice* Device, IBuffer* Source)
{
    BufferDesc Desc;
    Desc.Name           = "Local light readback";
    Desc.Size           = Source->GetDesc().Size;
    Desc.Usage          = USAGE_STAGING;
    Desc.CPUAccessFlags = CPU_ACCESS_READ;
    RefCntAutoPtr<IBuffer> Staging;
    Device->CreateBuffer(Desc, nullptr, &Staging);
    Context->CopyBuffer(Source, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Staging, 0, Desc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->Flush();
    Context->WaitForIdle();
    void* Data = nullptr;
    Context->MapBuffer(Staging, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
    Require(Data != nullptr, "Readback unavailable");
    std::vector<Uint32> Result(static_cast<size_t>(Desc.Size / sizeof(Uint32)));
    std::memcpy(Result.data(), Data, static_cast<size_t>(Desc.Size));
    Context->UnmapBuffer(Staging, MAP_READ);
    return Result;
}
} // namespace

int main(int Argc, char** Argv)
try
{
    Require(Argc == 3, "Usage: LocalLightSamplingTest <engine DLL> <shader root>");
    const std::string DLL   = Argv[1];
    const auto        Slash = DLL.find_last_of("/\\");
    if (Slash != std::string::npos) SetDllDirectoryA(DLL.substr(0, Slash).c_str());
    HMODULE Library = LoadLibraryA(Argv[1]);
    Require(Library != nullptr, "Cannot load engine");
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
        auto Vulkan = reinterpret_cast<IEngineFactoryVk* (*)()>(GetProcAddress(Library, "GetEngineFactoryVk"));
        Require(Vulkan != nullptr, "Missing factory");
        Factory = Vulkan();
        Factory->SetMessageCallback(Message);
        EngineVkCreateInfo CI;
        CI.EnableValidation = true;
        Vulkan()->CreateDeviceAndContextsVk(CI, &Device, &Context);
    }
    Require(Device && Context, "Cannot create test device");
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    std::string                                    Search = std::string{Argv[2]} + ";" + Argv[2] + "/PathTracer";
    Factory->CreateDefaultShaderSourceStreamFactory(Search.c_str(), &Source);
    ShaderCreateInfo SCI;
    SCI.Desc                       = {"Local light table regression", SHADER_TYPE_COMPUTE, false};
    SCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    SCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    SCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    SCI.FilePath                   = "PathTracer/Lighting/LightsBaker.hlsl";
    SCI.EntryPoint                 = "FillLocalSamplingCS";
    SCI.pShaderSourceStreamFactory = Source;
    RefCntAutoPtr<IShader> Shader;
    Device->CreateShader(SCI, &Shader);
    Require(Shader != nullptr, "Producer compilation failed");
    ComputePipelineStateCreateInfo PCI;
    PCI.PSODesc.Name                               = "Local light producer test";
    PCI.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
    PCI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    PCI.pCS                                        = Shader;
    RefCntAutoPtr<IPipelineState> PSO;
    Device->CreateComputePipelineState(PCI, &PSO);
    Require(PSO != nullptr, "Producer pipeline failed");

    SCI.FilePath   = nullptr;
    SCI.EntryPoint = "main";
    SCI.Source     = R"(
#include "PathTracer/Lighting/LightSampler.hlsli"
RWStructuredBuffer<uint4> u_Results;
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x > 256u || tid.y >= 4u) return;
    uint2 pixel = uint2(tid.y % 2u, tid.y / 2u) * 8u;
    float samplePdf;
    uint sampled = SampleLocalLightIndex(pixel, (float(tid.x % 128u) + 0.5) / 128.0, samplePdf);
    u_Results[tid.y * 257u + tid.x] = uint4(asuint(SampleLocalPDF(pixel, tid.x)),
        asuint(SampleGlobalPDF(tid.x)), sampled, asuint(samplePdf));
})";
    Shader.Release();
    Device->CreateShader(SCI, &Shader);
    Require(Shader != nullptr, "Production PDF consumer compilation failed");
    PCI.pCS = Shader;
    RefCntAutoPtr<IPipelineState> QueryPSO;
    Device->CreateComputePipelineState(PCI, &QueryPSO);
    Require(QueryPSO != nullptr, "PDF consumer pipeline failed");

    // Multiple tiles, a partial edge tile, duplicate/unique lights, extreme counts,
    // coherent feedback and equal-weight tie-breaking. Compare to a CPU histogram,
    // independent of the GPU's sorting/counting algorithm.
    for (Uint32 Case = 0; Case < 5; ++Case)
    {
        constexpr Uint32    Width = 13, Height = 9, TilesX = 2, TilesY = 2;
        constexpr Uint32    Entries = RTXPT_LIGHTING_LOCAL_PROXY_COUNT;
        std::vector<Uint32> Proxies;
        for (Uint32 I = 0; I < (Case == 0 ? 1u : Case == 1 ? 128u : 19u); ++I)
            Proxies.push_back(Case == 0 ? 7u : Case == 1 ? (127u - I) * 2u : (I * 7u) % 9u);
        LightingControlData Control{};
        Control.TotalLightCount                   = 256;
        Control.SamplingProxyCount                = Case == 4 ? 0u : static_cast<Uint32>(Proxies.size());
        Control.LocalSamplingResolution           = uint2{TilesX, TilesY};
        Control.ImportanceSamplingType            = 2;
        Control.LocalToGlobalSampleRatio          = Case == 0 ? 0.0f : Case == 1 ? 1.0f : 0.65f;
        Control.BakerConstants.FeedbackResolution = uint2{Width, Height};
        auto                ControlBuffer         = Buffer(Device, "Control", sizeof(Control), BIND_SHADER_RESOURCE, &Control, sizeof(Control));
        auto                ProxyBuffer           = Buffer(Device, "Proxies", Proxies.size() * sizeof(Uint32), BIND_SHADER_RESOURCE, Proxies.data());
        std::vector<Uint32> Initial(TilesX * TilesY * Entries, 0xDEADBEEFu);
        auto                Output = Buffer(Device, "Local table", Initial.size() * sizeof(Uint32), BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE, Initial.data());
        std::vector<float>  Weights(Width * Height, 0);
        std::vector<Uint32> Candidates(Width * Height, RTXPT_INVALID_LIGHT_INDEX);
        if (Case == 2 || Case == 3)
        {
            Weights[1] = Weights[2]    = 3;
            Candidates[1]              = 5u | 0x80000000u;
            Candidates[2]              = 6u;
            Weights[Width * 8 + 12]    = 4;
            Candidates[Width * 8 + 12] = Case == 3 ? RTXPT_INVALID_LIGHT_INDEX : 4u;
        }
        auto Texture = [&](const char* Name, TEXTURE_FORMAT Format, const void* Data) {
            TextureDesc Desc;
            Desc.Name      = Name;
            Desc.Type      = RESOURCE_DIM_TEX_2D;
            Desc.Width     = Width;
            Desc.Height    = Height;
            Desc.Format    = Format;
            Desc.BindFlags = BIND_SHADER_RESOURCE;
            TextureSubResData       Sub{Data, Width * sizeof(Uint32)};
            TextureData             Init{&Sub, 1};
            RefCntAutoPtr<ITexture> Result;
            Device->CreateTexture(Desc, &Init, &Result);
            Require(Result != nullptr, "Feedback texture failed");
            return Result;
        };
        auto                                  WeightTexture    = Texture("Weights", TEX_FORMAT_R32_FLOAT, Weights.data());
        auto                                  CandidateTexture = Texture("Candidates", TEX_FORMAT_R32_UINT, Candidates.data());
        RefCntAutoPtr<IShaderResourceBinding> SRB;
        PSO->CreateShaderResourceBinding(&SRB, true);
        SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_LightingControl")->Set(ControlBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_LightSamplingProxies")->Set(ProxyBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_FeedbackTotalWeight")->Set(WeightTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_FeedbackCandidates")->Set(CandidateTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_LocalSamplingBuffer")->Set(Output->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Context->SetPipelineState(PSO);
        Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        DispatchComputeAttribs Dispatch;
        Dispatch.ThreadGroupCountX = TilesX;
        Dispatch.ThreadGroupCountY = TilesY;
        Context->DispatchCompute(Dispatch);
        auto                Actual = Read(Context, Device, Output);
        std::vector<Uint32> ProxyCounts(Control.TotalLightCount, 0u);
        for (Uint32 Light : Proxies)
            ++ProxyCounts[Light];
        auto       Counters = Buffer(Device, "Proxy counts", ProxyCounts.size() * sizeof(Uint32), BIND_SHADER_RESOURCE, ProxyCounts.data());
        BufferDesc ResultDesc;
        ResultDesc.Name              = "PDF results";
        ResultDesc.Size              = TilesX * TilesY * 257u * sizeof(uint4);
        ResultDesc.ElementByteStride = sizeof(uint4);
        ResultDesc.Mode              = BUFFER_MODE_STRUCTURED;
        ResultDesc.BindFlags         = BIND_UNORDERED_ACCESS;
        RefCntAutoPtr<IBuffer> Results;
        Device->CreateBuffer(ResultDesc, nullptr, &Results);
        RefCntAutoPtr<IShaderResourceBinding> QuerySRB;
        QueryPSO->CreateShaderResourceBinding(&QuerySRB, true);
        QuerySRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_LightingControl")->Set(ControlBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        QuerySRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_LightProxyCounters")->Set(Counters->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        QuerySRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_LocalSamplingBuffer")->Set(Output->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        QuerySRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Results")->Set(Results->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Context->SetPipelineState(QueryPSO);
        Context->CommitShaderResources(QuerySRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Dispatch.ThreadGroupCountX = (257u + 63u) / 64u;
        Dispatch.ThreadGroupCountY = TilesX * TilesY;
        Context->DispatchCompute(Dispatch);
        const auto PDFs = Read(Context, Device, Results);
        auto       Near = [](float Expected, Uint32 Bits) {
            float Value;
            std::memcpy(&Value, &Bits, sizeof(Value));
            Require(std::abs(Expected - Value) < 1e-6f, "Incorrect conditional light PDF");
        };
        for (Uint32 TY = 0; TY < TilesY; ++TY)
            for (Uint32 TX = 0; TX < TilesX; ++TX)
            {
                const Uint32 Base     = (TY * TilesX + TX) * Entries;
                Uint32       Feedback = RTXPT_INVALID_LIGHT_INDEX;
                float        Best     = 0;
                for (Uint32 Y = TY * 8; Y < std::min(Height, (TY + 1) * 8); ++Y)
                    for (Uint32 X = TX * 8; X < std::min(Width, (TX + 1) * 8); ++X)
                    {
                        Uint32 Candidate = Candidates[Y * Width + X] & 0x7FFFFFFFu;
                        if ((Candidates[Y * Width + X] & 0x80000000u) != 0u && Candidate < Control.TotalLightCount && Weights[Y * Width + X] > Best)
                        {
                            Best     = Weights[Y * Width + X];
                            Feedback = Candidate;
                        }
                    }
                std::map<Uint32, Uint32> Expected;
                for (Uint32 I = 0; I < Entries; ++I)
                    ++Expected[Control.SamplingProxyCount == 0 ? 0u : Feedback != RTXPT_INVALID_LIGHT_INDEX && I < 8 ? Feedback : Proxies[I % Proxies.size()]];
                Uint32 Index = Base;
                for (const auto& Light : Expected)
                    for (Uint32 I = 0; I < Light.second; ++I, ++Index)
                    {
                        Require(Light.first == (Actual[Index] >> 9), "Local table is not sorted or changed the sampling distribution");
                        Require(Light.second == (Actual[Index] & 511u) + 1u, "Packed local multiplicity is incorrect");
                    }
                for (Uint32 Light = 0; Light <= Control.TotalLightCount; ++Light)
                {
                    const Uint32 PDFBase   = ((TY * TilesX + TX) * 257u + Light) * 4u;
                    const float  LocalPDF  = static_cast<float>(Expected[Light]) / Entries;
                    const float  GlobalPDF = Control.SamplingProxyCount == 0 || Light >= Control.TotalLightCount ? 0.0f :
                                                                                                                  static_cast<float>(ProxyCounts[Light]) / Control.SamplingProxyCount;
                    Near(LocalPDF, PDFs[PDFBase]);
                    Near(GlobalPDF, PDFs[PDFBase + 1]);
                    const Uint32 Sampled = PDFs[PDFBase + 2];
                    Require((Actual[Base + Light % Entries] >> 9) == Sampled, "Sampling changed the selected table entry");
                    Near(static_cast<float>(Expected[Sampled]) / Entries, PDFs[PDFBase + 3]);
                }
            }
        std::cout << "Local light producer case " << Case << " passed\n";
    }
    // PDF-only evaluation must match the original combined BSDF/PDF function for
    // diffuse, metallic, rough/delta dielectric and transmission configurations.
    SCI.Source = R"(
#include "PathTracer/Lighting/LightSampler.hlsli"
#include "PathTracer/Lighting/LightSamplingMIS.hlsli"
RWStructuredBuffer<uint4> u_Contract;
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint n = tid.x;
    if (n >= 128) return;
    LightingControlData ctrl = (LightingControlData)0;
    ctrl.ImportanceSamplingType = 2;
    ctrl.LocalToGlobalSampleRatio = float(n % 5) * 0.25;
    uint localCount, globalCount;
    GetCandidateSampleCounts(ctrl, (n & 1) != 0, n / 2, localCount, globalCount);
    float pdf = float(n % 7) * 0.25;
    float lightMIS = LightVsBSDFMISApprox(pdf, 5, 1, true);
    float bsdfMIS = LightVsBSDFMISApprox(pdf, 5, 1, false);
    u_Contract[n] = uint4(localCount, globalCount, asuint(lightMIS), asuint(bsdfMIS));
    StandardBSDFData d = (StandardBSDFData)0;
    d.N = float3(0,0,1);
    d.diffuse = 0.5;
    d.specular = 0.04;
    d.transmission = 0.8;
    d.roughness = float(n % 4) / 3.0;
    d.alpha = d.roughness * d.roughness;
    d.eta = (n & 1) != 0 ? 1.5 : 1.0 / 1.5;
    d.metallic = (n % 3) == 0 ? 1 : 0;
    d.specularTransmission = (n % 3) == 1 ? 1 : 0;
    d.diffuseTransmission = (n % 3) == 2 ? 0.5 : 0;
    d.thinSurface = (n & 1) != 0;
    d.activeLobes = 0x3f;
    float3 wo = normalize(float3(0.2, 0.1, 1));
    float3 wi = normalize(float3(float(n % 5) - 2, 0.3, (n & 4) != 0 ? -1 : 1));
    float3 f;
    float fullPdf;
    EvalBSDF(d, wo, wi, 0, f, fullPdf);
    u_Contract[128 + n] = uint4(asuint(EvalBSDFPdf(d, wo, wi)), asuint(fullPdf),
        asuint(EvalLightSamplingMIS(1, 0.2, 1, 0.8)), asuint(EvalLightSamplingMIS(1, 0, 1, 0)));
    // Enumerate every candidate combination and reservoir outcome. The expected
    // integral of the two lights is 2 + 4, including disjoint local support.
    float pGlobal = 0.05 + 0.9 * float(n % 17) / 16.0;
    float pLocal = float(n % 3) * 0.5;
    float expectedLight = 0.0;
    for (uint mask = 0u; mask < 32u; ++mask)
    {
        float tupleProbability = 1.0;
        float weights[5], densities[5];
        uint lights[5];
        float weightSum = 0.0;
        for (uint i = 0u; i < 5u; ++i)
        {
            lights[i] = (mask >> i) & 1u;
            float p = i < 3u ? pGlobal : pLocal;
            densities[i] = lights[i] == 0u ? p : 1.0 - p;
            tupleProbability *= densities[i];
            weights[i] = densities[i] > 0.0 ? (lights[i] == 0u ? 2.0 : 4.0) / densities[i] : 0.0;
            weightSum += weights[i];
        }
        if (tupleProbability == 0.0) continue;
        for (uint i = 0u; i < 5u; ++i)
        {
            float pOther = i < 3u ? pLocal : pGlobal;
            float otherPdf = lights[i] == 0u ? pOther : 1.0 - pOther;
            float probability = weights[i] / weightSum;
            float outerMIS = EvalLightSamplingMIS(1, densities[i] + otherPdf, 1, 0.5);
            float contribution = weights[i] * EvalLightSamplingMIS(1, densities[i], 1, otherPdf) /
                                 (probability * (i < 3u ? 3.0 : 2.0)) * outerMIS;
            expectedLight += tupleProbability * probability * contribution;
        }
    }
    float expectedBSDF = 2.0 * EvalLightSamplingMIS(1, 0.5, 1, pGlobal + pLocal) +
                         4.0 * EvalLightSamplingMIS(1, 0.5, 1, 2.0 - pGlobal - pLocal);
    const float4 value = EvalBSDFValue(d, wo, wi);
    PolymorphicLightInfo spot = (PolymorphicLightInfo)0;
    spot.shaping = float4(0.5, 0.25, 0, 0);
    spot.directionRange = float4(0, 0, 1, 0);
    u_Contract[256 + n] = uint4(asuint(max(abs(value.x-f.x), max(abs(value.y-f.y), abs(value.z-f.z)))),
                               asuint(expectedLight + expectedBSDF), asuint(value.w),
                               asuint(EvalSpotFalloff(spot, float3(sqrt(1.0 - 0.625 * 0.625), 0, -0.625))));
})";
    Shader.Release();
    Device->CreateShader(SCI, &Shader);
    Require(Shader != nullptr, "Light sampling contract shader compilation failed");
    PCI.pCS = Shader;
    RefCntAutoPtr<IPipelineState> ContractPSO;
    Device->CreateComputePipelineState(PCI, &ContractPSO);
    Require(ContractPSO != nullptr, "Light sampling contract PSO failed");
    auto                                  ContractOutput = Buffer(Device, "Light sampling contract", 384 * sizeof(uint4), BIND_UNORDERED_ACCESS, nullptr, sizeof(uint4));
    RefCntAutoPtr<IShaderResourceBinding> ContractSRB;
    ContractPSO->CreateShaderResourceBinding(&ContractSRB, true);
    ContractSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Contract")->Set(ContractOutput->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    Context->SetPipelineState(ContractPSO);
    Context->CommitShaderResources(ContractSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DispatchComputeAttribs ContractDispatch;
    ContractDispatch.ThreadGroupCountX = 2;
    Context->DispatchCompute(ContractDispatch);
    const auto ContractResults = Read(Context, Device, ContractOutput);
    auto       FloatAt         = [&](size_t Index) {
        float Value;
        std::memcpy(&Value, &ContractResults[Index], sizeof(Value));
        Require(std::isfinite(Value), "Non-finite light sampling result");
        return Value;
    };
    for (Uint32 N = 0; N < 128; ++N)
    {
        const Uint32 Count = N / 2;
        const Uint32 Local = Count > 0 && (N & 1) ? std::min(Count - 1, Uint32((Count - 1) * (N % 5) * 0.25f + 0.75f)) : 0;
        Require(Local == ContractResults[N * 4] && Count - Local == ContractResults[N * 4 + 1], "Candidate count contract");
        const float Pdf = (N % 7) * 0.25f;
        Require(std::abs(15.0f / (15.0f + Pdf) - FloatAt(N * 4 + 2)) < 1e-6f, "Approximate light MIS");
        Require(std::abs(Pdf / (15.0f + Pdf) - FloatAt(N * 4 + 3)) < 1e-6f, "Approximate BSDF MIS");
        const size_t Base = (128 + N) * 4;
        Require(std::abs(FloatAt(Base + 1) - FloatAt(Base)) <= 1e-5f * std::max(1.0f, FloatAt(Base + 1)), "PDF-only BSDF evaluation differs");
        Require(std::abs(0.2f - FloatAt(Base + 2)) < 1e-6f && 0.0f == FloatAt(Base + 3), "Balance MIS / zero-density boundary");
        const size_t ValueBase = (256 + N) * 4;
        Require(FloatAt(ValueBase) <= 1e-6f, "Value-only BSDF evaluation differs");
        Require(std::abs(6.0f - FloatAt(ValueBase + 1)) < 1e-4f, "Stratified WRS / forward and reverse MIS changed the integral");
        Require(FloatAt(ValueBase + 2) >= 0.0f, "Invalid specular radiance average");
        Require(std::abs(0.5f - FloatAt(ValueBase + 3)) < 1e-6f, "Spotlight falloff differs from upstream smoothstep");
    }
    Require(Errors == 0, "Graphics validation errors");
    std::cout << "Local light sampling regression passed\n";
    return 0;
}
catch (const std::exception& Error)
{
    std::cerr << "FAILED: " << Error.what() << '\n';
    return 1;
}
