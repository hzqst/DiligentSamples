// RTXPT-fork LightsBaker feedback stages translated to separate Diligent resources.
#include "Lighting/LightingTypes.hlsli"

cbuffer g_FeedbackConstants
{
    uint2 FeedbackSize;
    uint FrameIndex;
    uint ResetHistory;
    uint2 TileJitter;
    uint2 PreviousTileJitter;
    uint HasHistoryRemap;
    uint3 Padding;
};
RWStructuredBuffer<LightingControlData> u_LightingControl;
Texture2D<float> t_FeedbackTotalWeight;
Texture2D<uint> t_FeedbackCandidates;
RWTexture2D<float> u_FeedbackTotalWeight;
RWTexture2D<uint> u_FeedbackCandidates;
RWTexture2D<uint2> u_Filtered;
RWTexture2D<uint2> u_Blended;
RWTexture2D<uint2> u_Processed;
Texture2D<float> t_Depth;
Texture2D<float4> t_Motion;
RWTexture2D<float> u_HistoryDepth;
RWStructuredBuffer<uint> u_FeedbackCounts;
StructuredBuffer<uint> t_LightSamplingProxies;
StructuredBuffer<uint> t_HistoryRemapPastToCurrent;
RWStructuredBuffer<uint> u_LocalSamplingBuffer;

static const float kHistoryDropoff = 0.005;
static const float kDepthDisocclusionThreshold = 1.5;

struct MicroRng
{
    uint N;
    static MicroRng make(uint2 coord, uint frame, uint stream)
    {
        MicroRng rng;
        rng.N = ((coord.x << 16) | coord.y) ^ 0x9e3779b9u;
        rng.N ^= frame + (rng.N << 6) + (rng.N >> 2);
        rng.N ^= stream + (rng.N << 6) + (rng.N >> 2);
        return rng;
    }
    uint Next()
    {
        N ^= N >> 16; N *= 0x21f0aaadu;
        N ^= N >> 15; N *= 0xf35a2d97u;
        N ^= N >> 15;
        return N;
    }
    float NextFloat() { return float(Next() >> 8) / 16777216.0; }
    float2 NextFloat2() { return float2(NextFloat(), NextFloat()); }
};

uint2 EmptyFeedback() { return uint2(0u, RTXPT_INVALID_LIGHT_INDEX); }
uint RemapPastToCurrent(uint index)
{
    const LightingControlData ctrl = u_LightingControl[0];
    if (index == RTXPT_INVALID_LIGHT_INDEX) return index;
    if (HasHistoryRemap != 0u)
    {
        // Keep even a speculated descriptor load in bounds on Vulkan devices
        // without robust buffer access. The map always allocates at least one word.
        const uint safeIndex = min(index, max(1u, ctrl.HistoricTotalLightCount) - 1u);
        index = index < ctrl.HistoricTotalLightCount ? t_HistoryRemapPastToCurrent[safeIndex] : RTXPT_INVALID_LIGHT_INDEX;
    }
    return index < ctrl.TotalLightCount ? index : RTXPT_INVALID_LIGHT_INDEX;
}
float FeedbackWeight(uint2 value) { return value.y == RTXPT_INVALID_LIGHT_INDEX ? 0.0 : asfloat(value.x); }
bool IsSSC(uint2 value) { return value.y != RTXPT_INVALID_LIGHT_INDEX && (value.y & LFR_SCREEN_SPACE_COHERENT_FLAG) != 0u; }
uint2 LoadRaw(int2 pixel)
{
    pixel = clamp(pixel, 0, int2(FeedbackSize) - 1);
    return uint2(asuint(t_FeedbackTotalWeight[pixel]), t_FeedbackCandidates[pixel]);
}
void MergeFeedback(inout uint2 target, uint2 source, float multiplier, inout MicroRng rng)
{
    const float weight = min(FeedbackWeight(source) * multiplier, LFR_MAX_WEIGHT);
    const float sum = min(FeedbackWeight(target) + weight, LFR_MAX_WEIGHT);
    if (weight > 0.0 && rng.NextFloat() < weight / sum) target.y = source.y;
    target.x = asuint(sum);
}
uint SampleGlobal(inout MicroRng rng)
{
    const uint count = u_LightingControl[0].SamplingProxyCount;
    if (count == 0u) return RTXPT_INVALID_LIGHT_INDEX;
    return t_LightSamplingProxies[min(uint(rng.NextFloat() * count), count - 1u)];
}
bool Reproject(int2 pixel, out int2 previous)
{
    previous = int2(float2(pixel) + t_Motion[pixel].xy + 0.5);
    bool valid = ResetHistory == 0u && all(previous >= 0) && all(previous < int2(FeedbackSize));
    if (valid)
    {
        const float historic = u_HistoryDepth[previous];
        const float current = t_Depth[pixel];
        valid = historic > 0.0 && current > 0.0 && max(historic / current, current / historic) <= kDepthDisocclusionThreshold;
    }
    if (!valid) previous = pixel;
    return valid;
}

