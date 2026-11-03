// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// GPU oracle for the previous-frame confidence footprint, including conflicting writers.
#pragma once

#include "../src/RTXPTNrdConfig.hpp"
#include <limits>

static void RunNrdFrameTimeTest()
{
    constexpr float kSecondsPerFrame      = 1.0f / 60.0f;
    constexpr float kExpectedMilliseconds = 1000.0f / 60.0f;
    Require(std::abs(kExpectedMilliseconds - RTXPTNrdFrameTimeMilliseconds(kSecondsPerFrame)) < 0.0001f, "NRD seconds to milliseconds");
    for (float Seconds : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max()})
        Require(0.0f == RTXPTNrdFrameTimeMilliseconds(Seconds), "NRD invalid time selects automatic timing");
}

static void RunNrdHistoryConfidenceTest(IRenderDevice* Device, IDeviceContext* Context, IShaderSourceInputStreamFactory* Source)
{
    constexpr Uint32 kWidth = 8, kHeight = 4, kPixelCount = kWidth * kHeight;
    constexpr Uint32 kClear = 0, kScatter = 1, kResolve = 2, kPassCount = 3;
    const char*      Code       = R"(
#include "PostProcessing/RTXPTNrdHistoryConfidence.hlsli"
RWStructuredBuffer<float4> u_Points;
RWStructuredBuffer<float> u_Result;
[numthreads(8,4,1)] void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = uint2(8,4);
#if TEST_PASS == 0
    u_DenoiserHistoryConfidenceCoverage[id.xy] = 0u;
#elif TEST_PASS == 1
    const float4 Sample = u_Points[id.x + id.y * size.x];
    if (Sample.w != 0.0)
        RTXPTScatterHistoryConfidence(Sample.xy, size, Sample.z != 0.0);
#else
    RTXPTResolveHistoryConfidence(id.xy);
    u_Result[id.x + id.y * size.x] = u_DenoiserHistoryConfidence[id.xy];
#endif
})";
    auto             MakeBuffer = [&](const void* Data, Uint32 Stride) {
        BufferDesc D;
        D.Name              = "NRD confidence oracle buffer";
        D.Size              = Stride * kPixelCount;
        D.ElementByteStride = Stride;
        D.Mode              = BUFFER_MODE_STRUCTURED;
        D.BindFlags         = BIND_UNORDERED_ACCESS;
        BufferData             Initial{Data, D.Size};
        RefCntAutoPtr<IBuffer> Buffer;
        Device->CreateBuffer(D, Data ? &Initial : nullptr, &Buffer);
        Require(Buffer != nullptr, "confidence buffer");
        return Buffer;
    };
    auto MakeTexture = [&](TEXTURE_FORMAT Format) {
        TextureDesc D;
        D.Name      = "NRD confidence oracle texture";
        D.Type      = RESOURCE_DIM_TEX_2D;
        D.Width     = kWidth;
        D.Height    = kHeight;
        D.Format    = Format;
        D.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
        RefCntAutoPtr<ITexture> Texture;
        Device->CreateTexture(D, nullptr, &Texture);
        Require(Texture != nullptr, "confidence texture");
        return Texture;
    };
    auto                          Coverage   = MakeTexture(TEX_FORMAT_R32_UINT);
    auto                          Confidence = MakeTexture(TEX_FORMAT_R8_UNORM);
    auto                          Result     = MakeBuffer(nullptr, sizeof(float));
    RefCntAutoPtr<IPipelineState> PSOs[kPassCount];
    for (Uint32 Pass = 0; Pass < kPassCount; ++Pass)
    {
        const std::string Mode = std::to_string(Pass);
        ShaderMacro       Macro{"TEST_PASS", Mode.c_str()};
        ShaderCreateInfo  S;
        S.Desc                       = {"NRD confidence oracle", SHADER_TYPE_COMPUTE, false};
        S.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        S.ShaderCompiler             = SHADER_COMPILER_DXC;
        S.HLSLVersion                = {6, 0};
        S.pShaderSourceStreamFactory = Source;
        S.Source                     = Code;
        S.EntryPoint                 = "main";
        S.Macros                     = {&Macro, 1};
        RefCntAutoPtr<IShader> Shader;
        Device->CreateShader(S, &Shader);
        Require(Shader != nullptr, "confidence shader");
        ComputePipelineStateCreateInfo P;
        P.PSODesc.Name                               = S.Desc.Name;
        P.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
        P.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        P.pCS                                        = Shader;
        Device->CreateComputePipelineState(P, &PSOs[Pass]);
        Require(PSOs[Pass] != nullptr, "confidence PSO");
    }
    const auto Barrier = [&] {
        StateTransitionDesc D{Coverage, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE};
        Context->TransitionResourceStates(1, &D);
    };
    const float kNaN = std::numeric_limits<float>::quiet_NaN();
    // Coordinates below are previous-frame texel centers (integer at a texel center).
    // Repeating in reverse order also checks that atomic rejection is order independent.
    for (bool Reverse : {false, true})
    {
        std::vector<float4> Points(kPixelCount);
        Points[0] = float4{Reverse ? 2.0f : 1.0f, 1, 1, 1}; // changing the destination checks removal of stale coverage
        Points[1] = float4{3.25f, 1.5f, 1, 1};              // all four bilinear taps
        Points[2] = float4{6.25f, 2.25f, 1, 1};             // conflicting valid and invalid bilinear footprints
        Points[3] = float4{6.25f, 2.25f, 0, 1};
        Points[4] = float4{-0.25f, 0, 1, 1}; // partially off-screen: clamp sampler footprint
        Points[5] = float4{50, 0, 1, 1};     // fully off-screen must not write
        Points[6] = float4{kNaN, 0, 1, 1};
        Points[7] = float4{2, 3, 0, 1}; // reset/untracked/unsupported path
        if (Reverse) std::reverse(Points.begin(), Points.end());
        auto Input = MakeBuffer(Points.data(), sizeof(float4));
        for (Uint32 Pass = 0; Pass < kPassCount; ++Pass)
        {
            RefCntAutoPtr<IShaderResourceBinding> SRB;
            PSOs[Pass]->CreateShaderResourceBinding(&SRB, true);
            auto Bind = [&](const char* Name, IDeviceObject* Object) {
                if (auto* V = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name)) V->Set(Object);
            };
            Bind("u_DenoiserHistoryConfidenceCoverage", Coverage->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            Bind("u_DenoiserHistoryConfidence", Confidence->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            Bind("u_Points", Input->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
            Bind("u_Result", Result->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
            Context->SetPipelineState(PSOs[Pass]);
            Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});
            Barrier();
        }
        BufferDesc D;
        D.Name           = "NRD confidence oracle readback";
        D.Size           = sizeof(float) * kPixelCount;
        D.Usage          = USAGE_STAGING;
        D.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<IBuffer> Readback;
        Device->CreateBuffer(D, nullptr, &Readback);
        Context->CopyBuffer(Result, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Readback, 0, D.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->Flush();
        Context->WaitForIdle();
        void* Data = nullptr;
        Context->MapBuffer(Readback, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
        Require(Data != nullptr, "confidence readback");
        const auto* Values = static_cast<const float*>(Data);
        for (Uint32 Y = 0; Y < kHeight; ++Y)
            for (Uint32 X = 0; X < kWidth; ++X)
            {
                const bool Expected = (X == 0 && Y == 0) || (X == (Reverse ? 2u : 1u) && Y == 1) ||
                    ((X == 3 || X == 4) && (Y == 1 || Y == 2));
                Require((Expected ? 1.0f : 0.0f) == Values[X + Y * kWidth], "previous-coordinate confidence, rejection and holes");
            }
        Context->UnmapBuffer(Readback, MAP_READ);
    }
}
