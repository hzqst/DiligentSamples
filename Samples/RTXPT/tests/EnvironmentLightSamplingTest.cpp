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
#include "../src/RTXPTLightPacking.hpp"
#include "../src/RTXPTEnvironmentLights.hpp"
#include "../src/RTXPTLightHistory.hpp"
#include "../src/RTXPTEnvCubeBakeConstants.hpp"
#include "Float16.hpp"
#include "ShaderMacroHelper.hpp"
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

//A 14-lux sun of 0.53 degrees, baked into one 2048-texel face.
constexpr Uint32 kBakeResolution            = 2048;
constexpr float  kTestSunIlluminance        = 14.0f;
constexpr float  kTestSunDiameterDegrees    = 0.53f;
constexpr double kMinBakedIlluminanceRatio  = 0.8;
constexpr double kMaxBakedIlluminanceRatio  = 1.1;
constexpr double kBakedEnergyRatioTolerance = 0.02;

//One bake: a single sun of Illuminance lux travelling along Direction, stored at RadianceScale, seen through the
//environment rows WorldToLocal0 and WorldToLocal2. DirectionalLightCount 0 disables it.
struct DirectionalBakeCase
{
    const char* Name                  = "normal";
    float       Illuminance           = kTestSunIlluminance;
    float3      Direction             = float3{0, 0, -1};
    float       RadianceScale         = kEnvironmentRadianceScale;
    Uint32      DirectionalLightCount = 1;
    float4      WorldToLocal0         = float4{1, 0, 0, 0};
    float4      WorldToLocal2         = float4{0, 0, 1, 0};
};

//The production cube-bake pipeline with a black source cube, writing one face that the CPU reads back.
struct DirectionalBakeResources
{
    RefCntAutoPtr<IPipelineState>         PSO;
    RefCntAutoPtr<IShaderResourceBinding> SRB;
    RefCntAutoPtr<IBuffer>                CB;
    RefCntAutoPtr<ITexture>               BlackCube;
    RefCntAutoPtr<ITexture>               Output;
    RefCntAutoPtr<ITexture>               Readback;
    RefCntAutoPtr<ISampler>               Sampler;
};

DirectionalBakeResources CreateDirectionalBakeResources(IRenderDevice* Device, IEngineFactory* Factory)
{
    DirectionalBakeResources                       R;
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    CreateRTXPTShaderSourceFactory(Factory, "shaders;shaders/PathTracer", &Source);
    ShaderMacroHelper Macros;
    Macros.Add("ENV_BAKE_SOURCE_CUBE", 1);
    ShaderCreateInfo SCI;
    SCI.Desc                       = {"Directional environment bake test", SHADER_TYPE_COMPUTE, false};
    SCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    SCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    SCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    SCI.FilePath                   = "PathTracer/Lighting/EnvMapImportanceBaker.hlsl";
    SCI.EntryPoint                 = "EnvCubeBakeCS";
    SCI.pShaderSourceStreamFactory = Source;
    SCI.Macros                     = Macros;
    RefCntAutoPtr<IShader> Shader;
    Device->CreateShader(SCI, &Shader);
    Require(Shader != nullptr, "Compile the production directional cube bake");
    ComputePipelineStateCreateInfo PCI;
    PCI.PSODesc.Name                               = "Directional environment bake test";
    PCI.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
    PCI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    PCI.pCS                                        = Shader;
    Device->CreateComputePipelineState(PCI, &R.PSO);
    Require(R.PSO != nullptr, "Create the directional cube-bake pipeline");
    R.PSO->CreateShaderResourceBinding(&R.SRB, true);

    BufferDesc CBDesc;
    CBDesc.Name      = "Directional bake test constants";
    CBDesc.Size      = sizeof(EnvCubeBakeConstantsCPU);
    CBDesc.BindFlags = BIND_UNIFORM_BUFFER;
    Device->CreateBuffer(CBDesc, nullptr, &R.CB);
    const float4                     Black{};
    std::array<TextureSubResData, 6> BlackFaces;
    for (auto& Face : BlackFaces) Face = TextureSubResData{&Black, sizeof(Black)};
    TextureDesc BlackDesc;
    BlackDesc.Name  = "Directional bake black source";
    BlackDesc.Type  = RESOURCE_DIM_TEX_CUBE;
    BlackDesc.Width = BlackDesc.Height = 1;
    BlackDesc.ArraySize                = 6;
    BlackDesc.Format                   = TEX_FORMAT_RGBA32_FLOAT;
    BlackDesc.BindFlags                = BIND_SHADER_RESOURCE;
    TextureData BlackData{BlackFaces.data(), static_cast<Uint32>(BlackFaces.size())};
    Device->CreateTexture(BlackDesc, &BlackData, &R.BlackCube);
    TextureDesc OutputDesc;
    OutputDesc.Name      = "Directional bake face";
    OutputDesc.Type      = RESOURCE_DIM_TEX_2D_ARRAY;
    OutputDesc.ArraySize = 1;
    OutputDesc.Width = OutputDesc.Height = kBakeResolution;
    OutputDesc.Format                    = TEX_FORMAT_RGBA16_FLOAT;
    OutputDesc.BindFlags                 = BIND_UNORDERED_ACCESS;
    Device->CreateTexture(OutputDesc, nullptr, &R.Output);
    OutputDesc.Name           = "Directional bake readback";
    OutputDesc.Usage          = USAGE_STAGING;
    OutputDesc.BindFlags      = BIND_NONE;
    OutputDesc.CPUAccessFlags = CPU_ACCESS_READ;
    Device->CreateTexture(OutputDesc, nullptr, &R.Readback);
    Device->CreateSampler(SamplerDesc{}, &R.Sampler);
    Require(R.CB && R.BlackCube && R.Output && R.Readback && R.Sampler, "Create directional bake test resources");

    R.SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "cbEnvCubeBake")->Set(R.CB);
    R.SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "g_EnvBakeSource")->Set(R.BlackCube->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    R.SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "g_EnvBakeSourceSampler")->Set(R.Sampler);
    R.SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "g_EnvBakeOutput")->Set(R.Output->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    return R;
}

