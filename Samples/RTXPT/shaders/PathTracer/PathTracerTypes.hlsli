#ifndef __PATH_TRACER_TYPES_HLSLI__
#define __PATH_TRACER_TYPES_HLSLI__

#include "Config.h"
#include "PathTracerHelpers.hlsli"
#include "BSDFs/BxDF.hlsli"
#include "PathState.hlsli"
#include "PathPayload.hlsli"
#include "DenoisingLayers.hlsli"
#include "Surface/ShadingData.hlsli"

static const uint cMaxDeltaLobes = 3;

struct DeltaLobe
{
    float3 dir;
    float3 thp;
    uint   transmission;
    float  probability;

    static DeltaLobe make()
    {
        DeltaLobe ret;
        ret.dir          = float3(0.0, 0.0, 1.0);
        ret.thp          = float3(0.0, 0.0, 0.0);
        ret.transmission = 0u;
        ret.probability  = 0.0;
        return ret;
    }
};

struct BSDFSample
{
    uint   lobe;
    uint   deltaLobeIndex;
    float  pdf;
    float  lobeP;
    float3 weight;
    float3 wi;

    uint getDeltaLobeIndex() { return deltaLobeIndex; }
    bool isLobe(uint testLobe) { return (lobe & testLobe) != 0u; }
};

namespace PathTracer
{

    struct ActiveBSDF
    {
        SurfaceBSDFData    standardData;

        void evalDeltaLobes(ShadingData shadingData,
                            out DeltaLobe deltaLobes[cMaxDeltaLobes],
                            out uint deltaLobeCount,
                            out float nonDeltaPart)
        {
            for (uint i = 0u; i < cMaxDeltaLobes; ++i)
                deltaLobes[i] = DeltaLobe::make();

            const MaterialHeader mtl = MakeMaterialHeader(standardData);
            FalcorBSDF bsdf = FalcorBSDF::make(mtl, shadingData.N, shadingData.V, standardData);

            BxDFDeltaLobe localLobes[cBxDFMaxDeltaLobes];
            uint localCount;
            const float3 viewLocal = float3(dot(shadingData.V, shadingData.T),
                                            dot(shadingData.V, shadingData.B),
                                            dot(shadingData.V, shadingData.N));
            bsdf.evalDeltaLobes(viewLocal, localLobes, localCount, nonDeltaPart);

            deltaLobeCount = min(localCount, cMaxDeltaLobes);
            for (uint i = 0u; i < deltaLobeCount; ++i)
            {
                deltaLobes[i].dir = normalize(shadingData.T * localLobes[i].dir.x +
                                               shadingData.B * localLobes[i].dir.y +
                                               shadingData.N * localLobes[i].dir.z);
                deltaLobes[i].thp          = localLobes[i].thp;
                deltaLobes[i].transmission = localLobes[i].transmission;
                deltaLobes[i].probability  = localLobes[i].probability;
            }
        }

        void estimateSpecDiffBSDF(out float3 diffBSDFEstimate, out float3 specBSDFEstimate, float3 normal, float3 view)
        {
            const lpfloat dataDiffuseTransmission  = standardData.DiffuseTransmission();
            const lpfloat dataSpecularTransmission = standardData.SpecularTransmission();
            const lpfloat3 dataTransmission        = standardData.Transmission();

            const lpfloat3 diffuseReflectionAlbedo =
                (lpfloat(1.0) - dataDiffuseTransmission) *
                (lpfloat(1.0) - dataSpecularTransmission) *
                standardData.Diffuse();
            const lpfloat3 diffuseTransmissionAlbedo =
                dataDiffuseTransmission *
                dataTransmission *
                (lpfloat(1.0) - dataSpecularTransmission);
            const lpfloat3 specularReflectionAlbedo =
                (lpfloat(1.0) - dataSpecularTransmission) *
                standardData.Specular();
            const lpfloat3 specularTransmissionAlbedo =
                dataSpecularTransmission *
                dataTransmission;

            diffBSDFEstimate = diffuseReflectionAlbedo + diffuseTransmissionAlbedo;

            const lpfloat dataRoughness = standardData.Roughness();
            const float alpha         = dataRoughness * dataRoughness;
            const float roughness     = alpha < kMinGGXAlpha ? 0.0 : dataRoughness;
            const float NdotV         = saturate(dot(normal, view));
            const float ggxAlpha      = roughness * roughness;
            specBSDFEstimate = approxSpecularIntegralGGX(specularReflectionAlbedo, ggxAlpha, NdotV) +
                specularTransmissionAlbedo;
        }
    };

    struct WorkingContext
    {
        RWTexture2D<float4> OutputColor;
        RWTexture2D<float>  Depth;
        RWTexture2D<float4> MotionVectors;
        PathTracerConstants PtConsts;

#if PATH_TRACER_MODE != PATH_TRACER_MODE_REFERENCE || defined(__INTELLISENSE__)
        DenoisingLayersContext DenoisingLayers;
#endif
    };

    struct SurfaceData
    {
        ShadingData shadingData;
        ActiveBSDF             bsdf;
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
        float3 prevPosW;
        bool previousSurfaceValid;
        // The surface did not move or change its material since the previous frame.
        bool previousSurfaceUnchanged;
        // Zero normal means the reflector has no supported previous plane.
        float4 previousReflectionPlane;
#endif
        uint    neeTriangleLightIndex;
        uint    neeAnalyticLightIndex;
        float   emissiveLightPdf;

