#ifndef __RTXPT_DENOISER_NRD_HLSLI__
#define __RTXPT_DENOISER_NRD_HLSLI__

#include "../PathTracer/PathTracerHelpers.hlsli"
#include "RTXPTNrdParameters.h"

#ifndef RTXPT_HAS_NRD_HEADERS
#define RTXPT_HAS_NRD_HEADERS 0
#endif

#if RTXPT_HAS_NRD_HEADERS
#if !defined(NRD_NORMAL_ENCODING) || NRD_NORMAL_ENCODING != 2
#error NRD_NORMAL_ENCODING must be 2
#endif
#if !defined(NRD_ROUGHNESS_ENCODING) || NRD_ROUGHNESS_ENCODING != 1
#error NRD_ROUGHNESS_ENCODING must be 1
#endif
#define NRD_HEADER_ONLY
#include "NRD.hlsli"
#endif

#ifndef RTXPT_VIEWZ_SKY_MARKER
#define RTXPT_VIEWZ_SKY_MARKER 3.402823466e+38F
#endif

float4 RTXPTDenoiserPackNormalAndRoughness(float3 Normal, float Roughness)
{
#if RTXPT_HAS_NRD_HEADERS
    return NRD_FrontEnd_PackNormalAndRoughness(Normal, Roughness, 0);
#else
    return float4(normalize(Normal) * 0.5 + 0.5, saturate(Roughness));
#endif
}

float4 RTXPTDenoiserRelaxPackRadianceHitDist(float3 Radiance, float HitDist)
{
#if RTXPT_HAS_NRD_HEADERS
    return RELAX_FrontEnd_PackRadianceAndHitDist(Radiance, HitDist, true);
#else
    return float4(Radiance, HitDist);
#endif
}

float4 RTXPTDenoiserReblurPackRadianceNormHitDist(float3 Radiance, float NormHitDist)
{
#if RTXPT_HAS_NRD_HEADERS
    return REBLUR_FrontEnd_PackRadianceAndNormHitDist(Radiance, NormHitDist, true);
#else
    return float4(Radiance, NormHitDist);
#endif
}

float RTXPTDenoiserReblurGetNormHitDist(float HitDist, float ViewZ, float Roughness)
{
    // NRD's normalization clamps to epsilon: bypass it for genuinely missing lobes.
    if (!isfinite(HitDist) || HitDist <= 0.0)
        return 0.0;
#if RTXPT_HAS_NRD_HEADERS
    const float3 HitParams = float3(kRTXPTNrdHitDistanceA, kRTXPTNrdHitDistanceB, kRTXPTNrdHitDistanceC);
    return REBLUR_FrontEnd_GetNormHitDist(HitDist, ViewZ, HitParams, Roughness);
#else
    return HitDist > 0.0 ? saturate(HitDist / max(abs(ViewZ), 1.0)) : 0.0;
#endif
}

// The SIGMA penumbra of the distance to the closest occluder of a sun shadow ray: NRD_FP16_MAX where nothing
// occludes it, 0 where the surface gets no sunlight.
float RTXPTDenoiserSigmaPackPenumbra(float DistanceToOccluder, float TanAngularRadius)
{
#if RTXPT_HAS_NRD_HEADERS
    return SIGMA_FrontEnd_PackPenumbra(DistanceToOccluder, TanAngularRadius);
#else
    return DistanceToOccluder;
#endif
}

// The sun visibility in [0, 1] of the denoised SIGMA shadow.
float RTXPTDenoiserSigmaUnpackShadow(float Shadow)
{
#if RTXPT_HAS_NRD_HEADERS
    return SIGMA_BackEnd_UnpackShadow(Shadow);
#else
    return Shadow;
#endif
}

void RTXPTDenoiserPostDenoiseProcess(float3 DiffBSDFEstimate,
                                     float3 SpecBSDFEstimate,
                                     inout float4 DiffRadianceHitDistDenoised,
                                     inout float4 SpecRadianceHitDistDenoised)
{
#if RTXPT_HAS_NRD_HEADERS
#if USE_RELAX
    DiffRadianceHitDistDenoised.xyz = RELAX_BackEnd_UnpackRadiance(DiffRadianceHitDistDenoised).xyz;
    SpecRadianceHitDistDenoised.xyz = RELAX_BackEnd_UnpackRadiance(SpecRadianceHitDistDenoised).xyz;
#else
    DiffRadianceHitDistDenoised.xyz = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(DiffRadianceHitDistDenoised).xyz;
    SpecRadianceHitDistDenoised.xyz = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(SpecRadianceHitDistDenoised).xyz;
#endif
#endif

    DiffRadianceHitDistDenoised.xyz *= max(DiffBSDFEstimate, 0.0.xxx);
    SpecRadianceHitDistDenoised.xyz *= max(SpecBSDFEstimate, 0.0.xxx);
}

#endif // __RTXPT_DENOISER_NRD_HLSLI__
