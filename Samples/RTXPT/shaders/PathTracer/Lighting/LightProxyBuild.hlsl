// Per-triangle, power-proportional light sampling proxy build (GPU).
//
// Replaces the CPU "emissive bucket" proxy build with RTXPT-fork's per-light power-proportional model:
// every analytic light AND every emissive triangle becomes an independent light with a weight derived from
// its power, and the sampling-proxy table is populated so each light is selected with probability
// proportional to its weight (see Lighting/LightSampler.hlsli SampleGlobalLightIndex).
//
// Unified index order: environment quads, analytic lights, emissive triangles.
//
// Pipeline (one Dispatch each, UAV barriers between):
//   ResetProxyBuildCS      - clear the GPU-computed control scalars
//   ComputeWeightsCS       - per-light weight + atomic weight-sum (RTXPT-fork ComputeWeights, simplified)
//   ComputeProxyCountsCS   - mix temporal feedback, allocate counts and 256-entry jobs
//   FinalizeProxyJobsCS    - build a bounded two-dimensional indirect dispatch
//   ScatterProxiesCS       - execute jobs in parallel (ExecuteProxyJobs)
//
// Atomic allocation replaces upstream's prefix sum while preserving per-light
// multiplicities. A large light spans many workgroups instead of one serial thread.

#ifndef __LIGHT_PROXY_BUILD_HLSL__
#define __LIGHT_PROXY_BUILD_HLSL__

#include "PathTracerShared.h"
#include "Lighting/LightingTypes.hlsli"
#include "Lighting/EnvironmentLight.hlsli"

struct LightProxyBuildConstants
{
    uint TotalLightCount;
    uint AnalyticLightCount;
    uint ProxyBudget;          // capacity of u_LightSamplingProxies (kProxyRatio * TotalLightCount)
    uint ImportanceSamplingType; // 0 = uniform (1 proxy/light), otherwise power-proportional
    uint HistoricWeightCount;
    uint HasHistoryRemap;
    uint HasFrustumPlanes;
    uint Padding;
    float4 FrustumPlanes[5];
    float3 CameraPosition;
    uint HasCameraPosition;
};

ConstantBuffer<LightProxyBuildConstants> g_LightProxyBuildConstants;

RWStructuredBuffer<LightingControlData> u_LightingControl;     // GPU writes WeightsSumUINT/SamplingProxyCount/ProxyBuildTaskCount
StructuredBuffer<PolymorphicLightInfo> t_Lights;
StructuredBuffer<PolymorphicLightInfoEx> t_LightsEx;
RWStructuredBuffer<PolymorphicLightInfo> u_Lights;
RWStructuredBuffer<PolymorphicLightInfoEx> u_LightsEx;
StructuredBuffer<uint> t_FeedbackCounts;
StructuredBuffer<uint> t_HistoryRemapCurrentToPast;
StructuredBuffer<float> t_HistoricLightWeights;
RWStructuredBuffer<float>               u_LightWeights;        // per-light weight [TotalLightCount]
RWStructuredBuffer<uint>                u_LightProxyCounters;  // per-light proxy count [TotalLightCount]
RWStructuredBuffer<uint>                u_LightSamplingProxies;// flat proxy -> light index table [ProxyBudget]
RWStructuredBuffer<uint4> u_ProxyJobs;
RWStructuredBuffer<uint> u_ProxyDispatchArgs;

static const float kLightProxyPi = 3.14159265358979323846;

float ProxyLuminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// RTXPT-fork sphere / point power, including the shaping flux factor.
float ComputeAnalyticWeight(PolymorphicLightInfoFull packed)
{
    const AnalyticLightData light = UnpackAnalyticLight(packed);
    const float luma             = ProxyLuminance(light.colorType.xyz);
    const float radius           = max(light.positionRadius.w, 0.0);
    const float shapingFlux = light.shaping.x < -0.5 ? 1.0 : (1.0 - light.shaping.x) * lerp(1.0, 0.5, light.shaping.y) * 0.5;
    float power = light.colorType.w == kPolymorphicLightTypePoint ? 4.0 * kLightProxyPi * luma :
                  4.0 * kLightProxyPi * kLightProxyPi * radius * radius * luma;
    power *= shapingFlux;
    // Diligent retains explicit distant lights; upstream bakes them into its environment.
    if (light.colorType.w == kPolymorphicLightTypeDirectional)
        power = luma * max(light.shaping.w, 1e-8);
    const float weight = pow(max(0.0, power), 0.8);
    return weight >= RTXPT_LIGHTING_MIN_WEIGHT_THRESHOLD ? weight : 0.0;
}

