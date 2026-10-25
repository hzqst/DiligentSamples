#ifndef __LIGHT_SAMPLER_HLSLI__
#define __LIGHT_SAMPLER_HLSLI__

#include "../PathTracerBridge.hlsli"
#include "../BSDFs/BxDF.hlsli"
#include "../Utils/SampleGenerators.hlsli"
#include "PolymorphicLight.hlsli"
#include "LightSamplingMIS.hlsli"
#include "EnvMap.hlsli"

struct DirectLightSample
{
    float3 dir;
    float  distance;
    float3 radianceOverPdf;
    float  selectionPdf;
    float  solidAnglePdf;
    uint   index;
    bool   sampleableByBSDF;
    bool   fromLocalDistribution;

    bool Valid()
    {
        return any(radianceOverPdf > 0.0);
    }
};

DirectLightSample DirectLightSample_make_empty()
{
    DirectLightSample sample;
    sample.dir              = float3(0.0, 1.0, 0.0);
    sample.distance         = 0.0;
    sample.radianceOverPdf  = float3(0.0, 0.0, 0.0);
    sample.selectionPdf    = 0.0;
    sample.solidAnglePdf   = 0.0;
    sample.index           = RTXPT_INVALID_LIGHT_INDEX;
    sample.sampleableByBSDF = false;
    sample.fromLocalDistribution = false;
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
    const uint lightCount = ctrl.TotalLightCount - ctrl.LightClusterCount;
    if (lightCount == 0u)
    {
        pdf = 0.0;
        return RTXPT_INVALID_LIGHT_INDEX;
    }

    const uint lightIndex = min(uint(randomValue * float(lightCount)), lightCount - 1u);
    pdf                   = 1.0 / float(lightCount);
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
        return lightIndex < ctrl.TotalLightCount - ctrl.LightClusterCount ? 1.0 / float(ctrl.TotalLightCount - ctrl.LightClusterCount) : 0.0;

    if (ctrl.SamplingProxyCount == 0u)
        return 0.0;

    // Match RTXPT-fork LightSampler::SampleGlobalPDF: use the real per-light proxy count (no max(1u, ...)).
    // A zero-proxy light (sub-threshold weight) is never selected by NEE, so it must report pdf 0 here;
    // otherwise the BSDF-hit MIS weight is wrongly reduced and dim emitters lose energy (darkening).
    float scale = 1.0;
    if (ctrl.LightClusterCount != 0u && t_Lights[lightIndex].IsPartOfLightCluster())
    {
        const uint membership = t_LightsEx[lightIndex].ClusterIndexAndCount;
        scale = 1.0 / float(PolymorphicLightInfoEx::UnpackClusterLightCount(membership));
        lightIndex = PolymorphicLightInfoEx::UnpackClusterIndex(membership);
    }
    return scale * float(t_LightProxyCounters[lightIndex]) / float(ctrl.SamplingProxyCount);
}

float SampleLocalPDF(uint2 pixelPos, uint lightIndex)
{
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (ctrl.LocalSamplingResolution.x == 0u || ctrl.LocalSamplingResolution.y == 0u)
        return 0.0;

    const uint2 tilePos = min(GetLocalTilePos(pixelPos), ctrl.LocalSamplingResolution - 1u);
    const uint   base   = LLSB_ComputeBaseAddress(tilePos, ctrl.LocalSamplingResolution);
    const uint packed = LocalLightBinarySearch(t_LocalSamplingBuffer, base, lightIndex);
    float count = packed == RTXPT_INVALID_LIGHT_INDEX ? 0.0 : float(UnpackMiniListCount(packed));
    if (ctrl.LightClusterCount != 0u && lightIndex < ctrl.TotalLightCount - ctrl.LightClusterCount)
    {
        const uint membership = t_LightsEx[lightIndex].ClusterIndexAndCount;
        if (membership != RTXPT_INVALID_LIGHT_INDEX)
        {
            const uint clusterPacked = LocalLightBinarySearch(t_LocalSamplingBuffer, base, PolymorphicLightInfoEx::UnpackClusterIndex(membership));
            if (clusterPacked != RTXPT_INVALID_LIGHT_INDEX)
                count += float(UnpackMiniListCount(clusterPacked)) / float(PolymorphicLightInfoEx::UnpackClusterLightCount(membership));
        }
    }
    return count / float(RTXPT_LIGHTING_LOCAL_PROXY_COUNT);
}

