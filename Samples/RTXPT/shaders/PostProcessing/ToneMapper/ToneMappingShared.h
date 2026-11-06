#ifndef __RTXPT_TONE_MAPPING_SHARED_H__
#define __RTXPT_TONE_MAPPING_SHARED_H__

#define RTXPT_TONEMAPPING_EXPOSURE_KEY 0.042

// Where the exposure comes from: ColorTransform alone, the Sample's auto exposure (the mean of the luminance mips) on
// top of it, or a resolved exposure texture (t_Exposure) on top of a ColorTransform without exposure.
#define RTXPT_TONEMAPPING_EXPOSURE_PARAMETERS 0
#define RTXPT_TONEMAPPING_EXPOSURE_MEASURED   1
#define RTXPT_TONEMAPPING_EXPOSURE_RESOLVED   2

// FXC's shader model 5 path does not accept enum declarations.
static const uint RTXPTToneMapperOperator_Linear           = 0;
static const uint RTXPTToneMapperOperator_Reinhard         = 1;
static const uint RTXPTToneMapperOperator_ReinhardModified = 2;
static const uint RTXPTToneMapperOperator_HejiHableAlu     = 3;
static const uint RTXPTToneMapperOperator_HableUc2         = 4;
static const uint RTXPTToneMapperOperator_Aces             = 5;

struct RTXPTToneMappingConstants
{
    float    WhiteScale;
    float    WhiteMaxLuminance;
    uint     ToneMapOperator;
    uint     Clamped;
    uint     ExposureSource;
    float    AvgLuminance;
    float    AutoExposureLumValueMin;
    float    AutoExposureLumValueMax;
    float3x4 ColorTransform;
    uint     Enabled;
    uint     _Padding0;
    uint     _Padding1;
    uint     _Padding2;
};

#endif