// Mirrors RTXPT-fork TriangleLight::GetPower (PolymorphicLight.hlsli) + ComputeWeight: power = area*PI*Luminance,
// shaped by pow(.,0.8). radiance already includes the baked emissive texture (EmissiveTriangleBuild.hlsl).
float ComputeEmissiveTriangleWeight(PolymorphicLightInfo packed)
{
    const EmissiveTriangle tri = UnpackTriangleLight(packed);
    const float3 ng    = cross(tri.edge1.xyz, tri.edge2.xyz);
    const float  area  = 0.5 * length(ng);
    const float  power = area * kLightProxyPi * ProxyLuminance(tri.radiance.rgb);
    float        weight = pow(max(0.0, power), 0.8);
    if (weight < RTXPT_LIGHTING_MIN_WEIGHT_THRESHOLD)
        weight = 0.0;
    return weight;
}

float ComputeLightWeight(uint lightIndex)
{
    PolymorphicLightInfoFull packed;
    packed.Base = t_Lights[lightIndex];
    packed.Extended = (PolymorphicLightInfoEx)0;
    const uint type = DecodeLightType(packed.Base);
    if (type == kPolymorphicLightTypeCluster) return 0.0;
    if (type == kPolymorphicLightTypeEnvironment)
    {
        const float weight = pow(max(0.0, asfloat(packed.Base.Scalars)), 0.8);
        return weight >= RTXPT_LIGHTING_MIN_WEIGHT_THRESHOLD ? weight : 0.0;
    }
    if (type == kPolymorphicLightTypeTriangle)
        return ComputeEmissiveTriangleWeight(packed.Base);
    if (packed.Base.HasLightShaping())
        packed.Extended = t_LightsEx[lightIndex];
    return ComputeAnalyticWeight(packed);
}

float ImportanceBooster(uint lightIndex, float weight)
{
    const LightingControlData ctrl = u_LightingControl[0];
    const LightsBakerConstants constants = ctrl.BakerConstants;
    if (constants.ImportanceBoostFrustumMul > 0.0)
    {
        float boostK = 0.5;
        const PolymorphicLightInfo light = t_Lights[lightIndex];
        const uint type = DecodeLightType(light);
        const bool distant = type == kPolymorphicLightTypeEnvironment || type == kPolymorphicLightTypeDirectional;
        if (!distant)
        {
            const float3 center = light.Center;
            float distanceMin = 0.0;
            [unroll] for (uint i = 0u; i < 5u; ++i)
            {
                const float4 plane = g_LightProxyBuildConstants.HasFrustumPlanes != 0u ? g_LightProxyBuildConstants.FrustumPlanes[i] : constants.FrustumPlanes[i];
                distanceMin = min(distanceMin, dot(center, plane.xyz) - plane.w);
            }
            boostK = saturate(1.0 + distanceMin / max(1e-5, constants.ImportanceBoostFrustumFadeDistance));
        }
        weight *= 1.0 + constants.ImportanceBoostFrustumMul * boostK;
    }
    if (ctrl.LastFrameTemporalFeedbackAvailable != 0u && constants.ImportanceBoostIntensityDelta > 0.0)
    {
        const uint previous = g_LightProxyBuildConstants.HasHistoryRemap != 0u ? t_HistoryRemapCurrentToPast[lightIndex] : lightIndex;
        const uint safePrevious = min(previous, max(1u, g_LightProxyBuildConstants.HistoricWeightCount) - 1u);
        const float historicWeight = previous < g_LightProxyBuildConstants.HistoricWeightCount ? t_HistoricLightWeights[safePrevious] : 0.0;
        weight += constants.ImportanceBoostIntensityDelta * max(0.0, weight - historicWeight * 1.1);
    }
    return weight;
}

[numthreads(1, 1, 1)]
void ResetProxyBuildCS(uint dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId != 0u)
        return;
    u_LightingControl[0].WeightsSumUINT      = 0u;
    u_LightingControl[0].SamplingProxyCount  = 0u;
    u_LightingControl[0].ProxyBuildTaskCount = 0u;
    u_ProxyDispatchArgs[0] = 0u;
    u_ProxyDispatchArgs[1] = 1u;
    u_ProxyDispatchArgs[2] = 1u;
}

// Single-group weight computation + reduction (avoids float atomics for cross-platform DXC/SPIR-V support):
// each of the 256 threads processes a contiguous block of lights, then a groupshared reduction writes the
// total weight sum directly. Dispatched with exactly one thread group.
#define LPB_REDUCE_THREADS 256u
groupshared float g_BlockWeightSums[LPB_REDUCE_THREADS];