bool UseApproximateLightMIS()
{
#ifdef RTXPT_USE_APPROXIMATE_MIS
    return RTXPT_USE_APPROXIMATE_MIS != 0;
#else
    return g_Const.ptConsts.NEEMISType == 2u ||
        (g_Const.ptConsts.NEEMISType == 1u && PATH_TRACER_MODE != PATH_TRACER_MODE_REFERENCE);
#endif
}

void ComputeLightSelectionPdfs(DirectLightSample sample, uint localCount, uint globalCount,
                               uint2 pixelPos, out float thisPdf, out float otherPdf, out float thisCount)
{
    thisPdf = sample.selectionPdf;
    if (sample.fromLocalDistribution)
    {
        otherPdf = SampleGlobalPDF(sample.index);
        thisCount = float(localCount);
    }
    else
    {
        otherPdf = localCount > 0u ? SampleLocalPDF(pixelPos, sample.index) : 0.0;
        thisCount = float(globalCount);
    }
}

float ComputeLightVsBSDFMISForBSDF(uint2 pixelPos, uint lightIndex, float bsdfPdf, float solidAnglePdf,
                                  uint candidates, uint fullSamples, bool coherent)
{
    if (bsdfPdf <= 0.0 || fullSamples == 0u)
        return 1.0;
    if (UseApproximateLightMIS())
        return LightVsBSDFMISApprox(bsdfPdf, candidates, fullSamples, false);
    if (lightIndex == RTXPT_INVALID_LIGHT_INDEX)
        return 1.0;
    uint localCount, globalCount;
    GetCandidateSampleCounts(Bridge::getLightingControl(), coherent, candidates, localCount, globalCount);
    const float lightPdf = (SampleGlobalPDF(lightIndex) + (localCount > 0u ? SampleLocalPDF(pixelPos, lightIndex) : 0.0)) * float(fullSamples) * solidAnglePdf;
    return EvalLightSamplingMIS(1.0, bsdfPdf, 1.0, lightPdf);
}