[numthreads(256, 1, 1)]
void ResetFeedbackCounts(uint index : SV_DispatchThreadID)
{
    if (index <= u_LightingControl[0].TotalLightCount) u_FeedbackCounts[index] = 0u;
    if (index == 0u)
    {
        u_LightingControl[0].LastFrameTemporalFeedbackAvailable = ResetHistory == 0u;
        u_LightingControl[0].LastFrameLocalSamplesAvailable = ResetHistory == 0u;
        u_LightingControl[0].BakerConstants.UpdateCounter = FrameIndex;
        u_LightingControl[0].BakerConstants.FeedbackResolution = FeedbackSize;
        u_LightingControl[0].BakerConstants.BlendedFeedbackResolution = (FeedbackSize + 1u) / 2u;
        u_LightingControl[0].TotalMaxFeedbackCount = FeedbackSize.x * FeedbackSize.y;
        u_LightingControl[0].LocalSamplingTileJitter = TileJitter;
        u_LightingControl[0].LocalSamplingTileJitterPrev = PreviousTileJitter;
        u_LightingControl[0].LocalSamplingResolution = (FeedbackSize + 14u) / 8u;
        u_LightingControl[0].TileBufferHeight = (FeedbackSize.y + 14u) / 8u;
    }
}

[numthreads(8, 8, 1)]
void PreFilterFeedback(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= FeedbackSize)) return;
    uint2 selected = EmptyFeedback();
    if (ResetHistory == 0u)
    {
        const uint2 center = LoadRaw(pixel);
        const bool centerPresent = center.y != RTXPT_INVALID_LIGHT_INDEX;
        uint2 kernel[9];
        float cdf[9];
        float sum = 0.0;
        uint i = 0u;
        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                kernel[i] = LoadRaw(int2(pixel) + int2(x, y));
                float multiplier = x == 0 && y == 0 ? 48.0 : 1.0;
                multiplier *= centerPresent && IsSSC(center) == IsSSC(kernel[i]) ? 128.0 : 1.0;
                sum += FeedbackWeight(kernel[i]) * multiplier;
                cdf[i++] = sum;
            }
        MicroRng rng = MicroRng::make(pixel, FrameIndex, 7u);
        const float random = rng.NextFloat();
        uint pick = 8u;
        [unroll] for (uint j = 0u; j < 8u; ++j)
            if (random < cdf[j] / sum) { pick = j; break; }
        selected = kernel[pick];
    }
    const uint lightIndex = RemapPastToCurrent(selected.y & ~LFR_SCREEN_SPACE_COHERENT_FLAG);
    selected.y = lightIndex == RTXPT_INVALID_LIGHT_INDEX ? lightIndex : lightIndex | (selected.y & LFR_SCREEN_SPACE_COHERENT_FLAG);
    // All feedback affects global weights; only SSC feedback belongs in local tables.
    InterlockedAdd(u_FeedbackCounts[min(lightIndex, u_LightingControl[0].TotalLightCount)], 1u);
    u_Filtered[pixel] = IsSSC(selected) && lightIndex < u_LightingControl[0].TotalLightCount ? selected : EmptyFeedback();
}

