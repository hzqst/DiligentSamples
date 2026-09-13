// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
// Standalone GPU regression test. See README.md for build/run instructions.
#include <Windows.h>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "../../../../DiligentCore/Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#include "../../../../DiligentCore/Common/interface/RefCntAutoPtr.hpp"
#include "../../../../DiligentCore/Common/interface/Float16.hpp"
#include "../src/RTXPTFrameConstants.hpp"
#include "../src/RTXPTRenderTargets.hpp"

using namespace Diligent;

namespace
{
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

Uint32 Pack(float Low, float High)
{
    return Float16{Low}.Raw() | (Uint32{Float16{High}.Raw()} << 16);
}

using Plane = RTXPTStablePlaneData;
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
} // namespace

int main(int Argc, char** Argv)
try
{
    Require(Argc == 3, "Usage: RRPrepareInputsTest <GraphicsEngineD3D12 DLL> <RTXPT shaders directory>");
    HMODULE Library = LoadLibraryA(Argv[1]);
    Require(Library != nullptr, "Cannot load D3D12 engine DLL");
    auto GetFactory = reinterpret_cast<IEngineFactoryD3D12* (*)()>(GetProcAddress(Library, "GetEngineFactoryD3D12"));
    Require(GetFactory != nullptr, "Missing D3D12 factory");
    IEngineFactoryD3D12*          Factory = GetFactory();
    RefCntAutoPtr<IRenderDevice>  Device;
    RefCntAutoPtr<IDeviceContext> Context;
    EngineD3D12CreateInfo         EngineCI;
    EngineCI.EnableValidation = true;
    Factory->CreateDeviceAndContextsD3D12(EngineCI, &Device, &Context);
    Require(Device && Context, "Cannot create D3D12 device");

    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    Factory->CreateDefaultShaderSourceStreamFactory(Argv[2], &Source);
    ShaderMacro      Macros[] = {{"RTXPT_POST_PROCESS_MODE", "9"}, {"RTXPT_HAS_NRD_HEADERS", "0"}};
    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc                       = {"RR input regression", SHADER_TYPE_COMPUTE, false};
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.FilePath                   = "PostProcessing/RTXPTPostProcess.csh";
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.Macros                     = {Macros, 2};
    ShaderCI.pShaderSourceStreamFactory = Source;
    RefCntAutoPtr<IShader> Shader;
    Device->CreateShader(ShaderCI, &Shader);
    Require(Shader != nullptr, "RR shader compilation failed");
    ComputePipelineStateCreateInfo PSOCI;
    PSOCI.PSODesc.Name                               = "RR input regression";
    PSOCI.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
    PSOCI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    PSOCI.pCS                                        = Shader;
    RefCntAutoPtr<IPipelineState> PSO;
    Device->CreateComputePipelineState(PSOCI, &PSO);
    Require(PSO != nullptr, "RR pipeline creation failed");

    // One-pixel inputs deliberately dispatch an 8x8 group to exercise bounds checks.
    for (int Test = 0; Test < 10; ++Test)
    {
        SampleConstants Frame     = {};
        Frame.ptConsts.imageWidth = Frame.ptConsts.imageHeight = 1;
        Frame.ptConsts._activeStablePlaneCount                 = Test == 8 ? 0 : 3;
        Frame.ptConsts.genericTSLineStride                     = 8;
        Frame.ptConsts.genericTSPlaneStride                    = 64;
        Frame.ptConsts.DLSSRRBrightnessClampK                  = Test == 3 ? 2.0f : 0.0f;
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
        RefCntAutoPtr<IBuffer> StablePlanes;
        Device->CreateBuffer(SPDesc, &SPData, &StablePlanes);

        std::array<Uint32, 4> Header = {{Test < 2 ? 0xffffffffu : 1u, Test == 5 ? 1u : 0xffffffffu, 0xffffffffu, 0u}};
        if (Test == 6)
            Header[1] = 1;
        if (Test == 7)
            Header = {{0xffffffffu, 1u, 0xffffffffu, 3u}}; // missing primary and invalid dominant
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
        std::array<RefCntAutoPtr<ITexture>, 4> Outputs;
        for (auto& Output : Outputs)
            Device->CreateTexture(TexDesc, nullptr, &Output);

        RefCntAutoPtr<IShaderResourceBinding> SRB;
        PSO->CreateShaderResourceBinding(&SRB, true);
        const auto Bind = [&](const char* Name, IDeviceObject* Object) {
            Require(Object != nullptr, "Missing test resource");
            auto* Variable = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name);
            Require(Variable != nullptr, "Missing shader resource");
            Variable->Set(Object);
        };
        Bind("g_Const", Constants);
        Bind("u_StablePlanesBuffer", StablePlanes->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Bind("u_StablePlanesHeader", HeaderTexture->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_StableRadiance", StableRadiance->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        const char* Names[] = {"u_OutputColor", "u_RRDiffuseAlbedo", "u_RRSpecAlbedo", "u_RRNormalsAndRoughness"};
        for (size_t Index = 0; Index < Outputs.size(); ++Index)
            Bind(Names[Index], Outputs[Index]->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Context->SetPipelineState(PSO);
        Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});

        std::array<std::array<float, 4>, 4> Results;
        for (size_t Index = 0; Index < Outputs.size(); ++Index)
        {
            TextureDesc ReadDesc    = TexDesc;
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
            for (size_t Channel = 0; Channel < 4; ++Channel)
            {
                float Value = Float16::HalfBitsToFloat(static_cast<const Uint16*>(Mapped.pData)[Channel]);
                Require(std::isfinite(Value), "Non-finite RR output");
                Results[Index][Channel] = Value;
            }
            Context->UnmapTextureSubresource(Readback, 0, 0);
        }
        const auto& Normal = Results[3];
        Near(1, std::sqrt(Normal[0] * Normal[0] + Normal[1] * Normal[1] + Normal[2] * Normal[2]));
        Require(Normal[3] >= 0 && Normal[3] <= 1, "Roughness outside [0,1]");
        for (size_t Guide = 1; Guide <= 2; ++Guide)
            for (float Value : Results[Guide])
                Require(Value >= 0 && Value <= 1, "Albedo outside [0,1]");
        if (Test == 0)
        {
            Near(0, Results[0][0]);
            Near(0.05f, Results[1][0]);
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
        if (Test >= 8) Near(0, Results[0][0]);
        std::cout << "RR input case " << Test << " passed\n";
        Context->Flush();
        Context->FinishFrame();
    }
    Context->WaitForIdle();
    return 0;
}
catch (const std::exception& Error)
{
    std::cerr << Error.what() << '\n';
    return 1;
}
