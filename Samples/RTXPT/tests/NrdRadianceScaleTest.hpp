// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
// Invoke the production Prepare and final merge entry points on layers in working units (ptConsts.radianceScale),
// then read back the NRD inputs and the scene-linear HDR.
#pragma once

// The bits of the half float equal to Value, which is zero or a positive normal half float.
static Uint16 ToHalfBits(float Value)
{
    constexpr Uint32 FloatExponentShift = 23, FloatExponentMask = 0xffu, FloatExponentBias = 127;
    constexpr Uint32 HalfExponentShift = 10, HalfMantissaMask = 0x3ffu, HalfExponentBias = 15;
    constexpr Uint32 MantissaShift = FloatExponentShift - HalfExponentShift;
    if (Value == 0.0f)
        return 0;
    Uint32 Bits = 0;
    std::memcpy(&Bits, &Value, sizeof(Bits));
    const Uint32 Exponent = ((Bits >> FloatExponentShift) & FloatExponentMask) - FloatExponentBias + HalfExponentBias;
    return static_cast<Uint16>((Exponent << HalfExponentShift) | ((Bits >> MantissaShift) & HalfMantissaMask));
}

static void RunNrdRadianceScaleTest(IRenderDevice* Device, IDeviceContext* Context, IShaderSourceInputStreamFactory* Source)
{
    // Working units per scene unit: a power of two, which the outputs divide by exactly.
    constexpr float RadianceScale = 0.25f;
    // Prepare clamps the luminance of a demodulated NRD input to
    // min(255, preExposedGrayLuminance * denoiserRadianceClampK * 16).
    constexpr float GrayLuminance = 0.5f;
    constexpr float ClampK        = 8.0f;
    constexpr float ClampMax      = 64.0f;
    // Prepare and the final merge floor a BSDF estimate at this value; the layers of the test store none.
    constexpr float MinBSDFEstimate = 0.04f;
    constexpr float HalfMax         = 65504.0f;
    constexpr float Tolerance       = 0.001f;
    // The stable radiance of the pixel; its last channel leaves fp16 when divided by RadianceScale.
    constexpr float Stable[] = {2.0f, 4.0f, 30000.0f, 0.0f};
    // The gray noisy radiance of layer 0: demodulated, the bright one exceeds ClampMax and the dim one does not.
    constexpr float BrightRadiance = 1000.0f;
    constexpr float DimRadiance    = 1.0f;
    // The HDR before the final merge of layer 0, whose first channel leaves fp16 with the layer, and the gray
    // demodulated radiance the denoiser returned for the layer.
    constexpr float Merged[] = {64000.0f, 0.0f, 1.0f, 1.0f};
    constexpr float Denoised = 10000.0f;
    constexpr float ViewZ    = 10.0f;

    constexpr Uint32 ResultCount    = 3; // u_OutputColor and both NRD radiance inputs
    constexpr Uint32 ResultChannels = 4;
    constexpr Uint32 LayerCount     = 3;
    constexpr Uint32 LineStride     = 8;
    constexpr Uint32 PlaneStride    = 64;
    constexpr Uint32 HalfBits       = 16;
    // The header's slices: the branch of each layer, a surface, then the dominant layer, layer zero.
    constexpr Uint32 Branches[]   = {1, 1, 1, 0};
    constexpr Uint32 HeaderSlices = sizeof(Branches) / sizeof(Branches[0]);

    constexpr Uint32 TextureWidth = 2;

    // A texture of TextureWidth x 1 texels and ArraySize slices. Texel, when not null, holds one texel of TexelBytes
    // for each slice, which every texel of the slice starts as.
    auto MakeTexture = [&](TEXTURE_FORMAT Format, const void* Texel = nullptr, Uint32 TexelBytes = 0u, Uint32 ArraySize = 1u) {
        TextureDesc D;
        D.Name      = "NRD radiance scale regression texture";
        D.Type      = ArraySize == 1u ? RESOURCE_DIM_TEX_2D : RESOURCE_DIM_TEX_2D_ARRAY;
        D.Width     = TextureWidth;
        D.Height    = 1;
        D.ArraySize = ArraySize;
        D.Format    = Format;
        D.BindFlags = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;

        const Uint32                   RowBytes = TextureWidth * TexelBytes;
        std::vector<Uint8>             Texels(size_t{ArraySize} * RowBytes);
        std::vector<TextureSubResData> Slices(ArraySize);
        for (Uint32 Slice = 0; Texel != nullptr && Slice < ArraySize; ++Slice)
        {
            Uint8* const Row = Texels.data() + size_t{Slice} * RowBytes;
            for (Uint32 X = 0; X < TextureWidth; ++X)
                std::memcpy(Row + X * TexelBytes, static_cast<const Uint8*>(Texel) + Slice * TexelBytes, TexelBytes);
            Slices[Slice] = TextureSubResData{Row, RowBytes};
        }
        TextureData             Initial{Slices.data(), ArraySize};
        RefCntAutoPtr<ITexture> T;
        Device->CreateTexture(D, Texel != nullptr ? &Initial : nullptr, &T);
        Require(T != nullptr, "radiance scale texture");
        return T;
    };
    auto MakeHalfTexture = [&](const float* Texel) {
        const Uint16 Bits[] = {ToHalfBits(Texel[0]), ToHalfBits(Texel[1]), ToHalfBits(Texel[2]), ToHalfBits(Texel[3])};
        return MakeTexture(TEX_FORMAT_RGBA16_FLOAT, Bits, sizeof(Bits));
    };
    auto MakeBuffer = [&](const void* Data, Uint64 Size, Uint32 Stride = 0u) {
        BufferDesc D;
        D.Name              = "NRD radiance scale regression buffer";
        D.Size              = Size;
        D.BindFlags         = Stride == 0u ? BIND_UNIFORM_BUFFER : BIND_UNORDERED_ACCESS;
        D.Mode              = Stride == 0u ? BUFFER_MODE_UNDEFINED : BUFFER_MODE_STRUCTURED;
        D.ElementByteStride = Stride;
        BufferData             Initial{Data, Size};
        RefCntAutoPtr<IBuffer> B;
        Device->CreateBuffer(D, Data != nullptr ? &Initial : nullptr, &B);
        Require(B != nullptr, "radiance scale buffer");
        return B;
    };
    const char* Code = R"(
#define RTXPT_HAS_NRD_HEADERS 1
#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#define main PostProcess
#include "PostProcessing/RTXPTPostProcess.csh"
#undef main
RWStructuredBuffer<float4> u_TestResult;
[numthreads(1,1,1)] void main()
{
    u_DenoiserHistoryConfidenceCoverage[uint2(0,0)] = 0u;
    u_DenoiserHistoryConfidenceCoverage[uint2(1,0)] = 0u;
    PostProcess(uint3(0,0,0));
    u_TestResult[0] = u_OutputColor[uint2(0,0)];
    u_TestResult[1] = u_DenoiserDiffRadianceHitDist[uint2(0,0)];
    u_TestResult[2] = u_DenoiserSpecRadianceHitDist[uint2(0,0)];
})";
    // The pipeline of RTXPT_POST_PROCESS_MODE Mode.
    auto MakePipeline = [&](const char* Mode) {
        ShaderMacro      Macros[] = {{"RTXPT_POST_PROCESS_MODE", Mode}};
        ShaderCreateInfo S;
        S.Desc                       = {"Production NRD radiance scale regression", SHADER_TYPE_COMPUTE, false};
        S.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        S.ShaderCompiler             = SHADER_COMPILER_DXC;
        S.HLSLVersion                = {6, 0};
        S.pShaderSourceStreamFactory = Source;
        S.Source                     = Code;
        S.EntryPoint                 = "main";
        S.Macros                     = {Macros, 1};
        RefCntAutoPtr<IShader> Shader;
        Device->CreateShader(S, &Shader);
        Require(Shader != nullptr, "production radiance scale shader");
        ComputePipelineStateCreateInfo P;
        P.PSODesc.Name                               = S.Desc.Name;
        P.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
        P.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        P.pCS                                        = Shader;
        RefCntAutoPtr<IPipelineState> PSO;
        Device->CreateComputePipelineState(P, &PSO);
        Require(PSO != nullptr, "production radiance scale PSO");
        return PSO;
    };
    // Runs PSO on layer 0 of a pixel whose layers trace in Scale working units, with NoisyRadiance in every channel of
    // the layer, and returns u_OutputColor and the diffuse and specular NRD inputs of the pixel.
    auto Run = [&](IPipelineState* PSO, float Scale, float NoisyRadiance) {
        const float Zero[]        = {0.0f, 0.0f, 0.0f, 0.0f};
        const float DenoisedRGB[] = {Denoised, Denoised, Denoised, 0.0f};
        auto        Header        = MakeTexture(TEX_FORMAT_R32_UINT, Branches, sizeof(Branches[0]), HeaderSlices);
        auto        StableTex     = MakeHalfTexture(Stable);
        auto        Output        = MakeHalfTexture(Merged);
        auto        Diff          = MakeHalfTexture(Zero);
        auto        Spec          = MakeHalfTexture(Zero);
        auto        DenoisedDiff  = MakeHalfTexture(DenoisedRGB);
        auto        DenoisedSpec  = MakeHalfTexture(Zero);
        auto        Validation    = MakeHalfTexture(Zero);
        auto        Motion        = MakeTexture(TEX_FORMAT_RGBA16_FLOAT);
        auto        Normal        = MakeTexture(TEX_FORMAT_RGB10A2_UNORM);
        auto        ViewZOut      = MakeTexture(TEX_FORMAT_R32_FLOAT);
        auto        ViewZIn       = MakeTexture(TEX_FORMAT_R32_FLOAT, &ViewZ, sizeof(ViewZ));
        auto        Confidence    = MakeTexture(TEX_FORMAT_R8_UNORM);
        auto        Coverage      = MakeTexture(TEX_FORMAT_R32_UINT);
        auto        Disocclusion  = MakeTexture(TEX_FORMAT_R8_UNORM);
        auto        Combined      = MakeTexture(TEX_FORMAT_R8_UNORM);
        auto        LegacySpec    = MakeTexture(TEX_FORMAT_R32_FLOAT);
        auto        Results       = MakeBuffer(nullptr, sizeof(float) * ResultChannels * ResultCount, sizeof(float) * ResultChannels);

        SampleConstants Frame{};
        Frame.ptConsts.imageWidth = Frame.ptConsts.camera.ViewportWidth = 2;
        Frame.ptConsts.imageHeight = Frame.ptConsts.camera.ViewportHeight = 1;
        Frame.ptConsts.camera.CameraW                                     = float3{0, 0, 1};
        Frame.ptConsts.camera.CameraU = Frame.ptConsts.camera.CameraV = float3{0, 0, 0};
        Frame.ptConsts._activeDenoisingLayerCount                     = LayerCount;
        Frame.ptConsts.genericTSLineStride                            = LineStride;
        Frame.ptConsts.genericTSPlaneStride                           = PlaneStride;
        Frame.ptConsts.radianceScale                                  = Scale;
        Frame.ptConsts.preExposedGrayLuminance                        = GrayLuminance;
        Frame.ptConsts.denoiserRadianceClampK                         = ClampK;
        Frame.view.MatWorldToView                                     = float4x4::Identity();
        Frame.view.MatWorldToClipNoOffset                             = float4x4::Identity();
        Frame.previousView                                            = Frame.view;
        Frame.view.ClipToWindowScale                                  = float2{1.0f, -0.5f};
        SampleMiniConstants Mini{};
        Mini.params = uint4{0, 1, 0, 0}; // Layer 0; Prepare starts the HDR with the stable radiance.
        std::vector<RTXPTDenoisingLayerData> Layers(LayerCount * PlaneStride);
        auto&                                L = Layers[0];
        L.SceneLength                          = ViewZ;
        L.VertexIndexAndRoughness              = (1u << HalfBits) | ToHalfBits(1.0f);
        L.PreviousVirtualPosition.w            = 1.0f;
        // Fp32ToFp16(float4): x holds green over red, y the specular average over blue. No specular average: the
        // radiance is diffuse.
        const Uint32 Noisy                                = ToHalfBits(NoisyRadiance);
        L.PackedNoisyRadianceAndSpecAvg[0]                = (Noisy << HalfBits) | Noisy;
        L.PackedNoisyRadianceAndSpecAvg[1]                = Noisy;
        auto                                  FrameCB     = MakeBuffer(&Frame, sizeof(Frame));
        auto                                  MiniCB      = MakeBuffer(&Mini, sizeof(Mini));
        auto                                  LayerBuffer = MakeBuffer(Layers.data(), Layers.size() * sizeof(Layers[0]), sizeof(Layers[0]));
        RefCntAutoPtr<IShaderResourceBinding> SRB;
        PSO->CreateShaderResourceBinding(&SRB, true);
        auto Bind = [&](const char* Name, IDeviceObject* Object) {
            if (auto* V = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name)) V->Set(Object);
        };
        Bind("g_Const", FrameCB);
        Bind("g_MiniConst", MiniCB);
        Bind("u_DenoisingLayersBuffer", LayerBuffer->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Bind("u_DenoisingLayersHeader", Header->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_StableRadiance", StableTex->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_OutputColor", Output->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_DenoiserDiffRadianceHitDist", Diff->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_DenoiserSpecRadianceHitDist", Spec->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_DenoiserMotionVectors", Motion->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_DenoiserNormalRoughness", Normal->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_DenoiserViewspaceZ", ViewZOut->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_DenoiserHistoryConfidence", Confidence->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_DenoiserHistoryConfidenceCoverage", Coverage->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_DenoiserDisocclusionThresholdMix", Disocclusion->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("u_CombinedHistoryClampRelax", Combined->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
        Bind("t_SpecularHitT", LegacySpec->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        Bind("t_DenoiserViewspaceZ", ViewZIn->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        Bind("t_DenoiserOutDiffRadianceHitDist", DenoisedDiff->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        Bind("t_DenoiserOutSpecRadianceHitDist", DenoisedSpec->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        Bind("t_DenoiserOutValidation", Validation->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        Bind("u_TestResult", Results->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Context->SetPipelineState(PSO);
        Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->DispatchCompute(DispatchComputeAttribs{1, 1, 1});
        BufferDesc D;
        D.Name           = "NRD radiance scale readback";
        D.Size           = sizeof(float) * ResultChannels * ResultCount;
        D.Usage          = USAGE_STAGING;
        D.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<IBuffer> Readback;
        Device->CreateBuffer(D, nullptr, &Readback);
        Context->CopyBuffer(Results, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Readback, 0, D.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->Flush();
        Context->WaitForIdle();
        void* Data = nullptr;
        Context->MapBuffer(Readback, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
        Require(Data != nullptr, "radiance scale readback");
        std::vector<float> R(ResultChannels * ResultCount);
        std::memcpy(R.data(), Data, R.size() * sizeof(float));
        Context->UnmapBuffer(Readback, MAP_READ);
        return R;
    };
    // Whether the first three channels of result Index of R are Expected.
    auto IsRGB = [&](const std::vector<float>& R, Uint32 Index, float Red, float Green, float Blue) {
        const float Expected[] = {Red, Green, Blue};
        bool        Equal      = true;
        for (Uint32 Channel = 0; Channel < 3; ++Channel)
        {
            const float Actual = R[Index * ResultChannels + Channel];
            Equal              = Equal && std::abs(Expected[Channel] - Actual) <= Tolerance * std::max(1.0f, Expected[Channel]);
        }
        if (!Equal)
            std::cerr << "Radiance scale result " << Index << ": " << R[Index * ResultChannels] << ", " << R[Index * ResultChannels + 1]
                      << ", " << R[Index * ResultChannels + 2] << '\n';
        return Equal;
    };
    constexpr Uint32 OutputResult = 0, DiffResult = 1, SpecResult = 2;
    const float      Demodulated = DimRadiance / MinBSDFEstimate;

    for (const char* Mode : {"4", "5"}) // RELAX and REBLUR prepare
    {
        auto       PSO    = MakePipeline(Mode);
        const auto Bright = Run(PSO, RadianceScale, BrightRadiance);
        Require(IsRGB(Bright, OutputResult, Stable[0] / RadianceScale, Stable[1] / RadianceScale, HalfMax),
                "prepare starts the HDR with the stable radiance in scene units, within fp16");
        // A gray radiance is its luminance in every channel of RELAX's input and in the luma of REBLUR's.
        Require(std::abs(ClampMax - Bright[DiffResult * ResultChannels]) <= Tolerance * ClampMax,
                "prepare clamps the demodulated radiance in working units, whatever the scale");
        const auto Dim = Run(PSO, RadianceScale, DimRadiance);
        Require(std::abs(Demodulated - Dim[DiffResult * ResultChannels]) <= Tolerance * Demodulated,
                "prepare keeps a demodulated radiance below the clamp");
        Require(0.0f == Dim[SpecResult * ResultChannels], "a radiance without a specular average is diffuse");
        const auto Unscaled = Run(PSO, 1.0f, DimRadiance);
        Require(IsRGB(Unscaled, OutputResult, Stable[0], Stable[1], Stable[2]), "a scale of 1 leaves the stable radiance as it is");
    }
    {
        auto        PSO    = MakePipeline("6"); // RELAX final merge
        const float Layer  = Denoised * MinBSDFEstimate;
        const auto  Scaled = Run(PSO, RadianceScale, DimRadiance);
        Require(IsRGB(Scaled, OutputResult, HalfMax, Merged[1] + Layer / RadianceScale, Merged[2] + Layer / RadianceScale),
                "the final merge adds the layer in scene units, within fp16");
        const auto Unscaled = Run(PSO, 1.0f, DimRadiance);
        Require(IsRGB(Unscaled, OutputResult, Merged[0] + Layer, Merged[1] + Layer, Merged[2] + Layer),
                "a scale of 1 adds the layer as it is");
    }
    {
        auto       PSO    = MakePipeline("8"); // Final merge without a denoiser
        const auto Scaled = Run(PSO, RadianceScale, BrightRadiance);
        Require(IsRGB(Scaled, OutputResult, (Stable[0] + BrightRadiance) / RadianceScale, (Stable[1] + BrightRadiance) / RadianceScale, HalfMax),
                "the merge without a denoiser writes the stable and noisy radiance in scene units, within fp16");
    }
}
