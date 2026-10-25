#ifndef __PACKING_HLSLI__
#define __PACKING_HLSLI__

inline float LinearToSRGB(float value)
{
    return value <= 0.0031308 ? value * 12.92 : pow(value, 1.0 / 2.4) * 1.055 - 0.055;
}

inline float SRGBToLinear(float value)
{
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

inline uint Pack_SRGB8_A8(float3 linearRGB, float alpha)
{
    linearRGB = saturate(linearRGB);
    float3 srgb = float3(LinearToSRGB(linearRGB.r), LinearToSRGB(linearRGB.g), LinearToSRGB(linearRGB.b));
    uint3 rgb = uint3(srgb * 255.0 + 0.5);
    return rgb.r | (rgb.g << 8) | (rgb.b << 16) | (uint(saturate(alpha) * 255.0 + 0.5) << 24);
}

inline float3 Unpack_SRGB8(uint packed)
{
    float3 srgb = float3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u) / 255.0;
    return float3(SRGBToLinear(srgb.r), SRGBToLinear(srgb.g), SRGBToLinear(srgb.b));
}

inline float Unpack_A8(uint packed) { return float(packed >> 24) / 255.0; }

#endif
