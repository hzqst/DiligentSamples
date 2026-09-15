#ifndef __BXDF_HLSLI__
#define __BXDF_HLSLI__

// glTF 2.0 metallic-roughness Cook-Torrance BSDF for the reference path tracer.
// Diffuse and GGX lobes are evaluated here so raygen only needs base color,
// metallic, roughness, and shading normal from the payload.

#include "../../Utils/SampleGenerators.hlsli"
#include "../../PathTracerHelpers.hlsli"
#include "../../Scene/Material/MaterialData.hlsli"
#include "LobeType.hlsli"

static const float K_PI   = 3.14159265358979323846;
static const float K_1_PI = 0.31830988618379067154;

static const float kMinCosTheta = 1e-6;
static const float kMinGGXAlpha = 0.0064;

static const uint kBSDFLobeDiffuseReflection    = kLobeTypeDiffuseReflection;
static const uint kBSDFLobeSpecularReflection   = kLobeTypeSpecularReflection;
static const uint kBSDFLobeDeltaReflection      = kLobeTypeDeltaReflection;
static const uint kBSDFLobeDiffuseTransmission  = kLobeTypeDiffuseTransmission;
static const uint kBSDFLobeSpecularTransmission = kLobeTypeSpecularTransmission;
static const uint kBSDFLobeDeltaTransmission    = kLobeTypeDeltaTransmission;
static const uint kBSDFLobeDelta                = kLobeTypeDelta;
static const uint kBSDFLobeTransmission         = kLobeTypeTransmission;

// Shading inputs resolved into the form the lobes consume.
// Match upstream StandardBSDFData's low-precision material storage. Directions,
// lobe evaluation, GGX alpha and PDFs retain their full-precision types.
struct StandardBSDFData
{
    float3 N;
    lpfloat3 diffuse;
    lpfloat3 specular;
    lpfloat3 transmission;
    lpfloat  roughness;
    float  alpha;
    lpfloat  eta;
    lpfloat  metallic;
    lpfloat  diffuseTransmission;
    lpfloat  specularTransmission;
    bool   thinSurface;
    uint   activeLobes;
    uint   psdExclude;
    uint   psdBlockMotionVectorsAtSurface;
    uint   psdDominantDeltaLobeP1;

    lpfloat3 Diffuse() { return diffuse; }
    lpfloat3 Specular() { return specular; }
    lpfloat3 Transmission() { return transmission; }
    lpfloat  Roughness() { return roughness; }
    lpfloat  Metallic() { return metallic; }
    lpfloat  Eta() { return eta; }
    lpfloat  DiffuseTransmission() { return diffuseTransmission; }
    lpfloat  SpecularTransmission() { return specularTransmission; }

    void SetEta(float value)
    {
        eta = lpfloat(value);
    }

    void SetRoughness(float value)
    {
        roughness            = lpfloat(saturate(value));
        const float dataRoughness = roughness;
        const float newAlpha = dataRoughness * dataRoughness;
        alpha                = (newAlpha < kMinGGXAlpha) ? 0.0 : max(newAlpha, kMinGGXAlpha);
    }
};

MaterialHeader MakeMaterialHeader(StandardBSDFData bsdfData)
{
    MaterialHeader header = MaterialHeader::make();
    header.setActiveLobes(bsdfData.activeLobes == 0u ? kLobeTypeAll : bsdfData.activeLobes);
    header.setThinSurface(bsdfData.thinSurface);
    header.setPSDExclude(bsdfData.psdExclude != 0u);
    header.setPSDBlockMotionVectorsAtSurface(bsdfData.psdBlockMotionVectorsAtSurface != 0u);
    header.setPSDDominantDeltaLobeP1(bsdfData.psdDominantDeltaLobeP1);
    return header;
}

StandardBSDFData MakeStandardBSDFData(float3 N,
                                      float3 baseColor,
                                      float  metallic,
                                      float  roughness,
                                      float  materialIoR,
                                      float  outsideIoR,
                                      float  transmissionFactor,
                                      float  diffuseTransmissionFactor,
                                      bool   thinSurface,
                                      bool   frontFacing,
                                      uint   activeLobes,
                                      bool   psdExclude,
                                      bool   psdBlockMotionVectorsAtSurface,
                                      uint   psdDominantDeltaLobeP1)
{
    const float r               = saturate(roughness);
    const float m               = saturate(metallic);
    const float safeMaterialIoR = max(materialIoR, 1.0);
    const float safeOutsideIoR  = max(outsideIoR, 1.0);
    const float f0Sqrt          = (safeMaterialIoR - 1.0) / max(safeMaterialIoR + 1.0, 1e-4);
    const float dielectric      = f0Sqrt * f0Sqrt;
    const float alpha           = r * r;

    StandardBSDFData bsdfData;
    bsdfData.N                              = N;
    bsdfData.diffuse                        = lpfloat3(baseColor * (1.0 - m));
    bsdfData.specular                       = lpfloat3(lerp(float3(dielectric, dielectric, dielectric), baseColor, m));
    bsdfData.transmission                   = lpfloat3(baseColor);
    bsdfData.roughness                      = lpfloat(r);
    bsdfData.alpha                          = (alpha < kMinGGXAlpha) ? 0.0 : max(alpha, kMinGGXAlpha);
    bsdfData.eta                            = lpfloat(frontFacing ? safeOutsideIoR / safeMaterialIoR : safeMaterialIoR / safeOutsideIoR);
    bsdfData.metallic                       = lpfloat(m);
    bsdfData.diffuseTransmission            = lpfloat(saturate(diffuseTransmissionFactor) * (1.0 - m));
    bsdfData.specularTransmission           = lpfloat(saturate(transmissionFactor) * (1.0 - m));
    bsdfData.thinSurface                    = thinSurface;
    bsdfData.activeLobes                    = activeLobes == 0u ? kLobeTypeAll : activeLobes;
    bsdfData.psdExclude                     = psdExclude ? 1u : 0u;
    bsdfData.psdBlockMotionVectorsAtSurface = psdBlockMotionVectorsAtSurface ? 1u : 0u;
    bsdfData.psdDominantDeltaLobeP1         = psdDominantDeltaLobeP1;
    return bsdfData;
}

