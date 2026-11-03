// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// Invoke the production Prepare entry point, then read both radiance alpha channels.
#pragma once

static void RunNrdPrepareInputsTest(IRenderDevice* Device, IDeviceContext* Context, IShaderSourceInputStreamFactory* Source)
{
    auto MakeTexture = [&](TEXTURE_FORMAT Format, Uint32 ArraySize = 1u) {
        TextureDesc D;
        D.Name      = "NRD prepare regression texture";
        D.Type      = ArraySize == 1u ? RESOURCE_DIM_TEX_2D : RESOURCE_DIM_TEX_2D_ARRAY;
        D.Width     = 2;
        D.Height    = 1;
        D.ArraySize = ArraySize;
        D.Format    = Format;
        D.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
        RefCntAutoPtr<ITexture> T;
        Device->CreateTexture(D, nullptr, &T);
        Require(T != nullptr, "prepare texture");
        return T;
    };
    auto MakeBuffer = [&](const void* Data, Uint64 Size, Uint32 Stride = 0u) {
        BufferDesc D;
        D.Name              = "NRD prepare regression buffer";
        D.Size              = Size;
        D.BindFlags         = Stride == 0u ? BIND_UNIFORM_BUFFER : BIND_UNORDERED_ACCESS;
        D.Mode              = Stride == 0u ? BUFFER_MODE_UNDEFINED : BUFFER_MODE_STRUCTURED;
        D.ElementByteStride = Stride;
        BufferData             Initial{Data, Size};
        RefCntAutoPtr<IBuffer> B;
        Device->CreateBuffer(D, Data != nullptr ? &Initial : nullptr, &B);
        Require(B != nullptr, "prepare buffer");
        return B;
    };
    const char* Code = R"(
#define RTXPT_HAS_NRD_HEADERS 1
#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#define main PrepareInputs
#include "PostProcessing/RTXPTPostProcess.csh"
#undef main
RWStructuredBuffer<float4> u_TestResult;
[numthreads(1,1,1)] void main()
{
    u_DenoiserHistoryConfidenceCoverage[uint2(0,0)] = 0u;
    u_DenoiserHistoryConfidenceCoverage[uint2(1,0)] = 0u;
    PrepareInputs(uint3(0,0,0));
    u_TestResult[0] = u_DenoiserDiffRadianceHitDist[uint2(0,0)];
    u_TestResult[0].x = u_DenoiserHistoryConfidenceCoverage[uint2(1,0)];
    u_TestResult[1] = u_DenoiserSpecRadianceHitDist[uint2(0,0)];
    u_TestResult[1].x = u_DenoiserHistoryConfidenceCoverage[uint2(0,0)];
})";
    for (bool Reblur : {false, true})
    {
        ShaderMacro      Macros[] = {{"RTXPT_POST_PROCESS_MODE", Reblur ? "5" : "4"}};
        ShaderCreateInfo S;
        S.Desc                       = {"Production NRD prepare regression", SHADER_TYPE_COMPUTE, false};
        S.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        S.ShaderCompiler             = SHADER_COMPILER_DXC;
        S.HLSLVersion                = {6, 0};
        S.pShaderSourceStreamFactory = Source;
        S.Source                     = Code;
        S.EntryPoint                 = "main";
        S.Macros                     = {Macros, 1};
        RefCntAutoPtr<IShader> Shader;
        Device->CreateShader(S, &Shader);
        Require(Shader != nullptr, "production prepare shader");
        ComputePipelineStateCreateInfo P;
        P.PSODesc.Name                               = S.Desc.Name;
        P.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
        P.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        P.pCS                                        = Shader;
        RefCntAutoPtr<IPipelineState> PSO;
        Device->CreateComputePipelineState(P, &PSO);
        Require(PSO != nullptr, "production prepare PSO");

        auto         Header       = MakeTexture(TEX_FORMAT_R32_UINT, 4);
        auto         Stable       = MakeTexture(TEX_FORMAT_RGBA16_FLOAT);
        auto         Output       = MakeTexture(TEX_FORMAT_RGBA16_FLOAT);
        auto         Diff         = MakeTexture(TEX_FORMAT_RGBA16_FLOAT);
        auto         Spec         = MakeTexture(TEX_FORMAT_RGBA16_FLOAT);
        auto         Motion       = MakeTexture(TEX_FORMAT_RGBA16_FLOAT);
        auto         Normal       = MakeTexture(TEX_FORMAT_RGB10A2_UNORM);
        auto         ViewZ        = MakeTexture(TEX_FORMAT_R32_FLOAT);
        auto         Confidence   = MakeTexture(TEX_FORMAT_R8_UNORM);
        auto         Coverage     = MakeTexture(TEX_FORMAT_R32_UINT);
        auto         Disocclusion = MakeTexture(TEX_FORMAT_R8_UNORM);
        auto         Combined     = MakeTexture(TEX_FORMAT_R8_UNORM);
        auto         LegacySpec   = MakeTexture(TEX_FORMAT_R32_FLOAT);
        const Uint32 Branches[]   = {1, 1, 1, 0}; // Dominant is layer zero.
        for (Uint32 Slice = 0; Slice < 4; ++Slice)
        {
            TextureSubResData Data{&Branches[Slice], sizeof(Uint32)};
            Context->UpdateTexture(Header, 0, Slice, Box{0, 1, 0, 1}, Data,
                                   RESOURCE_STATE_TRANSITION_MODE_TRANSITION, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        auto Results = MakeBuffer(nullptr, sizeof(float) * 8, sizeof(float) * 4);
        for (Uint32 Layer : {0u, 2u})
        {
            for (bool Missing : {false, true})
            {
                SampleConstants Frame{};
                Frame.ptConsts.imageWidth = Frame.ptConsts.camera.ViewportWidth = 2;
                Frame.ptConsts.imageHeight = Frame.ptConsts.camera.ViewportHeight = 1;
                Frame.ptConsts.camera.CameraW                                     = float3{0, 0, 1};
                Frame.ptConsts.camera.CameraU = Frame.ptConsts.camera.CameraV = float3{0, 0, 0};
                Frame.ptConsts._activeDenoisingLayerCount                     = 3;
                Frame.ptConsts.genericTSLineStride                            = 8;
                Frame.ptConsts.genericTSPlaneStride                           = 64;
                Frame.view.MatWorldToView                                     = float4x4::Identity();
                Frame.view.MatWorldToClipNoOffset                             = float4x4::Identity();
                Frame.previousView                                            = Frame.view;
                Frame.view.ClipToWindowScale                                  = float2{1.0f, -0.5f};
                Frame.previousView.MatWorldToClipNoOffset._41                 = 1.0f; // previous - current = +1 pixel
                SampleMiniConstants Mini{};
                Mini.params = uint4{Layer, 1, 0, 0};
                std::vector<RTXPTDenoisingLayerData> Layers(192);
                auto&                                L = Layers[Layer * Frame.ptConsts.genericTSPlaneStride];
                L.SceneLength                          = 10;
                L.VertexIndexAndRoughness              = (1u << 16) | 0x3c00u; // roughness 1
                L.PreviousVirtualPosition.w            = Missing ? 0.0f : 1.0f;
                // Counts, not stale numeric contents, decide whether the lobe was sampled.
                L.HitDistance.DiffuseDistance  = Missing ? 99.0f : 1.0f;
                L.HitDistance.SpecularDistance = Missing ? 99.0f : 2.0f;
                L.HitDistance.DiffuseSamples = L.HitDistance.SpecularSamples = Missing ? 0u : 1u;
                auto                                  FrameCB                = MakeBuffer(&Frame, sizeof(Frame));
                auto                                  MiniCB                 = MakeBuffer(&Mini, sizeof(Mini));
                auto                                  LayerBuffer            = MakeBuffer(Layers.data(), Layers.size() * sizeof(Layers[0]), sizeof(Layers[0]));
                RefCntAutoPtr<IShaderResourceBinding> SRB;
                PSO->CreateShaderResourceBinding(&SRB, true);
                auto Bind = [&](const char* Name, IDeviceObject* Object) {
                    if (auto* V = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name)) V->Set(Object);
                };
                Bind("g_Const", FrameCB);
                Bind("g_MiniConst", MiniCB);
                Bind("u_DenoisingLayersBuffer", LayerBuffer->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
                Bind("u_DenoisingLayersHeader", Header->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_StableRadiance", Stable->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_OutputColor", Output->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_DenoiserDiffRadianceHitDist", Diff->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_DenoiserSpecRadianceHitDist", Spec->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_DenoiserMotionVectors", Motion->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_DenoiserNormalRoughness", Normal->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_DenoiserViewspaceZ", ViewZ->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_DenoiserHistoryConfidence", Confidence->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_DenoiserHistoryConfidenceCoverage", Coverage->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_DenoiserDisocclusionThresholdMix", Disocclusion->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("u_CombinedHistoryClampRelax", Combined->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
                Bind("t_SpecularHitT", LegacySpec->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
                Bind("u_TestResult", Results->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
                Context->SetPipelineState(PSO);
                Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});
                BufferDesc D;
                D.Name           = "NRD prepare readback";
                D.Size           = sizeof(float) * 8;
                D.Usage          = USAGE_STAGING;
                D.CPUAccessFlags = CPU_ACCESS_READ;
                RefCntAutoPtr<IBuffer> Readback;
                Device->CreateBuffer(D, nullptr, &Readback);
                Context->CopyBuffer(Results, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Readback, 0, D.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                Context->Flush();
                Context->WaitForIdle();
                void* Data = nullptr;
                Context->MapBuffer(Readback, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
                Require(Data != nullptr, "prepare readback");
                float R[8];
                std::memcpy(R, Data, sizeof(R));
                Context->UnmapBuffer(Readback, MAP_READ);
                const float ExpectedDiff = Missing ? 0.0f : (Reblur ? 0.25f : 1.0f);
                const float ExpectedSpec = Missing ? 0.0f : (Reblur ? 0.5f : 2.0f);
                Require((Missing ? 2.0f : 1.0f) == R[0], "production prepare scatters path validity");
                Require(0.0f == R[4], "production prepare leaves the current coordinate unmapped");
                if (std::abs(ExpectedDiff - R[3]) >= 0.001f || std::abs(ExpectedSpec - R[7]) >= 0.001f)
                    std::cerr << "Prepare layer " << Layer << ", reblur " << Reblur << ", missing " << Missing
                              << ": alpha " << R[3] << ", " << R[7] << '\n';
                Require(std::abs(ExpectedDiff - R[3]) < 0.001f, "production diffuse prepare alpha");
                Require(std::abs(ExpectedSpec - R[7]) < 0.001f, "production non-dominant specular prepare alpha");
            }
        }
    }
}
