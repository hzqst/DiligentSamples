#ifndef __LIGHT_SAMPLING_MIS_HLSLI__
#define __LIGHT_SAMPLING_MIS_HLSLI__

#include "LightingTypes.hlsli"

// RTXPT-fork uses balance MIS for both the proposal mixture and light/BSDF MIS.
float EvalLightSamplingMIS(float countA, float pdfA, float countB, float pdfB)
{
    const float a = countA * pdfA;
    const float b = countB * pdfB;
    return a + b > 0.0 ? a / (a + b) : 0.0;
}

void GetCandidateSampleCounts(LightingControlData ctrl, bool coherent, uint candidates,
                              out uint localCount, out uint globalCount)
{
    localCount = coherent && ctrl.ImportanceSamplingType == 2u && candidates > 0u ?
        min(ComputeCandidateSampleLocalCount(saturate(ctrl.LocalToGlobalSampleRatio), candidates), candidates - 1u) : 0u;
    globalCount = candidates - localCount;
}

bool IsScreenSpaceCoherent(LightingControlData ctrl, float rayConeWidth, float totalPathLength)
{
    return totalPathLength > 0.0 && rayConeWidth / totalPathLength < ctrl.ScreenSpaceVsWorldSpaceThreshold;
}

static const float kLightSamplerMISApproximationBias = 3.0;
float LightVsBSDFMISApprox(float bsdfPdf, uint candidates, uint fullSamples, bool forLight)
{
    const float lightPdf = float(candidates * fullSamples) * kLightSamplerMISApproximationBias;
    return forLight ? EvalLightSamplingMIS(1.0, lightPdf, 1.0, bsdfPdf) :
                      EvalLightSamplingMIS(1.0, bsdfPdf, 1.0, lightPdf);
}

#endif // __LIGHT_SAMPLING_MIS_HLSLI__
