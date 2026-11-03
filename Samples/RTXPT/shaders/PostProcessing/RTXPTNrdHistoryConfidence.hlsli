// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#ifndef RTXPT_NRD_HISTORY_CONFIDENCE_HLSLI
#define RTXPT_NRD_HISTORY_CONFIDENCE_HLSLI

VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> u_DenoiserHistoryConfidenceCoverage;
VK_IMAGE_FORMAT("r8") RWTexture2D<float> u_DenoiserHistoryConfidence;

static const uint kRTXPTConfidenceValid = 1u;
static const uint kRTXPTConfidenceReject = 2u;

// NRD samples confidence at previous UV with a linear clamp sampler. Mark every in-bounds
// history tap with positive weight, using the same FP16 motion NRD consumes. Rejection wins collisions;
// unmapped texels stay zero. This conservative mask can reject extra neighboring history.
void RTXPTScatterHistoryConfidence(float2 PreviousPixel, uint2 ImageSize, bool Valid)
{
    // NRD validates individual history taps. An off-screen center can still reuse
    // history if its bilinear footprint has a positive-weight in-bounds tap.
    const float kBilinearSupport = 1.0;
    if (!all(isfinite(PreviousPixel)) || any(PreviousPixel <= -kBilinearSupport) ||
        any(PreviousPixel >= float2(ImageSize)))
        return;

    const int2 Base = int2(floor(PreviousPixel));
    const float2 Fraction = frac(PreviousPixel);
    const uint State = Valid ? kRTXPTConfidenceValid : kRTXPTConfidenceReject;
    [unroll]
    for (int Y = 0; Y < 2; ++Y)
    {
        [unroll]
        for (int X = 0; X < 2; ++X)
        {
            const float2 Weight = float2(X == 0 ? 1.0 - Fraction.x : Fraction.x,
                                         Y == 0 ? 1.0 - Fraction.y : Fraction.y);
            const int2 Tap = Base + int2(X, Y);
            if (all(Weight > 0.0) && all(Tap >= 0) && all(Tap < int2(ImageSize)))
            {
                InterlockedOr(u_DenoiserHistoryConfidenceCoverage[Tap], State);
            }
        }
    }
}

void RTXPTResolveHistoryConfidence(uint2 Pixel)
{
    u_DenoiserHistoryConfidence[Pixel] =
        u_DenoiserHistoryConfidenceCoverage[Pixel] == kRTXPTConfidenceValid ? 1.0 : 0.0;
}

#endif
