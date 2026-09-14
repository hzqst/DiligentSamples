#ifndef __LIGHT_SAMPLER_HLSLI__
#define __LIGHT_SAMPLER_HLSLI__

#include "../PathTracerBridge.hlsli"
#include "../Rendering/Materials/BxDF.hlsli"
#include "../Utils/SampleGenerators.hlsli"
#include "PolymorphicLight.hlsli"

struct DirectLightSample
{
    float3 dir;
    float  distance;
    float3 radianceOverPdf;
    float  proposalPdf;
    float3 bsdfF;
    float  bsdfPdf;
    uint   kind;
    uint   index;
    bool   valid;
    bool   sampleableByBSDF;
};

DirectLightSample DirectLightSample_make_empty()
{
    DirectLightSample sample;
    sample.dir              = float3(0.0, 1.0, 0.0);
    sample.distance         = 0.0;
    sample.radianceOverPdf  = float3(0.0, 0.0, 0.0);
    sample.proposalPdf      = 0.0;
    sample.bsdfF            = float3(0.0, 0.0, 0.0);
    sample.bsdfPdf          = 0.0;
    sample.kind            = kLightProxyKindAnalytic;
    sample.index           = 0u;
    sample.valid           = false;
    sample.sampleableByBSDF = false;
    return sample;
}

struct NEEWeightedReservoirSampler
{
    DirectLightSample candidate;
    float             weightSum;
    float             candidateWeight;

    static NEEWeightedReservoirSampler make()
    {
        NEEWeightedReservoirSampler sampler;
        sampler.candidate       = DirectLightSample_make_empty();
        sampler.weightSum       = 0.0;
        sampler.candidateWeight = 0.0;
        return sampler;
    }

    void Add(float randomValue, DirectLightSample sample, float targetWeight)
    {
        if (targetWeight <= 0.0)
            return;

        weightSum += targetWeight;
        if (randomValue * weightSum <= targetWeight)
        {
            candidate       = sample;
            candidateWeight = targetWeight;
        }
    }

    float CandidateProbability()
    {
        return weightSum > 0.0 ? candidateWeight / weightSum : 0.0;
    }
};

uint SampleUniformLightIndex(float randomValue, out float pdf)
{
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (ctrl.TotalLightCount == 0u)
    {
        pdf = 0.0;
        return RTXPT_INVALID_LIGHT_INDEX;
    }

    const uint lightIndex = min(uint(randomValue * float(ctrl.TotalLightCount)), ctrl.TotalLightCount - 1u);
    pdf                   = 1.0 / float(ctrl.TotalLightCount);
    return lightIndex;
}

uint SampleGlobalLightIndex(float randomValue, out float pdf)
{
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (ctrl.TotalLightCount == 0u)
    {
        pdf = 0.0;
        return RTXPT_INVALID_LIGHT_INDEX;
    }

    if (ctrl.ImportanceSamplingType == 0u)
        return SampleUniformLightIndex(randomValue, pdf);

    if (ctrl.SamplingProxyCount == 0u)
    {
        pdf = 0.0;
        return RTXPT_INVALID_LIGHT_INDEX;
    }

    const uint proxyIndex = min(uint(randomValue * float(ctrl.SamplingProxyCount)), ctrl.SamplingProxyCount - 1u);
    const uint lightIndex = t_LightSamplingProxies[proxyIndex];
    const uint proxyCount = t_LightProxyCounters[lightIndex]; // light came from the proxy table, so count >= 1
    pdf                   = float(proxyCount) / float(ctrl.SamplingProxyCount);
    return lightIndex;
}

uint2 GetLocalTilePos(uint2 pixelPos)
{
    const LightingControlData ctrl = Bridge::getLightingControl();
    return (pixelPos + ctrl.LocalSamplingTileJitter) / RTXPT_LIGHTING_SAMPLING_BUFFER_TILE_SIZE.xx;
}

bool HasLocalLightSampling(LightingControlData ctrl)
{
    return ctrl.ImportanceSamplingType == 2u &&
        saturate(ctrl.LocalToGlobalSampleRatio) > 0.0 &&
        ctrl.SamplingProxyCount > 0u &&
        ctrl.LocalSamplingResolution.x > 0u &&
        ctrl.LocalSamplingResolution.y > 0u;
}

