#include "Lighting/LightingTypes.hlsli"

StructuredBuffer<LightingControlData> t_LightingControl;
StructuredBuffer<uint>                t_LightProxyCounters;
StructuredBuffer<uint>                t_LightSamplingProxies;
Texture2D<float>                      t_FeedbackTotalWeight;
Texture2D<uint>                       t_FeedbackCandidates;
RWTexture2D<float>                    u_FeedbackTotalWeight;
RWTexture2D<uint>                     u_FeedbackCandidates;
RWStructuredBuffer<uint>              u_LocalSamplingBuffer;

[numthreads(8, 8, 1)]
void ClearFeedbackCS(uint3 tid : SV_DispatchThreadID)
{
    const LightingControlData ctrl = t_LightingControl[0];
    if (tid.x >= ctrl.BakerConstants.FeedbackResolution.x || tid.y >= ctrl.BakerConstants.FeedbackResolution.y)
        return;

    u_FeedbackTotalWeight[tid.xy] = 0.0;
    u_FeedbackCandidates[tid.xy]  = RTXPT_INVALID_LIGHT_INDEX;
}

groupshared uint g_LocalLights[RTXPT_LIGHTING_LOCAL_PROXY_COUNT];
groupshared float g_FeedbackWeights[RTXPT_LIGHTING_LOCAL_PROXY_COUNT];
groupshared uint g_FeedbackLights[RTXPT_LIGHTING_LOCAL_PROXY_COUNT];
groupshared uint g_FeedbackPixels[RTXPT_LIGHTING_LOCAL_PROXY_COUNT];

// One workgroup owns one tile. Keep the same proxy multiset, then sort it and
// store multiplicities once instead of recounting 128 entries at every path vertex.
[numthreads(RTXPT_LIGHTING_LOCAL_PROXY_COUNT, 1, 1)]
void FillLocalSamplingCS(uint3 tile : SV_GroupID, uint index : SV_GroupIndex)
{
    const LightingControlData ctrl = t_LightingControl[0];
    if (tile.x >= ctrl.LocalSamplingResolution.x || tile.y >= ctrl.LocalSamplingResolution.y)
        return;

    const uint2 feedbackResolution = ctrl.BakerConstants.FeedbackResolution;
    const uint2 pixelBase = tile.xy * RTXPT_LIGHTING_SAMPLING_BUFFER_TILE_SIZE;
    const uint base = LLSB_ComputeBaseAddress(tile.xy, ctrl.LocalSamplingResolution);
    if (ctrl.SamplingProxyCount == 0u)
    {
        // Local sampling is disabled by the consumer when there are no proxies.
        u_LocalSamplingBuffer[base + index] = PackMiniListLightAndCount(0u, RTXPT_LIGHTING_LOCAL_PROXY_COUNT);
        return;
    }

    g_FeedbackWeights[index] = 0.0;
    g_FeedbackLights[index] = RTXPT_INVALID_LIGHT_INDEX;
    g_FeedbackPixels[index] = index;
    const uint2 pixel = pixelBase + uint2(index % RTXPT_LIGHTING_SAMPLING_BUFFER_TILE_SIZE,
                                         index / RTXPT_LIGHTING_SAMPLING_BUFFER_TILE_SIZE);
    if (index < RTXPT_LIGHTING_SAMPLING_BUFFER_TILE_SIZE * RTXPT_LIGHTING_SAMPLING_BUFFER_TILE_SIZE &&
        all(pixel < feedbackResolution))
    {
        const uint candidateRaw = t_FeedbackCandidates.Load(int3(pixel, 0));
        const uint candidate = candidateRaw & ~LFR_SCREEN_SPACE_COHERENT_FLAG;
        const float weight = t_FeedbackTotalWeight.Load(int3(pixel, 0));
        if ((candidateRaw & LFR_SCREEN_SPACE_COHERENT_FLAG) != 0u && candidate < ctrl.TotalLightCount && weight > 0.0)
        {
            g_FeedbackLights[index] = candidate;
            g_FeedbackWeights[index] = weight;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = RTXPT_LIGHTING_LOCAL_PROXY_COUNT / 2u; stride > 0u; stride >>= 1u)
    {
        if (index < stride)
        {
            const uint other = index + stride;
            // Preserve the original row-major, first-maximum tie-breaking.
            if (g_FeedbackWeights[other] > g_FeedbackWeights[index] ||
                (g_FeedbackWeights[other] == g_FeedbackWeights[index] && g_FeedbackPixels[other] < g_FeedbackPixels[index]))
            {
                g_FeedbackWeights[index] = g_FeedbackWeights[other];
                g_FeedbackLights[index] = g_FeedbackLights[other];
                g_FeedbackPixels[index] = g_FeedbackPixels[other];
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    uint lightIndex = t_LightSamplingProxies[index % ctrl.SamplingProxyCount];
    if (g_FeedbackLights[0] != RTXPT_INVALID_LIGHT_INDEX && index < RTXPT_LIGHTING_SAMPLING_BUFFER_WINDOW_SIZE)
        lightIndex = g_FeedbackLights[0];
    g_LocalLights[index] = lightIndex;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint size = 2u; size <= RTXPT_LIGHTING_LOCAL_PROXY_COUNT; size <<= 1u)
    {
        [unroll]
        for (uint stride = size >> 1u; stride > 0u; stride >>= 1u)
        {
            const uint other = index ^ stride;
            if (other > index)
            {
                const uint a = g_LocalLights[index];
                const uint b = g_LocalLights[other];
                const bool ascending = (index & size) == 0u;
                g_LocalLights[index] = ascending ? min(a, b) : max(a, b);
                g_LocalLights[other] = ascending ? max(a, b) : min(a, b);
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }

    lightIndex = g_LocalLights[index];
    uint first = 0u, last = RTXPT_LIGHTING_LOCAL_PROXY_COUNT;
    while (first < last)
    {
        const uint middle = (first + last) / 2u;
        if (g_LocalLights[middle] < lightIndex)
            first = middle + 1u;
        else
            last = middle;
    }
    const uint lower = first;
    last = RTXPT_LIGHTING_LOCAL_PROXY_COUNT;
    while (first < last)
    {
        const uint middle = (first + last) / 2u;
        if (g_LocalLights[middle] <= lightIndex)
            first = middle + 1u;
        else
            last = middle;
    }
    u_LocalSamplingBuffer[base + index] = PackMiniListLightAndCount(lightIndex, first - lower);
}