StandardBSDFData MakeStandardBSDFData(float3 N,
                                      float3 baseColor,
                                      float  metallic,
                                      float  roughness,
                                      float  materialIoR,
                                      float  outsideIoR,
                                      float  transmissionFactor,
                                      float  diffuseTransmissionFactor,
                                      bool   thinSurface,
                                      bool   frontFacing)
{
    return MakeStandardBSDFData(N,
                                baseColor,
                                metallic,
                                roughness,
                                materialIoR,
                                outsideIoR,
                                transmissionFactor,
                                diffuseTransmissionFactor,
                                thinSurface,
                                frontFacing,
                                kLobeTypeAll,
                                false,
                                false,
                                0u);
}

StandardBSDFData MakeStandardBSDFData(float3 N, float3 baseColor, float metallic, float roughness)
{
    return MakeStandardBSDFData(N, baseColor, metallic, roughness, 1.5, 1.0, 0.0, 0.0, true, true);
}

float3 evalFresnelSchlick(float3 f0, float3 f90, float cosTheta)
{
    const float f = pow(saturate(1.0 - cosTheta), 5.0);
    return f0 + (f90 - f0) * f;
}

float evalFresnelSchlick(float f0, float f90, float cosTheta)
{
    const float f = pow(saturate(1.0 - cosTheta), 5.0);
    return f0 + (f90 - f0) * f;
}

float evalFresnelDielectric(float eta, float cosThetaI, out float cosThetaT)
{
    eta       = max(eta, 1e-7);
    cosThetaI = clamp(cosThetaI, -1.0, 1.0);
    if (cosThetaI < 0.0)
    {
        eta       = 1.0 / eta;
        cosThetaI = -cosThetaI;
    }

    const float sin2ThetaT = eta * eta * max(0.0, 1.0 - cosThetaI * cosThetaI);
    if (sin2ThetaT > 1.0)
    {
        cosThetaT = 0.0;
        return 1.0;
    }

    cosThetaT       = sqrt(max(0.0, 1.0 - sin2ThetaT));
    const float Rs  = (eta * cosThetaI - cosThetaT) / max(eta * cosThetaI + cosThetaT, 1e-7);
    const float Rp  = (eta * cosThetaT - cosThetaI) / max(eta * cosThetaT + cosThetaI, 1e-7);
    return saturate(0.5 * (Rs * Rs + Rp * Rp));
}

float evalFresnelDielectric(float eta, float cosThetaI)
{
    float cosThetaT;
    return evalFresnelDielectric(eta, cosThetaI, cosThetaT);
}

// Trowbridge-Reitz (GGX) normal distribution function.
float evalNdfGGX(float alpha, float cosTheta)
{
    const float A2 = alpha * alpha;
    const float D  = (cosTheta * cosTheta) * (A2 - 1.0) + 1.0;
    return A2 / (K_PI * D * D);
}

// Smith height-correlated visibility term = G / (4 * NoV * NoL).
float evalVisibilitySmithGGXCorrelated(float alpha, float cosThetaO, float cosThetaI)
{
    const float A2 = alpha * alpha;
    const float V  = cosThetaI * sqrt(cosThetaO * cosThetaO * (1.0 - A2) + A2);
    const float L  = cosThetaO * sqrt(cosThetaI * cosThetaI * (1.0 - A2) + A2);
    return 0.5 / max(V + L, 1e-7);
}