uint SampleLocalLightIndex(uint2 pixelPos, float randomValue, out float pdf)
{
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (ctrl.LocalSamplingResolution.x == 0u || ctrl.LocalSamplingResolution.y == 0u)
    {
        pdf = 0.0;
        return RTXPT_INVALID_LIGHT_INDEX;
    }

    const uint2 tilePos = min(GetLocalTilePos(pixelPos), ctrl.LocalSamplingResolution - 1u);
    const uint   base   = LLSB_ComputeBaseAddress(tilePos, ctrl.LocalSamplingResolution);
    const uint   localIndex = min(uint(randomValue * float(RTXPT_LIGHTING_LOCAL_PROXY_COUNT)), RTXPT_LIGHTING_LOCAL_PROXY_COUNT - 1u);
    const uint   packed = t_LocalSamplingBuffer[base + localIndex];
    const uint   lightIndex = UnpackMiniListLight(packed);
    const uint   occurrenceCount = UnpackMiniListCount(packed);
    pdf = float(occurrenceCount) / float(RTXPT_LIGHTING_LOCAL_PROXY_COUNT);
    return lightIndex;
}

float SampleGlobalPDF(uint lightIndex)
{
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (ctrl.TotalLightCount == 0u || lightIndex >= ctrl.TotalLightCount)
        return 0.0;

    if (ctrl.ImportanceSamplingType == 0u)
        return 1.0 / float(ctrl.TotalLightCount);

    if (ctrl.SamplingProxyCount == 0u)
        return 0.0;

    // Match RTXPT-fork LightSampler::SampleGlobalPDF: use the real per-light proxy count (no max(1u, ...)).
    // A zero-proxy light (sub-threshold weight) is never selected by NEE, so it must report pdf 0 here;
    // otherwise the BSDF-hit MIS weight is wrongly reduced and dim emitters lose energy (darkening).
    return float(t_LightProxyCounters[lightIndex]) / float(ctrl.SamplingProxyCount);
}

float SampleLocalPDF(uint2 pixelPos, uint lightIndex)
{
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (ctrl.LocalSamplingResolution.x == 0u || ctrl.LocalSamplingResolution.y == 0u)
        return 0.0;

    const uint2 tilePos = min(GetLocalTilePos(pixelPos), ctrl.LocalSamplingResolution - 1u);
    const uint   base   = LLSB_ComputeBaseAddress(tilePos, ctrl.LocalSamplingResolution);
    const uint packed = LocalLightBinarySearch(t_LocalSamplingBuffer, base, lightIndex);
    return packed == RTXPT_INVALID_LIGHT_INDEX ? 0.0 : float(UnpackMiniListCount(packed)) / float(RTXPT_LIGHTING_LOCAL_PROXY_COUNT);
}

float GetLightSelectionPdf(uint2 pixelPos, uint lightIndex)
{
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (lightIndex == RTXPT_INVALID_LIGHT_INDEX || lightIndex >= ctrl.TotalLightCount)
        return 0.0;

    const float globalPdf = SampleGlobalPDF(lightIndex);
    if (!HasLocalLightSampling(ctrl))
        return globalPdf;

    const float localRatio = saturate(ctrl.LocalToGlobalSampleRatio);
    const float localPdf   = SampleLocalPDF(pixelPos, lightIndex);
    return localRatio * localPdf + (1.0 - localRatio) * globalPdf;
}

