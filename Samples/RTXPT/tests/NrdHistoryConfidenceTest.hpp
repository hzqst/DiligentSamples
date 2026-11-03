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
    constexpr Uint32 kClear = 0, kScatter = 1, kResolve = 2, kConsume = 3, kPassCount = 4;
    const char*      Code       = R"(
#include "PostProcessing/RTXPTNrdHistoryConfidence.hlsli"
RWStructuredBuffer<float4> u_Points;
RWStructuredBuffer<float2> u_Result;
Texture2D<float> t_Confidence;
SamplerState s_LinearClamp;
[numthreads(8,4,1)] void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = uint2(8,4);
#if TEST_PASS == 0
    u_DenoiserHistoryConfidenceCoverage[id.xy] = 0u;
#elif TEST_PASS == 1
    const float4 Sample = u_Points[id.x + id.y * size.x];
    if (Sample.w != 0.0)
        RTXPTScatterHistoryConfidence(Sample.xy, size, Sample.z != 0.0);
#elif TEST_PASS == 2
    RTXPTResolveHistoryConfidence(id.xy);
    u_Result[id.x + id.y * size.x].x = u_DenoiserHistoryConfidence[id.xy];
#else
    const float4 Sample = u_Points[id.x + id.y * size.x];
    if (Sample.w != 0.0 && all(isfinite(Sample.xy)))
        u_Result[id.x + id.y * size.x].y = t_Confidence.SampleLevel(s_LinearClamp, (Sample.xy + 0.5) / size, 0);
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
    auto                          Result     = MakeBuffer(nullptr, sizeof(float2));
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
        const ImmutableSamplerDesc Sampler{SHADER_TYPE_COMPUTE, "s_LinearClamp", SamplerDesc{FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR, TEXTURE_ADDRESS_CLAMP, TEXTURE_ADDRESS_CLAMP, TEXTURE_ADDRESS_CLAMP}};
        if (Pass == kConsume)
        {
            P.PSODesc.ResourceLayout.ImmutableSamplers    = &Sampler;
            P.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
        }
        P.pCS = Shader;
        Device->CreateComputePipelineState(P, &PSOs[Pass]);
        Require(PSOs[Pass] != nullptr, "confidence PSO");
    }
    const auto Barrier = [&] {
        StateTransitionDesc D[] = {
            {Coverage, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE},
            {Result, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE}};
        Context->TransitionResourceStates(2, D);
    };
    auto Run = [&](std::vector<float4> Points, const std::vector<float>& Expected, bool Reverse) {
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
            Bind("t_Confidence", Confidence->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
            Bind("u_Points", Input->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
            Bind("u_Result", Result->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
            Context->SetPipelineState(PSOs[Pass]);
            Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});
            Barrier();
        }
        BufferDesc D;
        D.Name           = "NRD confidence oracle readback";
        D.Size           = sizeof(float2) * kPixelCount;
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
        const auto* Values = static_cast<const float2*>(Data);
        for (Uint32 Y = 0; Y < kHeight; ++Y)
            for (Uint32 X = 0; X < kWidth; ++X)
            {
                const Uint32 Index = X + Y * kWidth;
                Require(Expected[Index] == Values[Index].x, "previous-coordinate confidence, rejection and holes");
                const auto& Point = Points[Index];
                if (Point.w == 0 || !std::isfinite(Point.x) || !std::isfinite(Point.y)) continue;
                // Independently sample the explicitly expected texture, including linear clamp outside its UV range.
                const float  PX = std::clamp(Point.x, 0.0f, float(kWidth - 1));
                const float  PY = std::clamp(Point.y, 0.0f, float(kHeight - 1));
                const Uint32 X0 = Uint32(PX), Y0 = Uint32(PY);
                const Uint32 X1 = std::min(X0 + 1, kWidth - 1), Y1 = std::min(Y0 + 1, kHeight - 1);
                const float  FX = PX - X0, FY = PY - Y0;
                const float  Top            = Expected[X0 + Y0 * kWidth] * (1 - FX) + Expected[X1 + Y0 * kWidth] * FX;
                const float  Bottom         = Expected[X0 + Y1 * kWidth] * (1 - FX) + Expected[X1 + Y1 * kWidth] * FX;
                const float  ExpectedSample = Top * (1 - FY) + Bottom * FY;
                if (ExpectedSample != Values[Index].y)
                    std::cerr << "PreviousPixel=(" << Point.x << ", " << Point.y << "), expected consumer=" << ExpectedSample << ", actual=" << Values[Index].y << '\n';
                Require(ExpectedSample == Values[Index].y, "consumer samples expected confidence at the contributor's previous UV");
            }
        Context->UnmapBuffer(Readback, MAP_READ);
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
        std::vector<float> Expected(kPixelCount);
        Expected[0] = Expected[(Reverse ? 2u : 1u) + kWidth] = 1;
        for (Uint32 Y : {1u, 2u})
            for (Uint32 X : {3u, 4u}) Expected[X + Y * kWidth] = 1;
        Run(Points, Expected, Reverse);
    }
    // A valid border writer must not hide an invalid contributor whose center is outside
    // the image but whose bilinear history footprint still has positive in-bounds weight.
    constexpr float kLastX = float(kWidth - 1), kLastY = float(kHeight - 1);
    constexpr float kSamplerSubtexel = 1.0f / 256.0f;
    for (float Offset : {0.75f, 0.5f, 1.0f - kSamplerSubtexel, 1.0f, 1.25f})
        for (const auto& Previous : {float2{-Offset, 1}, float2{kLastX + Offset, 1},
                                     float2{1, -Offset}, float2{1, kLastY + Offset},
                                     float2{-Offset, -Offset}, float2{kLastX + Offset, -Offset},
                                     float2{-Offset, kLastY + Offset}, float2{kLastX + Offset, kLastY + Offset}})
            for (bool Reverse : {false, true})
            {
                const float2        Border{std::clamp(Previous.x, 0.0f, kLastX), std::clamp(Previous.y, 0.0f, kLastY)};
                std::vector<float4> Points(kPixelCount);
                Points[0] = float4{Previous.x, Previous.y, 0, 1};
                Points[1] = float4{Border.x, Border.y, 1, 1};
                Points[2] = float4{3, 2, 1, 1}; // unrelated history must remain usable
                std::vector<float> Expected(kPixelCount);
                Expected[3 + 2 * kWidth] = 1;
                if (Offset >= 1.0f) Expected[Uint32(Border.x) + Uint32(Border.y) * kWidth] = 1;
                Run(Points, Expected, Reverse);
            }
}