[numthreads(LPB_REDUCE_THREADS, 1, 1)]
void ComputeWeightsCS(uint groupThreadId : SV_GroupThreadID)
{
    const uint total     = g_LightProxyBuildConstants.TotalLightCount;
    const uint perThread = (total + LPB_REDUCE_THREADS - 1u) / LPB_REDUCE_THREADS;
    const uint from      = groupThreadId * perThread;
    const uint to        = min(from + perThread, total);

    float sum = 0.0;
    [loop]
    for (uint lightIndex = from; lightIndex < to; ++lightIndex)
    {
        const float weight = DecodeLightType(t_Lights[lightIndex]) == kPolymorphicLightTypeCluster ? 0.0 :
            ImportanceBooster(lightIndex, ComputeLightWeight(lightIndex));
        u_LightWeights[lightIndex] = weight;
        sum += weight;
    }

    g_BlockWeightSums[groupThreadId] = sum;
    GroupMemoryBarrierWithGroupSync();

    if (groupThreadId == 0u)
    {
        float totalWeight = 0.0;
        [loop]
        for (uint i = 0u; i < LPB_REDUCE_THREADS; ++i)
            totalWeight += g_BlockWeightSums[i];
        u_LightingControl[0].WeightsSumUINT = asuint(totalWeight);
    }
}

[numthreads(256, 1, 1)]
void ProcessClustersCS(uint clusterOffset : SV_DispatchThreadID)
{
    const LightingControlData ctrl = u_LightingControl[0];
    if (clusterOffset >= ctrl.LightClusterCount) return;
    const uint clusterIndex = ctrl.TotalLightCount - ctrl.LightClusterCount + clusterOffset;
    PolymorphicLightInfo cluster = u_Lights[clusterIndex];
    const uint originalFrom = cluster.Direction2 & 0x7fffffu;
    const uint originalCount = ((cluster.Direction2 >> 23) & 63u) + 1u;
    float3 boundsMin = 3.402823466e+38.xxx, boundsMax = -3.402823466e+38.xxx;
    float weight = 0.0;
    for (uint i = originalFrom; i < originalFrom + originalCount; ++i)
    {
        PolymorphicLightInfo light = u_Lights[i];
        light.ColorTypeAndFlags &= ~kPolymorphicLightIsInCluster;
        u_Lights[i] = light;
        u_LightsEx[i].ClusterIndexAndCount = RTXPT_INVALID_LIGHT_INDEX;
        const EmissiveTriangle tri = UnpackTriangleLight(light);
        boundsMin = min(boundsMin, min(tri.base.xyz, min(tri.base.xyz + tri.edge1.xyz, tri.base.xyz + tri.edge2.xyz)));
        boundsMax = max(boundsMax, max(tri.base.xyz, max(tri.base.xyz + tri.edge1.xyz, tri.base.xyz + tri.edge2.xyz)));
        weight += u_LightWeights[i];
    }
    cluster.Center = (boundsMin + boundsMax) * 0.5;
    const float3 extents = (boundsMax - boundsMin) * 0.5;
    const float3 cameraPosition = g_LightProxyBuildConstants.HasCameraPosition != 0u ?
        g_LightProxyBuildConstants.CameraPosition : ctrl.BakerConstants.SceneCameraPos;
    const float distance = length(cluster.Center - cameraPosition);
    const float averageWeight = ctrl.WeightsSum() / max(1u, ctrl.TotalLightCount - ctrl.LightClusterCount);
    const float brightness = weight / float(originalCount) / max(averageWeight, 1e-30);
    bool active = g_LightProxyBuildConstants.ImportanceSamplingType != 0u &&
        3000.0 * pow(length(extents) / max(distance, 1e-7), 2.0) * sqrt(max(0.1, brightness)) < 1.0;
    const float threshold = 0.01 * weight / float(originalCount);
    uint from = originalFrom, to = from + originalCount;
    for (; to - from > 1u && u_LightWeights[from] <= threshold; ++from) weight -= u_LightWeights[from];
    for (; to - from > 1u && u_LightWeights[to - 1u] <= threshold; --to) weight -= u_LightWeights[to - 1u];
    uint significantCount = 0;
    for (uint j = from; j < to; ++j) significantCount += u_LightWeights[j] > threshold ? 1u : 0u;
    active = active && significantCount >= RTXPT_LIGHT_CLUSTER_MIN_TRIANGLES;
    cluster.Direction1 = from | ((to - from - 1u) << 23) | (active ? 0x80000000u : 0u);
    uint3 packedExtents = f32tof16(extents);
    packedExtents += uint3(f16tof32(packedExtents) < extents);
    cluster.Scalars = packedExtents.x | (packedExtents.y << 16);
    cluster.LogRadiance = packedExtents.z;
    u_Lights[clusterIndex] = cluster;
    u_LightWeights[clusterIndex] = max(0.0, weight);
    for (uint j = from; j < to; ++j)
    {
        // Retain membership even for inactive clusters: historical local tables
        // may still select them, and their leaf PDFs must remain evaluable.
        u_LightsEx[j].ClusterIndexAndCount = PolymorphicLightInfoEx::PackClusterIndexAndCount(clusterIndex, to - from);
        if (active) u_Lights[j].ColorTypeAndFlags |= kPolymorphicLightIsInCluster;
    }
}