        static SurfaceData make(ShadingData shadingData,
                                ActiveBSDF bsdf,
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
                                float3 prevPosW,
                                bool previousSurfaceValid,
                                bool previousSurfaceUnchanged,
                                float4 previousReflectionPlane,
#endif
                                uint neeTriangleLightIndex,
                                uint neeAnalyticLightIndex,
                                float emissiveLightPdf)
        {
            SurfaceData ret;
            ret.shadingData           = shadingData;
            ret.bsdf                  = bsdf;
#if PATH_TRACER_MODE == PATH_TRACER_MODE_BUILD_DENOISING_LAYERS
            ret.prevPosW              = prevPosW;
            ret.previousSurfaceValid = previousSurfaceValid;
            ret.previousSurfaceUnchanged = previousSurfaceUnchanged;
            ret.previousReflectionPlane = previousReflectionPlane;
#endif
            ret.neeTriangleLightIndex = neeTriangleLightIndex;
            ret.neeAnalyticLightIndex = neeAnalyticLightIndex;
            ret.emissiveLightPdf      = emissiveLightPdf;
            return ret;
        }
    };

    struct NEEBSDFMISInfo
    {
        bool LightSamplingEnabled;
#if PT_USE_RESTIR_DI
        bool SkipEmissiveBRDF;
#endif
        bool LightSamplingIsSSC;
        uint CandidateSamples;
        uint FullSamples;

        static NEEBSDFMISInfo empty()
        {
            NEEBSDFMISInfo ret;
            ret.LightSamplingEnabled = false;
#if PT_USE_RESTIR_DI
            ret.SkipEmissiveBRDF = false;
#endif
            ret.LightSamplingIsSSC = false;
            ret.CandidateSamples   = 0;
            ret.FullSamples        = 0;
            return ret;
        }

        static NEEBSDFMISInfo Unpack16bit(uint packed)
        {
            NEEBSDFMISInfo ret;
            ret.LightSamplingEnabled = (packed & (1 << 15)) != 0;
#if PT_USE_RESTIR_DI
            ret.SkipEmissiveBRDF = (packed & (1 << 14)) != 0;
#endif
            ret.LightSamplingIsSSC = (packed & (1 << 13)) != 0;
            ret.CandidateSamples   = (packed >> 6) & 0x3f;
            ret.FullSamples        = packed & 0x3f;
            return ret;
        }

        uint Pack16bit()
        {
            uint packed = 0;
            packed |= (LightSamplingEnabled ? 1 : 0) << 15;
#if PT_USE_RESTIR_DI
            packed |= (SkipEmissiveBRDF ? 1 : 0) << 14;
#endif
            packed |= (LightSamplingIsSSC ? 1 : 0) << 13;
            packed |= (CandidateSamples & 0x3f) << 6;
            packed |= FullSamples & 0x3f;
            return packed;
        }

        static const uint SampleCountLimit() { return (1 << 6) - 1; }

        static bool equals(const NEEBSDFMISInfo a, const NEEBSDFMISInfo b)
        {
            return (a.LightSamplingEnabled == b.LightSamplingEnabled) &&
#if PT_USE_RESTIR_DI
                (a.SkipEmissiveBRDF == b.SkipEmissiveBRDF) &&
#endif
                (a.LightSamplingIsSSC == b.LightSamplingIsSSC) &&
                (a.CandidateSamples == b.CandidateSamples) &&
                (a.FullSamples == b.FullSamples);
        }

#if PT_USE_RESTIR_DI
        bool GetSkipEmissiveBRDF() { return SkipEmissiveBRDF; }
#else
        bool GetSkipEmissiveBRDF() { return false; }
#endif
    };

#define RTXPT_NEE_RESULT_MANUAL_PACK 1

    struct NEEResult
    {
#if RTXPT_NEE_RESULT_MANUAL_PACK
        uint2 RadianceAndSpecAvgPkg;
#else
        float4 RadianceAndSpecAvg;
#endif
        NEEBSDFMISInfo BSDFMISInfo;

        static NEEResult empty()
        {
            NEEResult ret;
#if RTXPT_NEE_RESULT_MANUAL_PACK
            ret.RadianceAndSpecAvgPkg = Fp32ToFp16(float4(0, 0, 0, 0));
#else
            ret.RadianceAndSpecAvg = float4(0, 0, 0, 0);
#endif
            ret.BSDFMISInfo = NEEBSDFMISInfo::empty();
            return ret;
        }

        void AccumulateRadiance(const float3 radiance, const float specAvg)
        {
#if RTXPT_NEE_RESULT_MANUAL_PACK
            RadianceAndSpecAvgPkg = Fp32ToFp16(Fp16ToFp32(RadianceAndSpecAvgPkg) + float4(radiance, specAvg));
#else
            RadianceAndSpecAvg += float4(radiance, specAvg);
#endif
        }

        float4 GetRadianceAndSpecAvg()
        {
#if RTXPT_NEE_RESULT_MANUAL_PACK
            return Fp16ToFp32(RadianceAndSpecAvgPkg);
#else
            return RadianceAndSpecAvg;
#endif
        }
    };

    struct VisibilityPayload
    {
        uint missed;

        static VisibilityPayload make()
        {
            VisibilityPayload ret;
            ret.missed = 0;
            return ret;
        }
    };
}

#endif // __PATH_TRACER_TYPES_HLSLI__