[numthreads(8, 8, 1)]
void BlendFeedback(uint2 pixelLR : SV_DispatchThreadID)
{
    if (any(pixelLR >= (FeedbackSize + 1u) / 2u)) return;
    MicroRng rng = MicroRng::make(pixelLR, FrameIndex, 3u);
    uint2 result = EmptyFeedback();
    if (ResetHistory == 0u)
    {
        [unroll] for (int x = -1; x < 3; ++x)
            [unroll] for (int y = -1; y < 3; ++y)
            {
                const int2 pixel = clamp(int2(pixelLR * 2u) + int2(x, y), 0, int2(FeedbackSize) - 1);
                int2 previous;
                if (Reproject(pixel, previous))
                    MergeFeedback(result, u_Filtered[previous], x < 0 || y < 0 || x >= 2 || y >= 2 ? kHistoryDropoff : 1.0, rng);
            }
    }
    if (result.y == RTXPT_INVALID_LIGHT_INDEX) result.y = SampleGlobal(rng);
    u_Blended[pixelLR] = result;
}

[numthreads(8, 8, 1)]
void ReprojectFeedback(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= FeedbackSize)) return;
    MicroRng rng = MicroRng::make(pixel, FrameIndex, 4u);
    int2 previous;
    const bool valid = Reproject(pixel, previous);
    uint2 result = valid ? u_Filtered[previous] : EmptyFeedback();
    if (ResetHistory == 0u)
        MergeFeedback(result, u_Blended[pixel / 2u], kHistoryDropoff, rng);
    if (result.y == RTXPT_INVALID_LIGHT_INDEX && valid)
    {
        const LightingControlData ctrl = u_LightingControl[0];
        const uint2 tile = min((uint2(previous) + ctrl.LocalSamplingTileJitterPrev) / RTXPT_LIGHTING_SAMPLING_BUFFER_TILE_SIZE,
                              ctrl.LocalSamplingResolution - 1u);
        const uint index = rng.Next() % RTXPT_LIGHTING_LOCAL_PROXY_COUNT;
        result.y = RemapPastToCurrent(UnpackMiniListLight(u_LocalSamplingBuffer[LLSB_ComputeBaseAddress(tile, ctrl.LocalSamplingResolution) + index]));
    }
    if (result.y == RTXPT_INVALID_LIGHT_INDEX) result.y = SampleGlobal(rng);
    u_Processed[pixel] = result;
}

int2 MirrorCoord(int2 coord)
{
    coord = abs(coord);
    coord = int2(coord.x < int(FeedbackSize.x) ? coord.x : 2 * int(FeedbackSize.x) - 2 - coord.x,
                 coord.y < int(FeedbackSize.y) ? coord.y : 2 * int(FeedbackSize.y) - 2 - coord.y);
    return clamp(coord, 0, int2(FeedbackSize) - 1);
}

