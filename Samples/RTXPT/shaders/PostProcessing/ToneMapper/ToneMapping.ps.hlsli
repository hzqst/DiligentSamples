#ifndef __RTXPT_TONE_MAPPING_PS_HLSLI__
#define __RTXPT_TONE_MAPPING_PS_HLSLI__

#include "ToneMappingShared.h"

SamplerState      s_LuminanceSampler;
SamplerState      s_ColorSampler;
Texture2D<float4> t_Color;
Texture2D<float>  t_Luminance;

cbuffer g_ToneMappingConstants
{
    RTXPTToneMappingConstants g_Params;
};

float CalcLuminance(float3 Color)
{
    return dot(Color, float3(0.299, 0.587, 0.114));
}

float3 ToneMapLinear(float3 Color) { return Color; }

float3 ToneMapReinhard(float3 Color)
{
    const float Luminance = max(CalcLuminance(Color), 1.0e-6);
    return Color * ((Luminance / (Luminance + 1.0)) / Luminance);
}

float3 ToneMapReinhardModified(float3 Color)
{
    const float Luminance = max(CalcLuminance(Color), 1.0e-6);
    const float WhiteMax  = max(g_Params.WhiteMaxLuminance, 1.0e-6);
    const float White2    = WhiteMax * WhiteMax;
    const float Mapped    = Luminance * (1.0 + Luminance / White2) * (1.0 + Luminance);
    return Color * (Mapped / Luminance);
}

float3 ToneMapHejiHableAlu(float3 Color)
{
    Color = max(0.0.xxx, Color - 0.004.xxx);
    Color = (Color * (6.2 * Color + 0.5)) / (Color * (6.2 * Color + 1.7) + 0.06);
    return pow(Color, 2.2.xxx);
}

float3 ApplyUc2Curve(float3 Color)
{
    const float A = 0.22;
    const float B = 0.30;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.01;
    const float F = 0.30;
    return ((Color * (A * Color + C * B) + D * E) / (Color * (A * Color + B) + D * F)) - (E / F);
}

float3 ToneMapHableUc2(float3 Color)
{
    Color = ApplyUc2Curve(2.0 * Color);
    const float WhiteScale = 1.0 / ApplyUc2Curve(g_Params.WhiteScale.xxx).x;
    return Color * WhiteScale;
}

float3 ToneMapAces(float3 Color)
{
    Color *= 0.6;
    const float A = 2.51;
    const float B = 0.03;
    const float C = 2.43;
    const float D = 0.59;
    const float E = 0.14;
    return saturate((Color * (A * Color + B)) / (Color * (C * Color + D) + E));
}

float3 ApplyOperator(float3 Color)
{
    switch (g_Params.ToneMapOperator)
    {
        case RTXPTToneMapperOperator_Linear:           return ToneMapLinear(Color);
        case RTXPTToneMapperOperator_Reinhard:         return ToneMapReinhard(Color);
        case RTXPTToneMapperOperator_ReinhardModified: return ToneMapReinhardModified(Color);
        case RTXPTToneMapperOperator_HejiHableAlu:     return ToneMapHejiHableAlu(Color);
        case RTXPTToneMapperOperator_HableUc2:         return ToneMapHableUc2(Color);
        case RTXPTToneMapperOperator_Aces:             return ToneMapAces(Color);
        default:                                       return Color;
    }
}

float GetAverageLuminance()
{
    uint Width;
    uint Height;
    uint MipLevels;
    t_Luminance.GetDimensions(0, Width, Height, MipLevels);

    const uint  AvgMip       = MipLevels > 0 ? MipLevels - 1 : 0;
    const float LogLuminance = t_Luminance.Load(int3(0, 0, AvgMip));
    return exp2(LogLuminance);
}

float4 ApplyToneMapping(float2 UV)
{
    const float4 SourceColor = t_Color.Sample(s_ColorSampler, UV);
    float3       FinalColor  = SourceColor.rgb;

    // Disabled tone mapping is a pure passthrough, including exposure.
    if (g_Params.Enabled != 0 && g_Params.AutoExposure != 0)
    {
        const float AvgLuminance = max(GetAverageLuminance(), 1.0e-6);
        FinalColor *= clamp(RTXPT_TONEMAPPING_EXPOSURE_KEY / AvgLuminance,
                            g_Params.AutoExposureLumValueMin,
                            g_Params.AutoExposureLumValueMax);
    }

    if (g_Params.Enabled != 0)
    {
        FinalColor = mul(FinalColor, (float3x3)g_Params.ColorTransform);
        FinalColor = ApplyOperator(FinalColor);
        if (g_Params.Clamped != 0)
            FinalColor = saturate(FinalColor);
    }

    return float4(FinalColor, SourceColor.a);
}

#endif
