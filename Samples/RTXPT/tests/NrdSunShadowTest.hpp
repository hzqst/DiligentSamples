// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// Invoke the production REBLUR Prepare and FinalMerge entry points with the sun shadow denoiser of layer 0: prepare
// packs the sun hit distance into the SIGMA penumbra, and the merge adds the direct sunlight times the SIGMA shadow.
#pragma once

static void RunNrdSunShadowTest(IRenderDevice* Device, IDeviceContext* Context, IShaderSourceInputStreamFactory* Source)
{
    constexpr Uint32 Width                    = 2;
    constexpr float  TanAngularRadius         = 0.025f;
    constexpr float  RadianceScale            = 2.0f;
    constexpr float  OccluderDistance         = 4.0f;
    constexpr float  MissDistance             = 65504.0f;
    constexpr float  SkyViewZ                 = 3.402823466e+38f;
    constexpr float  SurfaceViewZ             = 5.0f;
    constexpr Uint8  ShadowByte               = 128;
    constexpr float  ByteScale                = 255.0f;
    constexpr Uint32 LayerRecordCount         = 192;
    constexpr Uint32 ResultsPerPixel          = 2;
    constexpr Uint32 ResultCount              = Width * ResultsPerPixel;
    constexpr float  Tolerance                = 1.0e-3f;
    const float      DirectLighting[Width][4] = {{1.0f, 2.0f, 3.0f, 0.0f}, {1.0f, 2.0f, 3.0f, 0.0f}};

    auto MakeTexture = [&](TEXTURE_FORMAT Format, const void* Texels, Uint32 TexelBytes, Uint32 ArraySize = 1u) {
        TextureDesc D;
        D.Name      = "NRD sun shadow regression texture";
        D.Type      = ArraySize == 1u ? RESOURCE_DIM_TEX_2D : RESOURCE_DIM_TEX_2D_ARRAY;
        D.Width     = Width;
        D.Height    = 1;
        D.ArraySize = ArraySize;
        D.Format    = Format;
        D.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
        // Every texel the passes read starts defined: zero unless the test gives it.
        std::vector<Uint8>             Zeros(size_t{TexelBytes} * Width);
        const Uint64                   Stride = Uint64{TexelBytes} * Width;
        std::vector<TextureSubResData> Slices(ArraySize, TextureSubResData{Texels != nullptr ? Texels : Zeros.data(), Stride});
        TextureData                    Initial{Slices.data(), ArraySize};
        RefCntAutoPtr<ITexture>        T;
        Device->CreateTexture(D, &Initial, &T);
        Require(T != nullptr, "sun shadow texture");
        return T;
    };
    auto MakeBuffer = [&](const void* Data, Uint64 Size, Uint32 Stride = 0u) {
        BufferDesc D;
        D.Name              = "NRD sun shadow regression buffer";
        D.Size              = Size;
        D.BindFlags         = Stride == 0u ? BIND_UNIFORM_BUFFER : BIND_UNORDERED_ACCESS;
        D.Mode              = Stride == 0u ? BUFFER_MODE_UNDEFINED : BUFFER_MODE_STRUCTURED;
        D.ElementByteStride = Stride;
        BufferData             Initial{Data, Size};
        RefCntAutoPtr<IBuffer> B;
        Device->CreateBuffer(D, Data != nullptr ? &Initial : nullptr, &B);
        Require(B != nullptr, "sun shadow buffer");
        return B;
    };
    const char* Code = R"(
#define RTXPT_HAS_NRD_HEADERS 1
#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#define main ProductionPass
#include "PostProcessing/RTXPTPostProcess.csh"
#undef main
RWStructuredBuffer<float4> u_TestResult;
[numthreads(2,1,1)] void main(uint3 Id : SV_DispatchThreadID)
{
    ProductionPass(Id);
    u_TestResult[Id.x * 2]     = u_OutputColor[Id.xy];
    u_TestResult[Id.x * 2 + 1] = float4(u_SunPenumbra[Id.xy], 0.0, 0.0, 0.0);
})";
    for (bool Merge : {false, true})
    {
        ShaderMacro      Macros[] = {{"RTXPT_POST_PROCESS_MODE", Merge ? "7" : "5"}};
        ShaderCreateInfo S;
        S.Desc                       = {"Production NRD sun shadow regression", SHADER_TYPE_COMPUTE, false};
        S.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        S.ShaderCompiler             = SHADER_COMPILER_DXC;
        S.HLSLVersion                = {6, 0};
        S.pShaderSourceStreamFactory = Source;
        S.Source                     = Code;
        S.EntryPoint                 = "main";
        S.Macros                     = {Macros, 1};
        RefCntAutoPtr<IShader> Shader;
        Device->CreateShader(S, &Shader);
        Require(Shader != nullptr, "production sun shadow shader");
        ComputePipelineStateCreateInfo P;
        P.PSODesc.Name                               = S.Desc.Name;
        P.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
        P.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        P.pCS                                        = Shader;
        RefCntAutoPtr<IPipelineState> PSO;
        Device->CreateComputePipelineState(P, &PSO);
        Require(PSO != nullptr, "production sun shadow PSO");

        for (bool SunShadow : {false, true})
        {
            const float HitDistances[Width] = {OccluderDistance, MissDistance};
            const float ViewZs[Width]       = {SurfaceViewZ, SkyViewZ};
            const Uint8 Shadows[Width]      = {ShadowByte, ShadowByte};
            auto        HitDistance         = MakeTexture(TEX_FORMAT_R32_FLOAT, HitDistances, sizeof(float));
            auto        ViewZ               = MakeTexture(TEX_FORMAT_R32_FLOAT, ViewZs, sizeof(float));
            auto        Direct              = MakeTexture(TEX_FORMAT_RGBA32_FLOAT, DirectLighting, sizeof(DirectLighting[0]));
            auto        Shadow              = MakeTexture(TEX_FORMAT_R8_UNORM, Shadows, sizeof(Uint8));
            auto        Penumbra            = MakeTexture(TEX_FORMAT_R16_FLOAT, nullptr, sizeof(Uint16));
            auto        Output              = MakeTexture(TEX_FORMAT_RGBA16_FLOAT, nullptr, sizeof(Uint16) * 4);
            auto        Header              = MakeTexture(TEX_FORMAT_R32_UINT, nullptr, sizeof(Uint32), 4);
            auto        Rgba16              = [&]() {
                return MakeTexture(TEX_FORMAT_RGBA16_FLOAT, nullptr, sizeof(Uint16) * 4);
            };
            auto R8 = [&]() {
                return MakeTexture(TEX_FORMAT_R8_UNORM, nullptr, sizeof(Uint8));
            };

            SampleConstants Frame{};
            Frame.ptConsts.imageWidth = Frame.ptConsts.camera.ViewportWidth = Width;
            Frame.ptConsts.imageHeight = Frame.ptConsts.camera.ViewportHeight = 1;
            Frame.ptConsts.camera.CameraW                                     = float3{0, 0, 1};
            Frame.ptConsts.camera.CameraU = Frame.ptConsts.camera.CameraV = float3{0, 0, 0};
            Frame.ptConsts._activeDenoisingLayerCount                     = 3;
            Frame.ptConsts.genericTSLineStride                            = 8;
            Frame.ptConsts.genericTSPlaneStride                           = 64;
            Frame.ptConsts.radianceScale                                  = RadianceScale;
            Frame.view.MatWorldToView                                     = float4x4::Identity();
            Frame.view.MatWorldToClipNoOffset                             = float4x4::Identity();
            Frame.previousView                                            = Frame.view;
            Frame.sun.Enabled                                             = 1u;
            Frame.sun.ShadowDenoising                                     = SunShadow ? 1u : 0u;
            Frame.sun.TanAngularRadius                                    = TanAngularRadius;
            SampleMiniConstants Mini{};
            Mini.params = uint4{0, 0, SunShadow ? 1u : 0u, 0};
            std::vector<RTXPTDenoisingLayerData>  Layers(LayerRecordCount);
            auto                                  FrameCB     = MakeBuffer(&Frame, sizeof(Frame));
            auto                                  MiniCB      = MakeBuffer(&Mini, sizeof(Mini));
            auto                                  LayerBuffer = MakeBuffer(Layers.data(), Layers.size() * sizeof(Layers[0]), sizeof(Layers[0]));
            auto                                  Results     = MakeBuffer(nullptr, sizeof(float) * 4 * ResultCount, sizeof(float) * 4);
            RefCntAutoPtr<IShaderResourceBinding> SRB;
            PSO->CreateShaderResourceBinding(&SRB, true);
            auto Bind = [&](const char* Name, IDeviceObject* Object) {
                if (auto* V = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name)) V->Set(Object);
            };
            auto UAV = [](ITexture* T) {
                return T->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
            };
            auto SRV = [](ITexture* T) {
                return T->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
            };
            Bind("g_Const", FrameCB);
            Bind("g_MiniConst", MiniCB);
            Bind("u_DenoisingLayersBuffer", LayerBuffer->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
            Bind("u_DenoisingLayersHeader", UAV(Header));
            Bind("u_StableRadiance", UAV(Rgba16()));
            Bind("u_OutputColor", UAV(Output));
            Bind("u_DenoiserDiffRadianceHitDist", UAV(Rgba16()));
            Bind("u_DenoiserSpecRadianceHitDist", UAV(Rgba16()));
            Bind("u_DenoiserMotionVectors", UAV(Rgba16()));
            Bind("u_DenoiserNormalRoughness", UAV(MakeTexture(TEX_FORMAT_RGB10A2_UNORM, nullptr, sizeof(Uint32))));
            Bind("u_DenoiserViewspaceZ", UAV(MakeTexture(TEX_FORMAT_R32_FLOAT, nullptr, sizeof(float))));
            Bind("u_DenoiserHistoryConfidence", UAV(R8()));
            Bind("u_DenoiserHistoryConfidenceCoverage", UAV(MakeTexture(TEX_FORMAT_R32_UINT, nullptr, sizeof(Uint32))));
            Bind("u_DenoiserDisocclusionThresholdMix", UAV(R8()));
            Bind("u_CombinedHistoryClampRelax", UAV(R8()));
            Bind("u_SunPenumbra", UAV(Penumbra));
            Bind("t_SpecularHitT", SRV(MakeTexture(TEX_FORMAT_R32_FLOAT, nullptr, sizeof(float))));
            Bind("t_DenoiserViewspaceZ", SRV(ViewZ));
            Bind("t_DenoiserOutDiffRadianceHitDist", SRV(Rgba16()));
            Bind("t_DenoiserOutSpecRadianceHitDist", SRV(Rgba16()));
            Bind("t_DenoiserOutValidation", SRV(Rgba16()));
            Bind("t_SunHitDistance", SRV(HitDistance));
            Bind("t_SunDirectLighting", SRV(Direct));
            Bind("t_SunShadow", SRV(Shadow));
            Bind("u_TestResult", Results->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
            Context->SetPipelineState(PSO);
            Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});
            BufferDesc D;
            D.Name           = "NRD sun shadow readback";
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
            Require(Data != nullptr, "sun shadow readback");
            float R[ResultCount][4];
            std::memcpy(R, Data, sizeof(R));
            Context->UnmapBuffer(Readback, MAP_READ);

            auto Near = [&](float Expected, float Actual) {
                return std::abs(Expected - Actual) <= Tolerance * std::max(1.0f, std::abs(Expected));
            };
            if (!Merge)
            {
                // SIGMA_FrontEnd_PackPenumbra: half the penumbra size at the occluder distance; a miss stays NRD_FP16_MAX.
                const float ExpectedOccluded = SunShadow ? OccluderDistance * TanAngularRadius * 0.5f : 0.0f;
                const float ExpectedMiss     = SunShadow ? MissDistance : 0.0f;
                Require(Near(ExpectedOccluded, R[1][0]), "prepare packs the occluder distance into the SIGMA penumbra");
                Require(Near(ExpectedMiss, R[3][0]), "prepare keeps an unoccluded sun ray lit");
                continue;
            }
            // SIGMA_BackEnd_UnpackShadow squares the stored shadow; the merge leaves working units like the layers.
            const float Visibility = (ShadowByte / ByteScale) * (ShadowByte / ByteScale);
            for (Uint32 Channel = 0; Channel < 3; ++Channel)
            {
                const float Expected = SunShadow ? DirectLighting[0][Channel] * Visibility / RadianceScale : 0.0f;
                Require(Near(Expected, R[0][Channel]), "merge adds the shadowed direct sunlight of layer 0");
                Require(Near(0.0f, R[2][Channel]), "merge adds no sunlight to sky pixels");
            }
        }
    }
}