DirectLightSample GenerateDirectLightCandidate(StandardBSDFData bsdfData, float3 hitPos, float3 wo,
                                               uint2 pixelPos, inout SampleGenerator sg)
{
    DirectLightSample sample = DirectLightSample_make_empty();
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (ctrl.TotalLightCount == 0u)
        return sample;

    float selectionPdf = 0.0;
    uint  lightIndex   = RTXPT_INVALID_LIGHT_INDEX;

    const float localRatio = saturate(ctrl.LocalToGlobalSampleRatio);
    const bool  hasLocal   = HasLocalLightSampling(ctrl);
    if (hasLocal && sampleNext1D(sg) < localRatio)
    {
        lightIndex = SampleLocalLightIndex(pixelPos, sampleNext1D(sg), selectionPdf);
        selectionPdf = localRatio * selectionPdf + (1.0 - localRatio) * SampleGlobalPDF(lightIndex);
    }
    else
    {
        lightIndex = SampleGlobalLightIndex(sampleNext1D(sg), selectionPdf);
        if (hasLocal)
            selectionPdf = localRatio * SampleLocalPDF(pixelPos, lightIndex) + (1.0 - localRatio) * selectionPdf;
    }

    if (lightIndex == RTXPT_INVALID_LIGHT_INDEX || selectionPdf <= 0.0)
        return sample;

    if (lightIndex < ctrl.AnalyticLightCount)
    {
        const LightSample light = SampleAnalyticLight(Bridge::getLight(lightIndex), sampleNext2D(sg), hitPos);
        if (!light.valid || light.solidAnglePdf <= 0.0)
            return sample;

        const float lightPdf = selectionPdf * light.solidAnglePdf;
        if (lightPdf <= 0.0)
            return sample;

        const float  bsdfProb = getSpecularProbability(bsdfData, wo);
        float3       f;
        float        bsdfPdf;
        EvalBSDF(bsdfData, wo, light.dir, bsdfProb, f, bsdfPdf);

        sample.dir              = light.dir;
        sample.distance         = light.distance;
        sample.radianceOverPdf  = light.radiance * max(g_Const.ptConsts.lightIntensityScale, 0.0) / lightPdf;
        sample.proposalPdf      = lightPdf;
        sample.bsdfF            = f;
        sample.bsdfPdf          = bsdfPdf;
        sample.kind             = kLightProxyKindAnalytic;
        sample.index            = lightIndex;
        sample.valid            = true;
        sample.sampleableByBSDF = false;
    }
    else if (lightIndex >= ctrl.AnalyticLightCount && Bridge::getEmissiveTriangleCount() > 0u)
    {
        // Per-triangle selection (RTXPT-fork model): the global proxy table already chose this specific
        // emissive triangle with probability proportional to its power, so the unified light index maps
        // directly to a triangle and selectionPdf is already the per-triangle selection probability
        // (no extra uniform 1/triCount factor).
        const uint triCount = Bridge::getEmissiveTriangleCount();
        const uint triIndex = lightIndex - ctrl.AnalyticLightCount;
        if (triIndex >= triCount)
            return sample;
        const EmissiveTriangle tri = Bridge::getEmissiveTriangle(triIndex);

        const float3 ng    = cross(tri.edge1.xyz, tri.edge2.xyz);
        const float  ngLen = length(ng);
        if (ngLen <= 0.0)
            return sample;

        const float  area   = 0.5 * ngLen;
        const float3 normal = ng / ngLen;
        const float3 bary   = SampleTriangleUniform(sampleNext2D(sg));
        const float3 P      = tri.base.xyz + tri.edge1.xyz * bary.y + tri.edge2.xyz * bary.z;

        const float3 toLight = P - hitPos;
        const float  distSq  = max(1e-9, dot(toLight, toLight));
        const float  dist    = sqrt(distSq);
        const float3 wi      = toLight / dist;
        // One-sided emitter: only the front face (normal hemisphere) radiates, matching RTXPT-fork
        // TriangleLight::CalcSample (PolymorphicLight.hlsli) and the closest-hit frontFacing emission gate.
        const float  cosTheta = dot(normal, -wi);
        if (cosTheta <= 2e-9)
            return sample;

        const float solidAnglePdf = min(kMaxSolidAnglePdf, pdfAtoW(1.0 / area, dist, cosTheta));
        const float trianglePdf   = selectionPdf * solidAnglePdf;
        if (trianglePdf <= 0.0)
            return sample;

        const float bsdfProb = getSpecularProbability(bsdfData, wo);
        float3      f;
        float       bsdfPdf;
        EvalBSDF(bsdfData, wo, wi, bsdfProb, f, bsdfPdf);

        sample.dir              = wi;
        sample.distance         = dist;
        sample.radianceOverPdf  = tri.radiance.rgb / trianglePdf;
        sample.proposalPdf      = trianglePdf;
        sample.bsdfF            = f;
        sample.bsdfPdf          = bsdfPdf;
        sample.kind             = kLightProxyKindEmissiveTriangle;
        sample.index            = lightIndex; // unified light index (used for temporal feedback)
        sample.valid            = true;
        sample.sampleableByBSDF = true;
    }

    if (!sample.valid || sample.bsdfPdf <= 0.0)
        return DirectLightSample_make_empty();

    return sample;
}

void InsertFeedbackFromNEE(uint2 pixelPos, uint lightIndex, float3 contribution, float randomValue)
{
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (ctrl.TemporalFeedbackRequired == 0u || lightIndex == RTXPT_INVALID_LIGHT_INDEX)
        return;

    const float avgContribution = max(0.0, dot(contribution, float3(0.2126, 0.7152, 0.0722)));
    if (avgContribution <= 0.0)
        return;

    float feedbackWeight = avgContribution;
    const float globalPdf = max(SampleGlobalPDF(lightIndex), 1e-6);
    feedbackWeight /= pow(globalPdf, max(ctrl.GlobalFeedbackUseWeight, 0.0));

    LightFeedbackReservoir reservoir = LightFeedbackReservoir::make(pixelPos, u_FeedbackTotalWeight, u_FeedbackCandidates);
    reservoir.Add(randomValue, lightIndex, feedbackWeight, true);
}

float EvalDirectLightCandidateWeight(DirectLightSample sample)
{
    if (!sample.valid || sample.proposalPdf <= 0.0 || sample.bsdfPdf <= 0.0)
        return 0.0;

    return max(sample.radianceOverPdf.x, max(sample.radianceOverPdf.y, sample.radianceOverPdf.z)) *
        sample.bsdfPdf;
}

#endif // __LIGHT_SAMPLER_HLSLI__