float3 approxSpecularIntegralGGX(float3 specularReflectance, float alpha, float cosTheta)
{
    cosTheta = abs(cosTheta);

    float4 X;
    X.x = 1.0;
    X.y = cosTheta;
    X.z = cosTheta * cosTheta;
    X.w = cosTheta * X.z;

    float4 Y;
    Y.x = 1.0;
    Y.y = alpha;
    Y.z = alpha * alpha;
    Y.w = alpha * Y.z;

    // Coefficients for the height-correlated Smith GGX masking function used by this file.
    const float2x2 M1 = float2x2(0.995367, -1.38839,
                                 -0.24751, 1.97442);
    const float3x3 M2 = float3x3(1.0, 2.68132, 52.366,
                                 16.0932, -3.98452, 59.3013,
                                 -5.18731, 255.259, 2544.07);
    const float2x2 M3 = float2x2(-0.0564526, 3.82901,
                                 16.91, -11.0303);
    const float3x3 M4 = float3x3(1.0, 4.11118, -1.37886,
                                 19.3254, -28.9947, 16.9514,
                                 0.545386, 96.0994, -79.4492);

    float bias  = dot(mul(M1, X.xy), Y.xy) / max(dot(mul(M2, X.xyw), Y.xyw), 1e-7);
    float scale = dot(mul(M3, X.xy), Y.xy) / max(dot(mul(M4, X.xzw), Y.xyw), 1e-7);

    const float specularReflectanceLuma = dot(specularReflectance, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
    bias *= saturate(specularReflectanceLuma * 50.0);

    return specularReflectance * max(0.0, scale) + max(0.0, bias);
}

float luminance(float3 C)
{
    return dot(C, float3(0.2126, 0.7152, 0.0722));
}

float EmsApprox(float r2, float NdV)
{
    const float r4  = r2 * r2;
    const float nv0 = 0.2 * r2;
    const float nv1 = 0.32 * r2 + 1.94 * r4;
    return lerp(nv0, nv1, NdV);
}

float3 MultiScatterSpecularApprox(float alpha, float NdV, float3 F0)
{
    return 1.0 + F0 * EmsApprox(alpha, NdV);
}

float evalPdfGGX_BVNDF(float alphaValue, float3 viewLocal, float3 hLocal)
{
    const float2 alpha = alphaValue.xx;
    const float  ndf   = evalNdfGGX(alphaValue, hLocal.z);
    const float2 ai    = alpha * viewLocal.xy;
    const float  len2  = dot(ai, ai);
    const float  t     = sqrt(len2 + viewLocal.z * viewLocal.z);
    const float  a     = saturate(min(alpha.x, alpha.y));
    const float  s     = 1.0 + length(viewLocal.xy);
    const float  a2    = a * a;
    const float  s2    = s * s;
    const float  k     = (1.0 - a2) * s2 / max(s2 + a2 * viewLocal.z * viewLocal.z, 1e-7);
    return ndf / max(2.0 * (k * viewLocal.z + t), 1e-7);
}

float3 sampleGGX_BVNDF(float alphaValue, float3 viewLocal, float2 rand)
{
    const float2 alpha    = alphaValue.xx;
    const float3 iStd     = normalize(float3(viewLocal.xy * alpha, viewLocal.z));
    const float  phi      = 2.0 * K_PI * rand.x;
    const float  a        = saturate(min(alpha.x, alpha.y));
    const float  s        = 1.0 + length(viewLocal.xy);
    const float  a2       = a * a;
    const float  s2       = s * s;
    const float  k        = (1.0 - a2) * s2 / max(s2 + a2 * viewLocal.z * viewLocal.z, 1e-7);
    const float  b        = (viewLocal.z > 0.0) ? k * iStd.z : iStd.z;
    const float  z        = mad(1.0 - rand.y, 1.0 + b, -b);
    const float  sinTheta = sqrt(saturate(1.0 - z * z));
    const float3 oStd     = float3(sinTheta * cos(phi), sinTheta * sin(phi), z);
    const float3 mStd     = iStd + oStd;
    return normalize(float3(mStd.xy * alpha, mStd.z));
}

float3 sampleCosineHemisphereLocal(float2 rand, out float pdf)
{
    const float R     = sqrt(rand.x);
    const float theta = 2.0 * K_PI * rand.y;
    const float x     = R * cos(theta);
    const float y     = R * sin(theta);
    const float z     = sqrt(max(0.0, 1.0 - rand.x));
    pdf               = z * K_1_PI;
    return float3(x, y, z);
}

float3 GetTransmissionAlbedo(StandardBSDFData bsdfData)
{
    return bsdfData.thinSurface ? bsdfData.transmission : sqrt(max(bsdfData.transmission, float3(0.0, 0.0, 0.0)));
}

struct DiffuseReflectionFrostbite
{
    float3 albedo;
    float  roughness;

    float3 eval(const float3 wi, const float3 wo)
    {
        if (min(wi.z, wo.z) < kMinCosTheta)
            return float3(0.0, 0.0, 0.0);

        return evalWeight(wi, wo) * K_1_PI * wo.z;
    }

    bool sample(const float3 wi, out float3 wo, out float pdf, out float3 weight, out uint lobe, out float lobeP, float3 preGeneratedSample)
    {
        wo   = sampleCosineHemisphereLocal(preGeneratedSample.xy, pdf);
        lobe = kLobeTypeDiffuseReflection;

        if (min(wi.z, wo.z) < kMinCosTheta)
        {
            weight = float3(0.0, 0.0, 0.0);
            lobeP  = 0.0;
            return false;
        }

        weight = evalWeight(wi, wo);
        lobeP  = 1.0;
        return true;
    }

    float evalPdf(const float3 wi, const float3 wo)
    {
        if (min(wi.z, wo.z) < kMinCosTheta)
            return 0.0;

        return K_1_PI * wo.z;
    }

    float3 evalWeight(float3 wi, float3 wo)
    {
        const float3 h            = normalize(wi + wo);
        const float  woDotH       = saturate(dot(wo, h));
        const float  energyBias   = lerp(0.0, 0.5, roughness);
        const float  energyFactor = lerp(1.0, 1.0 / 1.51, roughness);
        const float  fd90         = energyBias + 2.0 * woDotH * woDotH * roughness;
        const float  wiScatter    = evalFresnelSchlick(1.0, fd90, wi.z);
        const float  woScatter    = evalFresnelSchlick(1.0, fd90, wo.z);
        return albedo * wiScatter * woScatter * energyFactor;
    }
};

struct DiffuseTransmissionLambert
{
    float3 albedo;

    float3 eval(const float3 wi, const float3 wo)
    {
        if (min(wi.z, -wo.z) < kMinCosTheta)
            return float3(0.0, 0.0, 0.0);

        return K_1_PI * albedo * -wo.z;
    }

    bool sample(const float3 wi, out float3 wo, out float pdf, out float3 weight, out uint lobe, out float lobeP, float3 preGeneratedSample)
    {
        wo   = sampleCosineHemisphereLocal(preGeneratedSample.xy, pdf);
        wo.z = -wo.z;
        lobe = kLobeTypeDiffuseTransmission;

        if (min(wi.z, -wo.z) < kMinCosTheta)
        {
            weight = float3(0.0, 0.0, 0.0);
            lobeP  = 0.0;
            return false;
        }

        weight = albedo;
        lobeP  = 1.0;
        return true;
    }

    float evalPdf(const float3 wi, const float3 wo)
    {
        if (min(wi.z, -wo.z) < kMinCosTheta)
            return 0.0;

        return K_1_PI * -wo.z;
    }
};

bool IsFiniteVector(float3 v)
{
    return all(v == v) && all(abs(v) < float3(3.402823466e+38, 3.402823466e+38, 3.402823466e+38));
}

bool IsFiniteScalar(float v)
{
    return (v == v) && (abs(v) < 3.402823466e+38);
}

struct SpecularReflectionMicrofacet
{
    float3 albedo;
    float  alpha;
    uint   activeLobes;

    bool hasLobe(uint lobe)
    {
        return (activeLobes & lobe) != 0u;
    }

    float3 eval(const float3 wi, const float3 wo)
    {
        if (alpha == 0.0 || min(wi.z, wo.z) < kMinCosTheta || !hasLobe(kLobeTypeSpecularReflection))
            return float3(0.0, 0.0, 0.0);

        const float3 hLocal = normalize(wi + wo);
        const float  wiDotH = saturate(dot(wi, hLocal));
        const float  d      = evalNdfGGX(alpha, hLocal.z);
        const float  vis    = evalVisibilitySmithGGXCorrelated(alpha, wi.z, wo.z);
        const float3 fresnel = evalFresnelSchlick(albedo, float3(1.0, 1.0, 1.0), wiDotH);
        const float3 msSpecular = MultiScatterSpecularApprox(alpha, wi.z, albedo);
        return d * vis * fresnel * msSpecular * wo.z;
    }

    bool sample(const float3 wi, out float3 wo, out float pdf, out float3 weight, out uint lobe, out float lobeP, float3 preGeneratedSample)
    {
        wo     = float3(0.0, 0.0, 0.0);
        pdf    = 0.0;
        weight = float3(0.0, 0.0, 0.0);
        lobe   = kLobeTypeSpecularReflection;
        lobeP  = 1.0;

        if (wi.z < kMinCosTheta)
            return false;

        if (alpha == 0.0)
        {
            if (!hasLobe(kLobeTypeDeltaReflection))
                return false;

            wo     = float3(-wi.x, -wi.y, wi.z);
            weight = evalFresnelSchlick(albedo, float3(1.0, 1.0, 1.0), wi.z);
            lobe   = kLobeTypeDeltaReflection;
            return IsFiniteVector(wo);
        }

        if (!hasLobe(kLobeTypeSpecularReflection))
            return false;

        const float3 hLocal = sampleGGX_BVNDF(alpha, wi, preGeneratedSample.xy);
        const float  wiDotH = dot(wi, hLocal);
        if (wiDotH <= 0.0)
            return false;

        wo = 2.0 * wiDotH * hLocal - wi;
        if (wo.z < kMinCosTheta)
            return false;

        pdf = evalPdf(wi, wo);
        if (!IsFiniteScalar(pdf) || pdf <= 0.0)
            return false;

        weight = eval(wi, wo) / pdf;
        lobe   = kLobeTypeSpecularReflection;
        return IsFiniteVector(weight);
    }

    float evalPdf(const float3 wi, const float3 wo)
    {
        if (alpha == 0.0 || min(wi.z, wo.z) < kMinCosTheta || !hasLobe(kLobeTypeSpecularReflection))
            return 0.0;

        const float3 hLocal = normalize(wi + wo);
        return evalPdfGGX_BVNDF(alpha, wi, hLocal);
    }
};

struct SpecularReflectionTransmissionMicrofacet
{
    float3 transmissionAlbedo;
    float  alpha;
    float  eta;
    uint   activeLobes;
    bool   isThinSurface;

    bool hasLobe(uint lobe)
    {
        return (activeLobes & lobe) != 0u;
    }

    float3 eval(const float3 wi, const float3 wo)
    {
        if (alpha == 0.0 || wi.z < kMinCosTheta || abs(wo.z) < kMinCosTheta)
            return float3(0.0, 0.0, 0.0);

        const bool isReflection = wo.z > 0.0;
        if ((isReflection && !hasLobe(kLobeTypeSpecularReflection)) ||
            (!isReflection && !hasLobe(kLobeTypeSpecularTransmission)))
            return float3(0.0, 0.0, 0.0);

        const float actualEta = (isThinSurface && !isReflection) ? 1.0 : eta;

        const float3 hUnnormalized = wo + wi * (isReflection ? 1.0 : actualEta);
        const float  hLen2         = dot(hUnnormalized, hUnnormalized);
        if (!IsFiniteScalar(hLen2) || hLen2 <= 1e-14)
            return float3(0.0, 0.0, 0.0);

        float3 hLocal = hUnnormalized * rsqrt(hLen2);
        if (hLocal.z < 0.0)
            hLocal = -hLocal;

        const float viewDotH  = dot(wi, hLocal);
        const float lightDotH = dot(wo, hLocal);
        const float d         = evalNdfGGX(alpha, hLocal.z);
        const float vis       = evalVisibilitySmithGGXCorrelated(alpha, wi.z, abs(wo.z));
        const float g         = vis * 4.0 * wi.z * abs(wo.z);
        const float fresnel   = evalFresnelDielectric(actualEta, viewDotH);

        if (isReflection)
            return float3(fresnel, fresnel, fresnel) * d * g * 0.25 / max(wi.z, kMinCosTheta);

        const float sqrtDenom = lightDotH + actualEta * viewDotH;
        const float t         = actualEta * actualEta * viewDotH * lightDotH /
            max(wi.z * sqrtDenom * sqrtDenom, 1e-7);
        return transmissionAlbedo * (1.0 - fresnel) * d * g * abs(t);
    }

    bool sample(const float3 wi, out float3 wo, out float pdf, out float3 weight, out uint lobe, out float lobeP, float3 preGeneratedSample)
    {
        wo     = float3(0.0, 0.0, 0.0);
        pdf    = 0.0;
        weight = float3(0.0, 0.0, 0.0);
        lobe   = kLobeTypeSpecularReflection;
        lobeP  = 1.0;

        if (wi.z < kMinCosTheta)
            return false;

        const bool delta                  = alpha == 0.0;
        const bool hasRoughReflection     = hasLobe(kLobeTypeSpecularReflection);
        const bool hasDeltaReflection     = hasLobe(kLobeTypeDeltaReflection);
        const bool hasRoughTransmission   = hasLobe(kLobeTypeSpecularTransmission);
        const bool hasDeltaTransmission   = hasLobe(kLobeTypeDeltaTransmission);
        const bool canReflect             = delta ? hasDeltaReflection : hasRoughReflection;
        const bool canTransmit            = delta ? hasDeltaTransmission : hasRoughTransmission;
        if (!(canReflect || canTransmit))
            return false;

        const float3 hLocal  = delta ? float3(0.0, 0.0, 1.0) : sampleGGX_BVNDF(alpha, wi, preGeneratedSample.xy);
        const float  wiDotH  = delta ? wi.z : dot(wi, hLocal);
        float        cosThetaT;
        float        fresnel = evalFresnelDielectric(eta, wiDotH, cosThetaT);
        const float  splitFresnel = fresnel;
        const bool   branchSplit  = canReflect && canTransmit;

        bool isReflection = canReflect;
        if (branchSplit)
            isReflection = preGeneratedSample.z < splitFresnel;
        else if (canTransmit && splitFresnel == 1.0)
            return false;

        float actualEta = eta;
        if (isThinSurface && !isReflection)
        {
            actualEta = 1.0;
            fresnel   = evalFresnelDielectric(actualEta, wi.z, cosThetaT);
        }

        const bool deltaEvent = delta;
        if (deltaEvent && !isReflection)
        {
            if (!hasDeltaTransmission)
                return false;
            fresnel = evalFresnelDielectric(actualEta, wi.z, cosThetaT);
        }

        if (isReflection)
        {
            if ((deltaEvent && !hasDeltaReflection) || (!deltaEvent && !hasRoughReflection))
                return false;
        }
        else
        {
            if ((deltaEvent && !hasDeltaTransmission) || (!deltaEvent && !hasRoughTransmission))
                return false;
        }

        if (isReflection)
            wo = deltaEvent ? float3(-wi.x, -wi.y, wi.z) : 2.0 * wiDotH * hLocal - wi;
        else
            wo = deltaEvent ? float3(-wi.x * actualEta, -wi.y * actualEta, -cosThetaT) :
                (actualEta * wiDotH - cosThetaT) * hLocal - actualEta * wi;

        if (abs(wo.z) < kMinCosTheta || ((wo.z > 0.0) != isReflection))
            return false;

        const float branchP      = isReflection ? splitFresnel : (1.0 - splitFresnel);
        const float branchWeight = isReflection ? fresnel : (1.0 - fresnel);
        lobeP = branchSplit ? branchP : 1.0;
        lobe  = isReflection ?
            (deltaEvent ? kLobeTypeDeltaReflection : kLobeTypeSpecularReflection) :
            (deltaEvent ? kLobeTypeDeltaTransmission : kLobeTypeSpecularTransmission);

        if (deltaEvent)
        {
            weight = isReflection ? float3(1.0, 1.0, 1.0) : transmissionAlbedo;
            if (!branchSplit)
                weight *= branchWeight;
            pdf    = 0.0;
            return IsFiniteVector(wo);
        }

        pdf = evalPdf(wi, wo);
        if (branchSplit && !(hasRoughReflection && hasRoughTransmission))
            pdf *= branchP;
        if (!IsFiniteScalar(pdf) || pdf <= 0.0)
            return false;

        weight = eval(wi, wo) / pdf;
        return IsFiniteVector(weight);
    }

    float evalPdf(const float3 wi, const float3 wo)
    {
        if (alpha == 0.0 || wi.z < kMinCosTheta || abs(wo.z) < kMinCosTheta)
            return 0.0;

        const bool isReflection = wo.z > 0.0;
        const bool hasReflection = hasLobe(kLobeTypeSpecularReflection);
        const bool hasTransmission = hasLobe(kLobeTypeSpecularTransmission);
        if ((isReflection && !hasReflection) || (!isReflection && !hasTransmission))
            return 0.0;

        const float actualEta = (isThinSurface && !isReflection) ? 1.0 : eta;

        const float3 hUnnormalized = wo + wi * (isReflection ? 1.0 : actualEta);
        const float  hLen2         = dot(hUnnormalized, hUnnormalized);
        if (!IsFiniteScalar(hLen2) || hLen2 <= 1e-14)
            return 0.0;

        float3 hLocal = hUnnormalized * rsqrt(hLen2);
        if (hLocal.z < 0.0)
            hLocal = -hLocal;

        const float viewDotH  = dot(wi, hLocal);
        const float lightDotH = dot(wo, hLocal);
        float       pdf       = evalPdfGGX_BVNDF(alpha, wi, hLocal);

        if (isReflection)
        {
            if (lightDotH <= 0.0)
                return 0.0;
            pdf *= viewDotH / max(lightDotH, 1e-7);
        }
        else
        {
            if (lightDotH > 0.0)
                return 0.0;
            const float sqrtDenom = lightDotH + actualEta * viewDotH;
            pdf *= viewDotH * 4.0 * abs(lightDotH) / max(sqrtDenom * sqrtDenom, 1e-7);
        }

        if (hasReflection && hasTransmission)
        {
            const float fresnel = evalFresnelDielectric(actualEta, viewDotH);
            pdf *= isReflection ? fresnel : (1.0 - fresnel);
        }

        if (!IsFiniteScalar(pdf))
            return 0.0;

        return clamp(pdf, 0.0, 3.402823466e+38);
    }
};

static const uint cBxDFMaxDeltaLobes = 2u;

struct BxDFDeltaLobe
{
    float3 dir;
    float3 thp;
    uint   transmission;
    float  probability;

    static BxDFDeltaLobe make()
    {
        BxDFDeltaLobe ret;
        ret.dir          = float3(0.0, 0.0, 1.0);
        ret.thp          = float3(0.0, 0.0, 0.0);
        ret.transmission = 0u;
        ret.probability  = 0.0;
        return ret;
    }
};

struct FalcorBSDF
{
    DiffuseReflectionFrostbite diffuseReflection;
    DiffuseTransmissionLambert diffuseTransmission;
    SpecularReflectionMicrofacet specularReflection;
    SpecularReflectionTransmissionMicrofacet specularReflectionTransmission;

    float diffTrans;
    float specTrans;
    float pDiffuseReflection;
    float pDiffuseTransmission;
    float pSpecularReflection;
    float pSpecularReflectionTransmission;
    bool  psdExclude;

    void __init(const MaterialHeader mtl, float3 N, float3 V, const StandardBSDFData data)
    {
        const bool isThinSurface = mtl.isThinSurface();
        const float3 dataTransmission = data.Transmission();
        const float3 transmissionAlbedo = isThinSurface ? dataTransmission : sqrt(max(dataTransmission, float3(0.0, 0.0, 0.0)));
        const float dataRoughness = data.Roughness();

        diffuseReflection.albedo   = data.Diffuse();
        diffuseReflection.roughness = dataRoughness;
        diffuseTransmission.albedo = transmissionAlbedo;

        float alpha = dataRoughness * dataRoughness;
        if (alpha < kMinGGXAlpha)
            alpha = 0.0;
        else
            alpha = max(alpha, kMinGGXAlpha);

        const uint activeLobes = mtl.getActiveLobes();
        psdExclude = mtl.isPSDExclude();

        const lpfloat3 dataSpecular = data.Specular();
        const lpfloat dataEta = data.Eta();
        specularReflection.albedo      = dataSpecular;
        specularReflection.alpha       = alpha;
        specularReflection.activeLobes = activeLobes;

        specularReflectionTransmission.transmissionAlbedo = transmissionAlbedo;
        specularReflectionTransmission.alpha = dataEta == 1.0 ? 0.0 : alpha;
        specularReflectionTransmission.eta = dataEta;
        specularReflectionTransmission.activeLobes = activeLobes;
        specularReflectionTransmission.isThinSurface = isThinSurface;

        diffTrans = data.DiffuseTransmission();
        specTrans = data.SpecularTransmission();

        const lpfloat dataMetallic = data.Metallic();
        const float metallicBRDF  = dataMetallic * (1.0 - specTrans);
        const float dielectricBSDF = (1.0 - dataMetallic) * (1.0 - specTrans);
        const float specularBSDF  = specTrans;
        const float diffuseWeight = luminance(data.Diffuse());
        const float specularWeight = luminance(evalFresnelSchlick(data.Specular(), float3(1.0, 1.0, 1.0), saturate(dot(V, N))));

        pDiffuseReflection = (activeLobes & kLobeTypeDiffuseReflection) != 0u ? diffuseWeight * dielectricBSDF * (1.0 - diffTrans) : 0.0;
        pDiffuseTransmission = (activeLobes & kLobeTypeDiffuseTransmission) != 0u ? diffuseWeight * dielectricBSDF * diffTrans : 0.0;
        pSpecularReflection = (activeLobes & (kLobeTypeSpecularReflection | kLobeTypeDeltaReflection)) != 0u ? specularWeight * (metallicBRDF + dielectricBSDF) : 0.0;
        pSpecularReflectionTransmission =
            (activeLobes & (kLobeTypeSpecularReflection | kLobeTypeDeltaReflection | kLobeTypeSpecularTransmission | kLobeTypeDeltaTransmission)) != 0u ? specularBSDF : 0.0;

        float normFactor = pDiffuseReflection + pDiffuseTransmission + pSpecularReflection + pSpecularReflectionTransmission;
        if (normFactor > 0.0)
        {
            normFactor = 1.0 / normFactor;
            pDiffuseReflection *= normFactor;
            pDiffuseTransmission *= normFactor;
            pSpecularReflection *= normFactor;
            pSpecularReflectionTransmission *= normFactor;
        }
    }

    static FalcorBSDF make(const MaterialHeader mtl, float3 N, float3 V, const StandardBSDFData data)
    {
        FalcorBSDF ret;
        ret.__init(mtl, N, V, data);
        return ret;
    }

    static uint getLobes(const StandardBSDFData data)
    {
        const float alpha = data.Roughness() * data.Roughness();
        const bool  isDelta = alpha < kMinGGXAlpha;
        const lpfloat diffTransValue = data.DiffuseTransmission();
        const lpfloat specTransValue = data.SpecularTransmission();

        uint lobes = isDelta ? kLobeTypeDeltaReflection : kLobeTypeSpecularReflection;
        if (any(data.Diffuse() > 0.0) && specTransValue < 1.0)
        {
            if (diffTransValue < 1.0)
                lobes |= kLobeTypeDiffuseReflection;
            if (diffTransValue > 0.0)
                lobes |= kLobeTypeDiffuseTransmission;
        }
        if (specTransValue > 0.0)
            lobes |= isDelta ? kLobeTypeDeltaTransmission : kLobeTypeSpecularTransmission;

        return lobes;
    }

    void evalDeltaLobes(const float3 wi, out BxDFDeltaLobe deltaLobes[cBxDFMaxDeltaLobes], out uint deltaLobeCount, out float nonDeltaPart)
    {
        deltaLobeCount = cBxDFMaxDeltaLobes;
        for (uint i = 0u; i < cBxDFMaxDeltaLobes; ++i)
            deltaLobes[i] = BxDFDeltaLobe::make();

        nonDeltaPart = pDiffuseReflection + pDiffuseTransmission;
        if (specularReflection.alpha > 0.0)
            nonDeltaPart += pSpecularReflection;
        if (specularReflectionTransmission.alpha > 0.0)
            nonDeltaPart += pSpecularReflectionTransmission;

        if ((pSpecularReflection + pSpecularReflectionTransmission) == 0.0 || psdExclude)
            return;

        BxDFDeltaLobe deltaTransmission = BxDFDeltaLobe::make();
        BxDFDeltaLobe deltaReflection   = BxDFDeltaLobe::make();
        deltaTransmission.transmission  = 1u;
        deltaReflection.transmission    = 0u;
        deltaReflection.dir             = float3(-wi.x, -wi.y, wi.z);

        if (specularReflection.alpha == 0.0 && specularReflection.hasLobe(kLobeTypeDeltaReflection))
        {
            deltaReflection.probability = pSpecularReflection;
            deltaReflection.thp = (1.0 - pSpecularReflectionTransmission) *
                evalFresnelSchlick(specularReflection.albedo, float3(1.0, 1.0, 1.0), wi.z);
        }

        if (specularReflectionTransmission.alpha == 0.0)
        {
            const bool hasReflection   = specularReflectionTransmission.hasLobe(kLobeTypeDeltaReflection);
            const bool hasTransmission = specularReflectionTransmission.hasLobe(kLobeTypeDeltaTransmission);
            if (hasReflection || hasTransmission)
            {
                float cosThetaT;
                float F = evalFresnelDielectric(specularReflectionTransmission.eta, wi.z, cosThetaT);

                if (hasReflection)
                {
                    const float localProbability = pSpecularReflectionTransmission * F;
                    deltaReflection.thp += float3(localProbability, localProbability, localProbability);
                    deltaReflection.probability += localProbability;
                }

                if (hasTransmission)
                {
                    float actualEta = specularReflectionTransmission.eta;
                    if (specularReflectionTransmission.isThinSurface)
                    {
                        actualEta = 1.0;
                        F         = evalFresnelDielectric(actualEta, wi.z, cosThetaT);
                    }

                    const float localProbability = pSpecularReflectionTransmission * (1.0 - F);
                    deltaTransmission.dir         = float3(-wi.x * actualEta, -wi.y * actualEta, -cosThetaT);
                    deltaTransmission.thp         = specularReflectionTransmission.transmissionAlbedo * localProbability;
                    deltaTransmission.probability = localProbability;
                }
            }
        }

        deltaLobes[0] = deltaTransmission;
        deltaLobes[1] = deltaReflection;
    }

    float4 eval(const float3 wi, const float3 wo)
    {
        float3 diffuse = float3(0.0, 0.0, 0.0);
        float3 specular = float3(0.0, 0.0, 0.0);

        if (pDiffuseReflection > 0.0)
            diffuse += (1.0 - specTrans) * (1.0 - diffTrans) * diffuseReflection.eval(wi, wo);
        if (pDiffuseTransmission > 0.0)
            diffuse += (1.0 - specTrans) * diffTrans * diffuseTransmission.eval(wi, wo);
        if (pSpecularReflection > 0.0)
            specular += (1.0 - specTrans) * specularReflection.eval(wi, wo);
        if (pSpecularReflectionTransmission > 0.0)
            specular += specTrans * specularReflectionTransmission.eval(wi, wo);

        return float4(diffuse + specular, Average(specular));
    }

    float evalPdf(const float3 wi, const float3 wo)
    {
        float pdf = 0.0;
        if (pDiffuseReflection > 0.0)
            pdf += pDiffuseReflection * diffuseReflection.evalPdf(wi, wo);
        if (pDiffuseTransmission > 0.0)
            pdf += pDiffuseTransmission * diffuseTransmission.evalPdf(wi, wo);
        if (pSpecularReflection > 0.0)
            pdf += pSpecularReflection * specularReflection.evalPdf(wi, wo);
        if (pSpecularReflectionTransmission > 0.0)
            pdf += pSpecularReflectionTransmission * specularReflectionTransmission.evalPdf(wi, wo);
        return pdf;
    }

    bool sample(const float3 wi, out float3 wo, out float pdf, out float3 weight, out uint lobe, out float lobeP, float3 preGeneratedSample)
    {
        wo     = float3(0.0, 0.0, 0.0);
        weight = float3(0.0, 0.0, 0.0);
        pdf    = 0.0;
        lobe   = 0u;
        lobeP  = 0.0;

        bool valid = false;
        const float uSelect = preGeneratedSample.z;

        if (uSelect < pDiffuseReflection)
        {
            valid = diffuseReflection.sample(wi, wo, pdf, weight, lobe, lobeP, preGeneratedSample);
            weight /= pDiffuseReflection;
            weight *= (1.0 - specTrans) * (1.0 - diffTrans);
            pdf *= pDiffuseReflection;
            lobeP *= pDiffuseReflection;
            if (pSpecularReflection > 0.0)
                pdf += pSpecularReflection * specularReflection.evalPdf(wi, wo);
            if (pSpecularReflectionTransmission > 0.0)
                pdf += pSpecularReflectionTransmission * specularReflectionTransmission.evalPdf(wi, wo);
        }
        else if (uSelect < pDiffuseReflection + pDiffuseTransmission)
        {
            valid = diffuseTransmission.sample(wi, wo, pdf, weight, lobe, lobeP, preGeneratedSample);
            weight /= pDiffuseTransmission;
            weight *= (1.0 - specTrans) * diffTrans;
            pdf *= pDiffuseTransmission;
            lobeP *= pDiffuseTransmission;
            if (pSpecularReflectionTransmission > 0.0)
                pdf += pSpecularReflectionTransmission * specularReflectionTransmission.evalPdf(wi, wo);
        }
        else if (uSelect < pDiffuseReflection + pDiffuseTransmission + pSpecularReflection)
        {
            valid = specularReflection.sample(wi, wo, pdf, weight, lobe, lobeP, preGeneratedSample);
            weight /= pSpecularReflection;
            weight *= (1.0 - specTrans);
            pdf *= pSpecularReflection;
            lobeP *= pSpecularReflection;
            if (pDiffuseReflection > 0.0)
                pdf += pDiffuseReflection * diffuseReflection.evalPdf(wi, wo);
            if (pSpecularReflectionTransmission > 0.0)
                pdf += pSpecularReflectionTransmission * specularReflectionTransmission.evalPdf(wi, wo);
        }
        else if (pSpecularReflectionTransmission > 0.0)
        {
            float3 localSample = preGeneratedSample;
            localSample.z = clamp((uSelect - (pDiffuseReflection + pDiffuseTransmission + pSpecularReflection)) /
                                      pSpecularReflectionTransmission,
                                  0.0,
                                  0.99999994);
            valid = specularReflectionTransmission.sample(wi, wo, pdf, weight, lobe, lobeP, localSample);
            weight /= pSpecularReflectionTransmission;
            weight *= specTrans;
            pdf *= pSpecularReflectionTransmission;
            lobeP *= pSpecularReflectionTransmission;
            if (pDiffuseReflection > 0.0)
                pdf += pDiffuseReflection * diffuseReflection.evalPdf(wi, wo);
            if (pDiffuseTransmission > 0.0)
                pdf += pDiffuseTransmission * diffuseTransmission.evalPdf(wi, wo);
            if (pSpecularReflection > 0.0)
                pdf += pSpecularReflection * specularReflection.evalPdf(wi, wo);
        }

        if (!valid || ((lobe & kLobeTypeDelta) != 0u))
            pdf = 0.0;

        return valid;
    }
};

float getSpecularProbability(StandardBSDFData bsdfData, float3 wo)
{
    const MaterialHeader mtl = MakeMaterialHeader(bsdfData);
    FalcorBSDF bsdf = FalcorBSDF::make(mtl, bsdfData.N, wo, bsdfData);
    return bsdf.pSpecularReflection + bsdf.pSpecularReflectionTransmission;
}

// Evaluate f(Wo,Wi) * NoL and the single-sample MIS pdf for the given unit directions.
// specProb is kept for compatibility and does not affect FalcorBSDF evaluation.
void EvalBSDF(StandardBSDFData bsdfData, float3 wo, float3 wi, float specProb, out float3 f, out float pdf)
{
    f   = float3(0.0, 0.0, 0.0);
    pdf = 0.0;

    float3 tangent;
    float3 bitangent;
    BranchlessONB(bsdfData.N, tangent, bitangent);

    const float3 viewLocal = float3(dot(wo, tangent), dot(wo, bitangent), dot(bsdfData.N, wo));
    const float3 scatterLocal = float3(dot(wi, tangent), dot(wi, bitangent), dot(bsdfData.N, wi));
    if (viewLocal.z < kMinCosTheta || abs(scatterLocal.z) < kMinCosTheta)
        return;

    const MaterialHeader mtl = MakeMaterialHeader(bsdfData);
    FalcorBSDF bsdf = FalcorBSDF::make(mtl, bsdfData.N, wo, bsdfData);
    const float4 value = bsdf.eval(viewLocal, scatterLocal);
    f = value.rgb;
    pdf = bsdf.evalPdf(viewLocal, scatterLocal);
}

float4 EvalBSDFValue(StandardBSDFData bsdfData, float3 wo, float3 wi)
{
    float3 tangent, bitangent;
    BranchlessONB(bsdfData.N, tangent, bitangent);
    const float3 viewLocal = float3(dot(wo, tangent), dot(wo, bitangent), dot(bsdfData.N, wo));
    const float3 scatterLocal = float3(dot(wi, tangent), dot(wi, bitangent), dot(bsdfData.N, wi));
    if (viewLocal.z < kMinCosTheta || abs(scatterLocal.z) < kMinCosTheta)
        return 0.0;
    FalcorBSDF bsdf = FalcorBSDF::make(MakeMaterialHeader(bsdfData), bsdfData.N, wo, bsdfData);
    return bsdf.eval(viewLocal, scatterLocal);
}

// Candidate selection needs only the PDF. Keep full BSDF evaluation after visibility,
// matching RTXPT-fork PathTracerNEE::EvalSampleWeight / ProcessLightSample.
float EvalBSDFPdf(StandardBSDFData bsdfData, float3 wo, float3 wi)
{
    float3 tangent, bitangent;
    BranchlessONB(bsdfData.N, tangent, bitangent);
    const float3 viewLocal = float3(dot(wo, tangent), dot(wo, bitangent), dot(bsdfData.N, wo));
    const float3 scatterLocal = float3(dot(wi, tangent), dot(wi, bitangent), dot(bsdfData.N, wi));
    if (viewLocal.z < kMinCosTheta || abs(scatterLocal.z) < kMinCosTheta)
        return 0.0;
    const MaterialHeader mtl = MakeMaterialHeader(bsdfData);
    FalcorBSDF bsdf = FalcorBSDF::make(mtl, bsdfData.N, wo, bsdfData);
    return bsdf.evalPdf(viewLocal, scatterLocal);
}

// Importance-sample an incident direction Wi. preGeneratedSample.xy samples the chosen lobe,
// and preGeneratedSample.z selects between BSDF lobe families.
bool SampleBSDF(StandardBSDFData bsdfData, float3 wo, float3 preGeneratedSample,
                out float3 wi, out float3 weight, out float pdf, out uint lobe, out float lobeP)
{
    wi     = float3(0.0, 0.0, 0.0);
    weight = float3(0.0, 0.0, 0.0);
    pdf    = 0.0;
    lobe   = 0u;
    lobeP  = 0.0;

    float3 tangent;
    float3 bitangent;
    BranchlessONB(bsdfData.N, tangent, bitangent);

    const float3 viewLocal = float3(dot(wo, tangent), dot(wo, bitangent), dot(bsdfData.N, wo));
    if (viewLocal.z < kMinCosTheta)
        return false;

    float3 scatterLocal;
    const MaterialHeader mtl = MakeMaterialHeader(bsdfData);
    FalcorBSDF bsdf = FalcorBSDF::make(mtl, bsdfData.N, wo, bsdfData);
    const bool valid = bsdf.sample(viewLocal, scatterLocal, pdf, weight, lobe, lobeP, preGeneratedSample);
    if (!valid)
        return false;

    wi = normalize(tangent * scatterLocal.x + bitangent * scatterLocal.y + bsdfData.N * scatterLocal.z);
    return IsFiniteVector(wi) && IsFiniteVector(weight) && IsFiniteScalar(pdf);
}

#endif // __BXDF_HLSLI__