//The illuminance the baked +Z face delivers to a receiver facing it: the radiance restored from its storage scale,
//integrated over each texel's solid angle times the receiver cosine.
double BakeDirectionalIlluminance(IDeviceContext* Context, const DirectionalBakeResources& R, const DirectionalBakeCase& Case)
{
    const float Diameter   = kTestSunDiameterDegrees * PI_F / 180.0f;
    const float SolidAngle = 2.0f * PI_F * (1.0f - std::cos(Diameter * 0.5f));
    const float Radiance   = Case.Illuminance / SolidAngle;

    AnalyticLightData Light;
    Light.colorType      = float4{Radiance, Radiance, Radiance, kPolymorphicLightTypeDirectional};
    Light.directionRange = float4{Case.Direction, 0};
    Light.shaping.w      = SolidAngle;
    EnvCubeBakeConstantsCPU C;
    C.Rotation              = float4x4::Identity();
    C.WorldToLocal0         = Case.WorldToLocal0;
    C.WorldToLocal1         = float4{0, 1, 0, 0};
    C.WorldToLocal2         = Case.WorldToLocal2;
    C.RadianceScale         = Case.RadianceScale;
    C.DirectionalLightCount = Case.DirectionalLightCount;
    C.CubeDim               = kBakeResolution;
    C.DirectionalLights[0]  = PackAnalyticLight(Light).Base;

    Context->UpdateBuffer(R.CB, 0, sizeof(C), &C, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->SetPipelineState(R.PSO);
    Context->CommitShaderResources(R.SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    constexpr Uint32 GroupCount = kBakeResolution / RTXPT_ENVMAP_BAKE_THREADS;
    Context->DispatchCompute(DispatchComputeAttribs{GroupCount, GroupCount, 1});
    CopyTextureAttribs Copy;
    Copy.pSrcTexture              = R.Output;
    Copy.pDstTexture              = R.Readback;
    Copy.SrcTextureTransitionMode = Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    Context->CopyTexture(Copy);
    Context->Flush();
    Context->WaitForIdle();

    MappedTextureSubresource Data;
    Context->MapTextureSubresource(R.Readback, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Data);
    Require(Data.pData != nullptr, "Read back the baked sun face");
    const auto BakedRadiance = [](Uint16 Bits) {
        const Float16 Half{Bits};
        const float   Value = static_cast<float>(Half);
        Require(!Half.Sign() && std::isfinite(Value), "Baked radiance is finite and nonnegative");
        return Value;
    };
    double Illuminance = 0;
    for (Uint32 Y = 0; Y < kBakeResolution; ++Y)
    {
        const auto*  Row = reinterpret_cast<const Uint16*>(static_cast<const Uint8*>(Data.pData) + Y * Data.Stride);
        const double V   = 2.0 * (Y + 0.5) / kBakeResolution - 1.0;
        for (Uint32 X = 0; X < kBakeResolution; ++X)
        {
            //A texel at (U, V) on the unit-distance face covers 4 / (Resolution^2 * (1 + U^2 + V^2)^1.5) steradians
            //and meets the receiver at cosine 1 / sqrt(1 + U^2 + V^2).
            const double U               = 2.0 * (X + 0.5) / kBakeResolution - 1.0;
            const double DistanceSquared = 1.0 + U * U + V * V;
            Illuminance += BakedRadiance(Row[X * 4]) * (4.0 / (double(kBakeResolution) * kBakeResolution * DistanceSquared * DistanceSquared)) / C.RadianceScale;
        }
    }
    Context->UnmapTextureSubresource(R.Readback, 0, 0);
    std::cout << "Directional bake " << Case.Name << ": integrated illuminance " << Illuminance << '\n';
    return Illuminance;
}

//Exercise the production cube-bake entry point independently of proxy selection and path-tracer MIS.
void TestDirectionalBake(IRenderDevice* Device, IDeviceContext* Context, IEngineFactory* Factory)
{
    const auto R = CreateDirectionalBakeResources(Device, Factory);

    const DirectionalBakeCase NormalCase;
    DirectionalBakeCase       DoubledCase = NormalCase;
    DoubledCase.Name                      = "doubled";
    DoubledCase.Illuminance               = 2.0f * kTestSunIlluminance;
    DoubledCase.RadianceScale             = kEnvironmentRadianceScale / 2.0f;
    DirectionalBakeCase DisabledCase      = NormalCase;
    DisabledCase.Name                     = "disabled";
    DisabledCase.DirectionalLightCount    = 0;
    DirectionalBakeCase OppositeCase      = NormalCase;
    OppositeCase.Name                     = "opposite";
    OppositeCase.Direction                = float3{0, 0, 1};
    //A quarter turn of the environment about +Y: the sun travelling along world -X reaches the same face.
    DirectionalBakeCase RotatedCase = NormalCase;
    RotatedCase.Name                = "rotated";
    RotatedCase.Direction           = float3{-1, 0, 0};
    RotatedCase.WorldToLocal0       = float4{0, 0, 1, 0};
    RotatedCase.WorldToLocal2       = float4{-1, 0, 0, 0};

    const double Normal   = BakeDirectionalIlluminance(Context, R, NormalCase);
    const double Doubled  = BakeDirectionalIlluminance(Context, R, DoubledCase);
    const double Disabled = BakeDirectionalIlluminance(Context, R, DisabledCase);
    const double Opposite = BakeDirectionalIlluminance(Context, R, OppositeCase);
    const double Rotated  = BakeDirectionalIlluminance(Context, R, RotatedCase);
    //Upstream's coverage approximation shaves the disk edge; allow that rasterization error, not a unit conversion.
    const bool NearInput = Normal > kTestSunIlluminance * kMinBakedIlluminanceRatio && Normal < kTestSunIlluminance * kMaxBakedIlluminanceRatio;
    Require(NearInput, "Baked sun irradiance agrees with its input illuminance");
    Require(std::abs(Doubled / Normal - 2.0) < kBakedEnergyRatioTolerance, "FP16 bake scaling preserves doubled solar energy");
    Require(Disabled == 0 && Opposite == 0, "Disabled and opposite-facing suns leave this face black");
    Require(std::abs(Rotated / Normal - 1.0) < kBakedEnergyRatioTolerance, "Environment rotation preserves the sun's world direction and energy");
}
} // namespace

int main(int Argc, char** Argv)
try
{
    Require(Argc == 2, "Run from the RTXPT sample directory: EnvironmentLightSamplingTest <engine DLL>");
    const std::string DLL        = Argv[1];
    const auto        Slash      = DLL.find_last_of("/\\");
    const std::string DXCompiler = Slash != std::string::npos ? DLL.substr(0, Slash + 1) + "dxcompiler.dll" : "dxcompiler.dll";
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
        CI.pDxCompilerPath  = DXCompiler.c_str();
        Vk()->CreateDeviceAndContextsVk(CI, &Device, &Context);
    }
    Require(Device && Context, "Cannot create device");
    TestDirectionalBake(Device, Context, Factory);
    RTXPTEnvironmentLights Baker;
    Require(Baker.Initialize(Device, Factory, nullptr, nullptr), "Cannot initialize production environment baker");
    RTXPTLightHistory History;
    Require(History.Initialize(Device, Factory, nullptr, nullptr), "Cannot initialize production history mapper");
    std::vector<Quad>   PreviousNodes;
    std::vector<Uint32> PreviousLookup;
    constexpr Uint32    Resolution = 128, MipCount = 8;
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
        Control.TotalLightCount                                 = Control.EnvmapQuadNodeCount;
        Control.BakerConstants.EnvMapImportanceMapResolution    = Resolution;
        Control.BakerConstants.EnvMapImportanceMapMIPCount      = MipCount;
        Control.BakerConstants.DistantVsLocalRelativeImportance = 0.0002f;
        Control.BakerConstants.EnvMapParams.Enabled             = 1;
        Control.BakerConstants.EnvMapParams.ColorMultiplier     = float3{1, 0.5f, 2};
        BufferDesc ControlDesc;
        ControlDesc.Name = "Environment test control";
        ControlDesc.Size = ControlDesc.ElementByteStride = sizeof(Control);
        ControlDesc.Mode                                 = BUFFER_MODE_STRUCTURED;
        ControlDesc.BindFlags                            = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
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
        const auto*       Packed = static_cast<const PolymorphicLightInfo*>(QuadData);
        Require(Baker.GetLights()->GetDesc().ElementByteStride == sizeof(PolymorphicLightInfo), "Packed environment stride");
        for (size_t I = 0; I < Actual.size(); ++I)
        {
            Require((Packed[I].ColorTypeAndFlags >> 24u & 15u) == 5u, "Environment light tag");
            const Uint32 Log   = Packed[I].LogRadiance & 0xffffu;
            const float  Scale = Log == 0 ? 0.0f : std::exp2(float(Log - 1u) / 65534.0f * 48.0f - 8.0f);
            float        Weight;
            std::memcpy(&Weight, &Packed[I].Scalars, sizeof(Weight));
            Actual[I] = Quad{Packed[I].Direction1 >> 16u, Packed[I].Direction1 & 0xffffu, Packed[I].Direction2 >> 16u, 0,
                             float4{float(Packed[I].ColorTypeAndFlags & 255u) / 255.0f * Scale,
                                    float(Packed[I].ColorTypeAndFlags >> 8u & 255u) / 255.0f * Scale,
                                    float(Packed[I].ColorTypeAndFlags >> 16u & 255u) / 255.0f * Scale, Weight}};
        }
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
        Base = Split(Base, 20, 3);
        std::vector<Quad> Expected;
        for (const Quad& Q : Base)
        {
            const auto Leaves = Split(std::vector<Quad>{Q}, 12, 0);
            Expected.insert(Expected.end(), Leaves.begin(), Leaves.end());
        }
        std::vector<Uint32> Coverage(Resolution * Resolution, 0);
        std::vector<Uint32> CurrentLookup(Resolution * Resolution);
        double              Area = 0;
        for (Uint32 I = 0; I < Actual.size(); ++I)
        {
            const Quad& Q = Actual[I];
            Require(Expected[I].X == Q.X && Expected[I].Y == Q.Y && Expected[I].Dim == Q.Dim, "Quadtree split differs from upstream oracle");
            Require(Q.Dim <= Resolution && Q.Dim > 0 && Q.X < Q.Dim && Q.Y < Q.Dim, "Invalid environment node");
            const Uint32 Mip            = MipFor(Q.Dim);
            const float4 V              = Mips[Mip][Q.Y * Q.Dim + Q.X];
            const float  ExpectedWeight = float(1u << (Mip * 2)) * V.w * (3.5f / 3.0f) * 0.0002f;
            // RGB8 rounding plus the upward-quantized logarithmic scale.
            const float ColorTolerance = std::max({V.x, V.y * 0.5f, V.z * 2, 1.0f / 256.0f}) * 1.001f / 510.0f + 1e-6f;
            Require(std::abs(V.x - Q.RadianceAndWeight.x) <= ColorTolerance && std::abs(V.y * 0.5f - Q.RadianceAndWeight.y) <= ColorTolerance && std::abs(V.z * 2 - Q.RadianceAndWeight.z) <= ColorTolerance, "Packed environment radiance/color multiplier");
            Require(std::abs(ExpectedWeight - Q.RadianceAndWeight.w) <= 1e-5f * std::max(1.0f, ExpectedWeight), "Environment power weight");
            Area += 1.0 / double(Q.Dim * Q.Dim);
            const Uint32 Scale = Resolution / Q.Dim;
            for (Uint32 Y = Q.Y * Scale; Y < (Q.Y + 1) * Scale; ++Y)
                for (Uint32 X = Q.X * Scale; X < (Q.X + 1) * Scale; ++X)
                {
                    const auto* Row = reinterpret_cast<const Uint32*>(static_cast<const Uint8*>(LookupData.pData) + Y * LookupData.Stride);
                    Require(I == Row[X], "Environment direction lookup does not match the sampled quad");
                    CurrentLookup[Y * Resolution + X] = Row[X];
                    ++Coverage[Y * Resolution + X];
                }
        }
        Context->UnmapTextureSubresource(ReadTexture, 0, 0);
        Require(std::abs(1.0 - Area) < 1e-12, "Quad solid angles do not cover the sphere");
        for (Uint32 Count : Coverage) Require(1 == Count, "Environment lookup has an overlap or a hole");
        History.SetSources({}, Control.TotalLightCount, Control.EnvmapQuadNodeCount);
        Require(History.Prepare(Context, ControlBuffer, Baker.GetLights(), Baker.GetLookupSRV()), "Map changed environment history");
        auto ReadMap = [&](IBuffer* Source) {
            BufferDesc Desc;
            Desc.Size           = Source->GetDesc().Size;
            Desc.Usage          = USAGE_STAGING;
            Desc.CPUAccessFlags = CPU_ACCESS_READ;
            RefCntAutoPtr<IBuffer> Readback;
            Device->CreateBuffer(Desc, nullptr, &Readback);
            Context->CopyBuffer(Source, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Readback, 0, Desc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            Context->WaitForIdle();
            void* Data = nullptr;
            Context->MapBuffer(Readback, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
            Require(Data != nullptr, "Cannot read history map");
            std::vector<Uint32> Result(Desc.Size / sizeof(Uint32));
            std::memcpy(Result.data(), Data, static_cast<size_t>(Desc.Size));
            Context->UnmapBuffer(Readback, MAP_READ);
            return Result;
        };
        const auto Forward           = ReadMap(History.GetCurrentToPast());
        const auto Reverse           = ReadMap(History.GetPastToCurrent());
        bool       FoundSplitOrMerge = false;
        for (Uint32 I = 0; I < Actual.size(); ++I)
        {
            if (PreviousNodes.empty()) Require(Forward[I] == ~0u, "First environment frame must have no history");
            else
            {
                const Quad&  Current         = Actual[I];
                const Quad&  Previous        = PreviousNodes[I];
                const Uint32 ExpectedForward = PreviousLookup[(Current.Y * Resolution / Current.Dim) * Resolution + Current.X * Resolution / Current.Dim];
                const Uint32 ExpectedReverse = CurrentLookup[(Previous.Y * Resolution / Previous.Dim) * Resolution + Previous.X * Resolution / Previous.Dim];
                Require(ExpectedForward == Forward[I] && ExpectedReverse == Reverse[I], "Environment split/merge history differs from independent LUT oracle");
                FoundSplitOrMerge |= Current.Dim != PreviousNodes[Forward[I]].Dim;
            }
        }
        if (Case != 0) Require(FoundSplitOrMerge, "Environment test did not exercise changing subdivisions");
        Require(History.Finish(Context, Baker.GetLights(), Baker.GetLookupSRV()), "Retain environment history");
        PreviousNodes  = Actual;
        PreviousLookup = CurrentLookup;
        if (Case == 2)
        {
            Require(Baker.CreateResources(Device, 0), "Disable environment for remapping");
            History.SetSources({}, 0, 0);
            Require(History.Prepare(Context, ControlBuffer, Baker.GetLights(), Baker.GetLookupSRV()), "Map disabled environment");
            for (Uint32 Index : ReadMap(History.GetPastToCurrent())) Require(Index == ~0u, "Disabled environment must invalidate old node references");
        }
        std::cout << "Environment case " << Case << ": all " << Actual.size() << " nodes, power/radiance and lookup coverage passed\n";
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