groupshared uint g_LocalLights[RTXPT_LIGHTING_LOCAL_PROXY_COUNT];
[numthreads(RTXPT_LIGHTING_LOCAL_PROXY_COUNT, 1, 1)]
void FillFeedbackTile(uint2 tile : SV_GroupID, uint index : SV_GroupIndex)
{
    const LightingControlData ctrl = u_LightingControl[0];
    if (any(tile >= ctrl.LocalSamplingResolution)) return;
    const uint base = LLSB_ComputeBaseAddress(tile, ctrl.LocalSamplingResolution);
    const int2 topLeft = int2(tile * RTXPT_LIGHTING_SAMPLING_BUFFER_TILE_SIZE) - int2(ctrl.LocalSamplingTileJitter);
    uint lightIndex = 0u;
    if (ctrl.SamplingProxyCount > 0u)
    {
        const uint windowSamples = RTXPT_LIGHTING_SAMPLING_BUFFER_WINDOW_SIZE * RTXPT_LIGHTING_SAMPLING_BUFFER_WINDOW_SIZE;
        if (index < windowSamples)
        {
            const int margin = (RTXPT_LIGHTING_SAMPLING_BUFFER_WINDOW_SIZE - RTXPT_LIGHTING_SAMPLING_BUFFER_TILE_SIZE) / 2;
            const int2 source = MirrorCoord(topLeft - margin + int2(index / RTXPT_LIGHTING_SAMPLING_BUFFER_WINDOW_SIZE, index % RTXPT_LIGHTING_SAMPLING_BUFFER_WINDOW_SIZE));
            lightIndex = u_Processed[source].y;
        }
        else
        {
            // The original FillTile uses one RNG sequence for all top-up samples.
            MicroRng rng = MicroRng::make(tile, FrameIndex, 5u);
            float2 offset = 0.0;
            [loop] for (uint i = windowSamples; i <= index; ++i)
                offset = (rng.NextFloat2() - 0.5) * (RTXPT_LIGHTING_SAMPLING_BUFFER_WINDOW_SIZE * 4.0);
            const int2 source = MirrorCoord(int2(float2(topLeft) + RTXPT_LIGHTING_SAMPLING_BUFFER_TILE_SIZE * 0.5 + offset + 0.5));
            lightIndex = u_Blended[source / 2].y;
        }
    }
    g_LocalLights[index] = lightIndex & ~LFR_SCREEN_SPACE_COHERENT_FLAG;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint size = 2u; size <= RTXPT_LIGHTING_LOCAL_PROXY_COUNT; size <<= 1u)
        [unroll] for (uint stride = size >> 1u; stride > 0u; stride >>= 1u)
        {
            const uint other = index ^ stride;
            const uint a = g_LocalLights[index], b = g_LocalLights[other];
            const bool keepMinimum = ((index & size) == 0u) == ((index & stride) == 0u);
            // All lanes finish reading the previous sorting stage before any lane
            // overwrites it. Each lane then owns exactly one shared-memory store.
            GroupMemoryBarrierWithGroupSync();
            g_LocalLights[index] = keepMinimum ? min(a, b) : max(a, b);
            GroupMemoryBarrierWithGroupSync();
        }
    lightIndex = g_LocalLights[index];
    uint first = 0u, last = RTXPT_LIGHTING_LOCAL_PROXY_COUNT;
    while (first < last)
    {
        const uint mid = (first + last) / 2u;
        if (g_LocalLights[mid] < lightIndex) first = mid + 1u;
        else last = mid;
    }
    const uint lower = first;
    last = RTXPT_LIGHTING_LOCAL_PROXY_COUNT;
    while (first < last)
    {
        const uint mid = (first + last) / 2u;
        if (g_LocalLights[mid] <= lightIndex) first = mid + 1u;
        else last = mid;
    }
    u_LocalSamplingBuffer[base + index] = PackMiniListLightAndCount(lightIndex, first - lower);
}

[numthreads(8, 8, 1)]
void RetainFeedbackHistory(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= FeedbackSize)) return;
    u_HistoryDepth[pixel] = t_Depth[pixel];
    uint2 result = EmptyFeedback();
    if (ResetHistory == 0u)
    {
        result = u_Processed[pixel];
        result.x = asuint(FeedbackWeight(result) * kHistoryDropoff);
        MicroRng rng = MicroRng::make(pixel, FrameIndex, 6u);
        const int2 offsets[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
        [unroll] for (uint i = 0; i < 4u; ++i)
            MergeFeedback(result, u_Processed[clamp(int2(pixel) + offsets[i], 0, int2(FeedbackSize) - 1)], kHistoryDropoff * kHistoryDropoff, rng);
        if (FeedbackWeight(result) < 1e-12) result = EmptyFeedback();
    }
    u_FeedbackTotalWeight[pixel] = asfloat(result.x);
    u_FeedbackCandidates[pixel] = result.y;
}