DirectLightSample GenerateDirectLightCandidate(float3 hitPos,
                                               uint2 pixelPos, bool fromLocalDistribution, inout SampleGenerator sg)
{
    DirectLightSample sample = DirectLightSample_make_empty();
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (ctrl.TotalLightCount == 0u)
        return sample;

    float selectionPdf = 0.0;
    uint  lightIndex   = RTXPT_INVALID_LIGHT_INDEX;

    if (fromLocalDistribution)
    {
        lightIndex = SampleLocalLightIndex(pixelPos, sampleNext1D(sg), selectionPdf);
    }
    else
    {
        lightIndex = SampleGlobalLightIndex(sampleNext1D(sg), selectionPdf);
    }

    if (lightIndex == RTXPT_INVALID_LIGHT_INDEX || selectionPdf <= 0.0)
        return sample;
    if (ctrl.LightClusterCount != 0u)
    {
        const PolymorphicLightInfo selected = t_Lights[lightIndex];
        if (DecodeLightType(selected) == kPolymorphicLightTypeCluster)
        {
            const uint count = ((selected.Direction1 >> 23) & 63u) + 1u;
            lightIndex = (selected.Direction1 & 0x7fffffu) + min(uint(sampleNext1D(sg) * count), count - 1u);
        }
        // Local history can contain both a cluster and a direct leaf. Evaluate
        // their combined probability, including after an LOD activation change.
        selectionPdf = fromLocalDistribution ? SampleLocalPDF(pixelPos, lightIndex) : SampleGlobalPDF(lightIndex);
        if (selectionPdf <= 0.0) return sample;
    }
    sample.selectionPdf = selectionPdf;
    sample.fromLocalDistribution = fromLocalDistribution;

    const PolymorphicLightInfoFull packedLight = Bridge::LoadLight(lightIndex);
    const uint lightType = DecodeLightType(packedLight.Base);
    const uint analyticEnd = ctrl.EnvmapQuadNodeCount + ctrl.AnalyticLightCount;
    if (lightType == kPolymorphicLightTypeEnvironment)
    {
        if ((g_Const.ptConsts.environmentNEEEnabled & 1u) == 0u)
            return DirectLightSample_make_empty();
        const EnvironmentQuadLight light = UnpackEnvironmentLight(packedLight.Base);
        const float2 uv = (float2(light.NodeX, light.NodeY) + sampleNext2D(sg)) / float(light.NodeDim);
        EnvMapSampler env = RTXPTCreateEnvMapSampler(Bridge::getEnvMapConstants());
        sample.dir = env.ToWorld(RTXPTEnvOctToDirEqualArea(uv));
        sample.distance = 100000.0; // RTXPT-fork DISTANT_LIGHT_DISTANCE.
        sample.solidAnglePdf = EnvironmentQuadSolidAnglePdf(light);
        sample.radianceOverPdf = light.Radiance / (selectionPdf * sample.solidAnglePdf);
        sample.index = lightIndex;
        sample.sampleableByBSDF = true;
    }
    else if (lightType != kPolymorphicLightTypeTriangle)
    {
        const LightSample light = SampleAnalyticLight(UnpackAnalyticLight(packedLight), sampleNext2D(sg), hitPos);
        if (!light.valid || light.solidAnglePdf <= 0.0)
            return sample;

        const float lightPdf = selectionPdf * light.solidAnglePdf;
        if (lightPdf <= 0.0)
            return sample;

        sample.dir              = light.dir;
        sample.distance         = light.distance;
        sample.radianceOverPdf  = light.radiance * max(g_Const.ptConsts.lightIntensityScale, 0.0) / lightPdf;
        sample.solidAnglePdf    = light.solidAnglePdf;
        sample.index            = lightIndex;
        sample.sampleableByBSDF = false;
    }
    else if (Bridge::getEmissiveTriangleCount() > 0u)
    {
        // Per-triangle selection (RTXPT-fork model): the global proxy table already chose this specific
        // emissive triangle with probability proportional to its power, so the unified light index maps
        // directly to a triangle and selectionPdf is already the per-triangle selection probability
        // (no extra uniform 1/triCount factor).
        const uint triCount = Bridge::getEmissiveTriangleCount();
        const uint triIndex = lightIndex - analyticEnd;
        if (triIndex >= triCount)
            return sample;
        const EmissiveTriangle tri = UnpackTriangleLight(packedLight.Base);

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

        sample.dir              = wi;
        sample.distance         = dist;
        sample.radianceOverPdf  = tri.radiance.rgb / trianglePdf;
        sample.solidAnglePdf    = solidAnglePdf;
        sample.index            = lightIndex; // unified light index (used for temporal feedback)
        sample.sampleableByBSDF = true;
    }

    return sample;
}

void InsertFeedbackFromNEE(uint2 pixelPos, uint lightIndex, float3 contribution, float randomValue, bool coherent)
{
    const LightingControlData ctrl = Bridge::getLightingControl();
    if (ctrl.TemporalFeedbackRequired == 0u || lightIndex == RTXPT_INVALID_LIGHT_INDEX)
        return;

    const float luminance = max(0.0, dot(contribution, float3(0.2126, 0.7152, 0.0722)));
    if (luminance <= 0.0)
        return;

    float feedbackWeight = luminance;
    float globalPdf = SampleGlobalPDF(lightIndex);
    if (ctrl.LightClusterCount != 0u && t_Lights[lightIndex].IsPartOfLightCluster())
    {
        const uint membership = t_LightsEx[lightIndex].ClusterIndexAndCount;
        // The proposal PDF remains the sampled triangle's PDF after unwrapping.
        // Only the feedback destination changes to the active cluster.
        lightIndex = PolymorphicLightInfoEx::UnpackClusterIndex(membership);
    }
    feedbackWeight = globalPdf > 0.0 ? feedbackWeight / pow(globalPdf, 0.65) : LFR_MAX_WEIGHT;

    LightFeedbackReservoir reservoir = LightFeedbackReservoir::make(pixelPos, u_FeedbackTotalWeight, u_FeedbackCandidates);
    reservoir.Add(randomValue, lightIndex, feedbackWeight, coherent);
}

float EvalDirectLightCandidateWeight(DirectLightSample sample, SurfaceBSDFData bsdfData, float3 wo)
{
    if (!sample.Valid())
        return 0.0;

    // The PDF is a temporary weight input, not part of the reservoir candidate.
    const float bsdfPdf = EvalBSDFPdf(bsdfData, wo, sample.dir);
    return max(sample.radianceOverPdf.x, max(sample.radianceOverPdf.y, sample.radianceOverPdf.z)) *
        bsdfPdf;
}

#endif // __LIGHT_SAMPLER_HLSLI__