[numthreads(256, 1, 1)]
void ComputeProxyCountsCS(uint dispatchThreadId : SV_DispatchThreadID)
{
    const uint lightIndex = dispatchThreadId;
    if (lightIndex >= g_LightProxyBuildConstants.TotalLightCount)
        return;

    const uint  totalLights = g_LightProxyBuildConstants.TotalLightCount;
    const uint  budget      = g_LightProxyBuildConstants.ProxyBudget;
    // Reserve one proxy per light so the per-light counts (each rounded up) still sum to <= budget.
    const uint sampledLightCount = totalLights - u_LightingControl[0].LightClusterCount;
    const uint  proxyPool   = (budget > sampledLightCount) ? (budget - sampledLightCount) : 0u;
    const float weightSum   = asfloat(u_LightingControl[0].WeightsSumUINT);
    float weight = u_LightWeights[lightIndex];
    const LightingControlData ctrl = u_LightingControl[0];
    if (ctrl.LastFrameTemporalFeedbackAvailable != 0u)
    {
        const uint validFeedbackCount = ctrl.TotalMaxFeedbackCount - t_FeedbackCounts[totalLights];
        uint feedbackCount = t_FeedbackCounts[lightIndex];
        const PolymorphicLightInfo light = t_Lights[lightIndex];
        if (DecodeLightType(light) == kPolymorphicLightTypeCluster)
        {
            const uint from = light.Direction1 & 0x7fffffu;
            const uint count = ((light.Direction1 >> 23) & 63u) + 1u;
            for (uint i = from; i < from + count; ++i) feedbackCount += t_FeedbackCounts[i];
        }
        const float feedbackWeight = float(feedbackCount) * weightSum / float(max(1u, validFeedbackCount));
        weight = lerp(weight, feedbackWeight, ctrl.GlobalFeedbackUseWeight);
    }

    uint proxyCount = 0u;
    if (weight > 0.0 && weightSum > 0.0)
    {
        if (g_LightProxyBuildConstants.ImportanceSamplingType == 0u)
            proxyCount = 1u; // uniform mode (reference/testing only)
        else
            proxyCount = (uint)ceil((float(proxyPool) * weight) / weightSum);
    }
    proxyCount = min(proxyCount, RTXPT_LIGHTING_MAX_SAMPLING_PROXIES_PER_LIGHT - 1u);
    const PolymorphicLightInfo light = t_Lights[lightIndex];
    if (light.IsPartOfLightCluster() ||
        (DecodeLightType(light) == kPolymorphicLightTypeCluster && (light.Direction1 & 0x80000000u) == 0u))
        proxyCount = 0u;

    u_LightProxyCounters[lightIndex] = proxyCount;
    if (proxyCount > 0u)
    {
        uint proxyBase, jobBase;
        InterlockedAdd(u_LightingControl[0].SamplingProxyCount, proxyCount, proxyBase);
        const uint jobCount = (proxyCount + 255u) / 256u;
        InterlockedAdd(u_LightingControl[0].ProxyBuildTaskCount, jobCount, jobBase);
        [loop] for (uint job = 0u; job < jobCount; ++job)
            u_ProxyJobs[jobBase + job] = uint4(lightIndex, proxyBase + job * 256u, min(256u, proxyCount - job * 256u), 0u);
    }
}

[numthreads(256, 1, 1)]
void ScatterProxiesCS(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint index = groupId.x + groupId.y * 65535u;
    if (index >= u_LightingControl[0].ProxyBuildTaskCount) return;
    const uint4 job = u_ProxyJobs[index];
    if (lane < job.z) u_LightSamplingProxies[job.y + lane] = job.x;
}

[numthreads(1, 1, 1)]
void FinalizeProxyJobsCS()
{
    const uint count = u_LightingControl[0].ProxyBuildTaskCount;
    u_ProxyDispatchArgs[0] = min(count, 65535u);
    u_ProxyDispatchArgs[1] = max(1u, (count + 65534u) / 65535u);
    u_ProxyDispatchArgs[2] = 1u;
}

#endif // __LIGHT_PROXY_BUILD_HLSL__
