#ifndef __DENOISING_GUIDES_RR_HLSLI__
#define __DENOISING_GUIDES_RR_HLSLI__

#include "PathTracerCamera.hlsli"
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_RRDiffuseAlbedo;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_RRSpecAlbedo;
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> u_RRNormalsAndRoughness;
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> u_RRDepth;
VK_IMAGE_FORMAT("rg16f") RWTexture2D<float2> u_RRMotionVectors;

// Sanitize before arithmetic: fp16 transport may contain overflow or invalid values.
float RRFiniteNonNegative(float value)
{
    return isnan(value) ? 0.0 : clamp(value, 0.0, HLF_MAX);
}

float3 RRFiniteNonNegative(float3 value)
{
    return float3(RRFiniteNonNegative(value.x), RRFiniteNonNegative(value.y), RRFiniteNonNegative(value.z));
}

void DLSSRRDenoiserPrepareInputs(uint2 pixelPos)
{
    // BUILD uses mini-constant sample offset zero. Depth is the dominant path's
    // virtual ray distance, not hardware depth or linear view Z.
    const CameraRay cameraRay = ComputePathTracerCameraRay(g_Frame.ptConsts, pixelPos, g_Frame.ptConsts.sampleBaseIndex);
    const float rayDistance = t_Depth[pixelPos];
    const float3 virtualWorldPos = cameraRay.origin + cameraRay.dir * rayDistance;
    const float viewZ = mul(float4(virtualWorldPos, 1.0), g_Frame.view.MatWorldToView).z;
    const float kSkyViewZ = kEnvironmentMapSceneDistance;
    u_RRDepth[pixelPos] = isfinite(rayDistance) && rayDistance > 0.0 && isfinite(viewZ) && viewZ > 0.0 ? viewZ : kSkyViewZ;
    const float2 motion = t_MotionVectors[pixelPos].xy;
    u_RRMotionVectors[pixelPos] = all(isfinite(motion)) ? clamp(motion, -HLF_MAX, HLF_MAX) : 0.0.xx;

    DenoisingLayersContext denoisingLayers = DenoisingLayersContext::make(
        u_DenoisingLayersHeader, u_DenoisingLayersBuffer, u_StableRadiance, g_Frame.ptConsts);
    const uint activeCount = min(g_Frame.ptConsts.GetActiveDenoisingLayerCount(), cDenoisingLayerCount);
    const uint dominantIndex = denoisingLayers.LoadDominantIndex(pixelPos);

    float3 combinedRadiance = RRFiniteNonNegative(denoisingLayers.LoadStableRadiance(pixelPos));
    const float stableMax = max(combinedRadiance.x, max(combinedRadiance.y, combinedRadiance.z));
    const float3 stableAlbedo = sqrt(combinedRadiance / (1.0 + stableMax)); // ReinhardMax
    const float stableAlbedoAvg = Average(stableAlbedo);

    // RTXPT-fork's active default: throughput + equalization + dominant-plane bias.
    const float kThroughputWeight = 0.2;
    const float kEqualizationWeight = 0.01;
    const float kDominantWeight = 0.05;
    float3 available = 0.0.xxx;
    float3 throughputWeights = float3(1, 0, 0);
    for (uint planeIndex = 0; planeIndex < activeCount; ++planeIndex)
    {
        if (denoisingLayers.GetBranchID(pixelPos, planeIndex) == cDenoisingLayerInvalidBranchID)
            continue;
        available[planeIndex] = 1.0;
        if (planeIndex != 0)
        {
            DenoisingLayer sp = denoisingLayers.LoadDenoisingLayer(pixelPos, planeIndex);
            float3 throughput, motionVectors;
            UnpackTwoFp32ToFp16(sp.PackedThpAndMVs, throughput, motionVectors);
            const float weight = saturate(Average(RRFiniteNonNegative(throughput)));
            throughputWeights[planeIndex] = weight;
            throughputWeights[0] = saturate(throughputWeights[0] - weight);
        }
    }
    float3 spWeights = throughputWeights * kThroughputWeight + kEqualizationWeight;
    if (dominantIndex < activeCount)
        spWeights[dominantIndex] += kDominantWeight;
    spWeights *= available;
    const float weightSum = spWeights.x + spWeights.y + spWeights.z;
    spWeights /= max(weightSum, 1e-7);

    float3 guideNormals = float3(0, 0, 1e-6);
    float3 diffAlbedo = 0.0.xxx;
    float3 specAlbedo = 0.0.xxx;
    float roughness = 0.0;
    for (uint planeIndex = 0; planeIndex < activeCount; ++planeIndex)
    {
        if (available[planeIndex] == 0.0)
            continue;
        DenoisingLayer sp = denoisingLayers.LoadDenoisingLayer(pixelPos, planeIndex);
        if (!isfinite(sp.SceneLength))
            continue; // Sky/emission already belongs to StableRadiance.
        combinedRadiance += RRFiniteNonNegative(sp.GetNoisyRadiance());
        const float weight = spWeights[planeIndex];
        if (weight > 1e-6)
        {
            float3 diffBSDFEstimate, specBSDFEstimate;
            UnpackTwoFp32ToFp16(sp.DenoiserPackedBSDFEstimate, diffBSDFEstimate, specBSDFEstimate);
            guideNormals += weight * sp.GetNormal();
            roughness += weight * saturate(RRFiniteNonNegative(sp.GetRoughness()));
            diffAlbedo += weight * saturate(RRFiniteNonNegative(diffBSDFEstimate));
            specAlbedo += weight * saturate(RRFiniteNonNegative(specBSDFEstimate));
        }
    }

    const float3 stableAlbedoGreyMix = lerp(stableAlbedo, 0.5.xxx, 0.2);
    diffAlbedo = lerp(diffAlbedo, stableAlbedoGreyMix,
        stableAlbedoAvg / (Average(diffAlbedo) + sqrt(stableAlbedoAvg) + 1e-7));
    const float normalLength = length(guideNormals);
    guideNormals = isfinite(normalLength) && normalLength >= 1e-5 ?
        guideNormals / normalLength : float3(0, 0, 1);
    const float kMinAlbedo = 0.0001;
    if (Average(diffAlbedo + specAlbedo) < kMinAlbedo)
        diffAlbedo += kMinAlbedo;

    // Color is scene-linear, not pre-exposed. CPU supplies the clamp threshold;
    // PreExposure and ExposureScale at the RR consumer must both be 1.
    const float maxRadiance = max(combinedRadiance.x, max(combinedRadiance.y, combinedRadiance.z));
    const float brightnessClamp = g_Frame.ptConsts.DLSSRRBrightnessClampK;
    const float clampMax = isfinite(brightnessClamp) && brightnessClamp > 0.0 ?
        min(brightnessClamp, HLF_MAX) : HLF_MAX;
    if (maxRadiance > clampMax)
        combinedRadiance *= clampMax / maxRadiance;

    u_OutputColor[pixelPos] = float4(combinedRadiance, 1);
    u_RRDiffuseAlbedo[pixelPos] = float4(saturate(diffAlbedo), 1);
    u_RRSpecAlbedo[pixelPos] = float4(saturate(specAlbedo), 1);
    u_RRNormalsAndRoughness[pixelPos] = float4(guideNormals, saturate(roughness));
#if RTXPT_PREPARE_DLSS_RR
    // v1.9 planar-reflector guide. The orthonormal reflector-basis construction
    // preserves reflection-ray length, so its mirrored image lies this far
    // along the primary ray. Keep Diligent's current-to-previous pixel units.
    float2 specularMotion = all(isfinite(motion)) ? motion : 0.0.xx;
    if (activeCount > 0u && available.x != 0.0)
    {
        DenoisingLayer primary = denoisingLayers.LoadDenoisingLayer(pixelPos, 0);
        const float hitT = t_SpecularHitT[pixelPos];
        if (isfinite(primary.SceneLength) && isfinite(primary.GetRoughness()) && all(isfinite(primary.RayDir)) &&
            dot(primary.RayDir, primary.RayDir) > 0.5 && isfinite(hitT) && hitT > 1e-3 && roughness < 0.25)
        {
            const float3 reflectionRay = reflect(primary.RayDir, primary.GetNormal()) * hitT;
            const float z = abs(dot(primary.GetNormal(), reflectionRay));
            const float imageDistance = sqrt(max(0.0, dot(reflectionRay, reflectionRay) - z * z) + max(z, 1e-5) * max(z, 1e-5));
            const float3 imageWorld = primary.RayOrigin + primary.RayDir * (primary.SceneLength + imageDistance);
            specularMotion = ComputePathTracerMotionVector(imageWorld, imageWorld + primary.GetWorldMotionVector(), g_Frame.view, g_Frame.previousView).xy;
        }
    }
    u_RRSpecMotionVectors[pixelPos] = all(isfinite(specularMotion)) ? clamp(specularMotion, -HLF_MAX, HLF_MAX) : 0.0.xx;
#endif
}
#endif
