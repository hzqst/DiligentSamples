#ifndef __MATERIAL_BRIDGE_HLSLI__
#define __MATERIAL_BRIDGE_HLSLI__

#include "../Config.h"
#include "../PathTracerShared.h"
#include "../Scene/Material/HomogeneousVolumeData.hlsli"

#ifndef RTXPT_ENABLE_MATERIAL_PSD_BRIDGE
#    define RTXPT_ENABLE_MATERIAL_PSD_BRIDGE 0
#endif

StructuredBuffer<MaterialPTData> t_PTMaterialData;

#ifdef ENABLE_MATERIAL_TEXTURES
// One Texture2DArray per loaded GLTF texture (the Diligent loader creates RESOURCE_DIM_TEX_2D_ARRAY textures).
// MATERIAL_TEXTURE_COUNT is supplied at compile time and equals RTXPTMaterials::GetTextureCount().
Texture2DArray t_BindlessTextures[MATERIAL_TEXTURE_COUNT];
SamplerState   s_MaterialSampler;
#endif

namespace Bridge
{
    MaterialPTData getMaterial(uint materialID)
    {
        return t_PTMaterialData[materialID];
    }

#ifdef ENABLE_MATERIAL_TEXTURES
    // Ray tracing shaders cannot derive LOD, so we sample the most detailed level. Slice is 0 for the
    // non-atlas bistro load; it is carried so an atlas path can be added later without touching call sites.
    float4 sampleMaterialTexture(uint textureIndex, float slice, float2 uv)
    {
        return t_BindlessTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(s_MaterialSampler, float3(uv, slice), 0.0);
    }

    float4 getBaseColor(MaterialPTData material, float2 uv)
    {
        float4 color = material.baseColorFactor;
        if ((material.flags & kMaterialFlagHasBaseColorTexture) != 0u)
            color *= sampleMaterialTexture(material.baseColorTextureIndex, material.baseColorTextureSlice, uv);
        return color;
    }

    float3 getEmission(MaterialPTData material, float2 uv)
    {
        float3 emission = material.emissiveFactor;
        if ((material.flags & kMaterialFlagHasEmissiveTexture) != 0u)
            emission *= sampleMaterialTexture(material.emissiveTextureIndex, material.emissiveTextureSlice, uv).rgb;
        return emission;
    }

    // True when the hit passes the alpha test (or is not alpha tested).
    bool alphaTestPasses(MaterialPTData material, float2 uv)
    {
        if ((material.flags & kMaterialFlagAlphaTested) == 0u)
            return true;
        // RTXPT-fork tests texture alpha at mip 0, without multiplying material opacity.
        // The CPU uploads the same UNORM8-quantized cutoff as SubInstanceData::AlphaCutoff().
        return sampleMaterialTexture(material.baseColorTextureIndex, material.baseColorTextureSlice, uv).a >= material.alphaCutoff;
    }

    // glTF metallic-roughness packing: roughness in .g, metallic in .b, each scaled by the material factor.
    float2 getMetallicRoughness(MaterialPTData material, float2 uv)
    {
        float metallic  = material.metallicFactor;
        float roughness = material.roughnessFactor;
        if ((material.flags & kMaterialFlagHasMetallicRoughnessTexture) != 0u)
        {
            const float4 mr = sampleMaterialTexture(material.metallicRoughnessTextureIndex, material.metallicRoughnessTextureSlice, uv);
            roughness *= mr.g;
            metallic  *= mr.b;
        }
        return float2(metallic, roughness);
    }

    // Tangent-space normal unpacked to [-1, 1] with NormalScale applied to xy. Returns (0,0,1) when there is no
    // normal map, which the caller treats as "no perturbation".
    float3 getTangentNormal(MaterialPTData material, float2 uv)
    {
        if ((material.flags & kMaterialFlagHasNormalTexture) == 0u)
            return float3(0.0, 0.0, 1.0);

        float3 n = sampleMaterialTexture(material.normalTextureIndex, material.normalTextureSlice, uv).xyz * 2.0 - 1.0;
        n.xy *= material.normalScale;
        return normalize(n);
    }

    float getTransmission(MaterialPTData material, float2 uv)
    {
        float transmission = material.transmissionFactor;
        if ((material.flags & kMaterialFlagHasTransmissionTexture) != 0u)
            transmission *= sampleMaterialTexture(material.transmissionTextureIndex, material.transmissionTextureSlice, uv).r;
        return saturate(transmission);
    }

    float getDiffuseTransmission(MaterialPTData material, float2 uv)
    {
        float transmission = material.diffuseTransmissionFactor;
        if ((material.flags & kMaterialFlagHasTransmissionTexture) != 0u)
            transmission *= sampleMaterialTexture(material.transmissionTextureIndex, material.transmissionTextureSlice, uv).r;
        return saturate(transmission);
    }

    float getThickness(MaterialPTData material, float2 uv)
    {
        float thickness = material.thicknessFactor;
        if ((material.flags & kMaterialFlagHasThicknessTexture) != 0u)
            thickness *= sampleMaterialTexture(material.thicknessTextureIndex, material.thicknessTextureSlice, uv).g;
        return max(thickness, 0.0);
    }
#else
    // Factor-only fallback (bindless material textures unavailable): no texture sampling, never alpha tested.
    float4 getBaseColor(MaterialPTData material, float2 uv) { return material.baseColorFactor; }
    float3 getEmission(MaterialPTData material, float2 uv) { return material.emissiveFactor; }
    bool   alphaTestPasses(MaterialPTData material, float2 uv) { return true; }
    float2 getMetallicRoughness(MaterialPTData material, float2 uv) { return float2(material.metallicFactor, material.roughnessFactor); }
    float3 getTangentNormal(MaterialPTData material, float2 uv) { return float3(0.0, 0.0, 1.0); }
    float  getTransmission(MaterialPTData material, float2 uv) { return saturate(material.transmissionFactor); }
    float  getDiffuseTransmission(MaterialPTData material, float2 uv) { return saturate(material.diffuseTransmissionFactor); }
    float  getThickness(MaterialPTData material, float2 uv) { return max(material.thicknessFactor, 0.0); }
#endif

    float loadIoR(uint materialID)
    {
        MaterialPTData material = getMaterial(materialID);
        return max(material.ior, 1.0);
    }

    float loadShadowNoLFadeout(uint materialID)
    {
        MaterialPTData material = getMaterial(materialID);
        return clamp(material.shadowNoLFadeout, 0.0, 0.25);
    }

    HomogeneousVolumeData loadHomogeneousVolumeData(uint materialID)
    {
        HomogeneousVolumeData volume;
        volume.sigmaS = float3(0.0, 0.0, 0.0);
        volume.sigmaA = float3(0.0, 0.0, 0.0);
        volume.g      = 0.0;

        MaterialPTData material = getMaterial(materialID);
        if ((material.flags & kMaterialFlagHasVolume) == 0u)
            return volume;

        const float3 attenuationColor = clamp(material.volumeAttenuationColor, 1e-7, 1.0);
        const float  attenuationDistance = max(material.volumeAttenuationDistance, 1e-30);
        volume.sigmaA = -log(attenuationColor) / attenuationDistance.xxx;
        return volume;
    }

    bool isThinSurface(MaterialPTData material)
    {
        return (material.flags & kMaterialFlagThinSurface) != 0u || (material.flags & kMaterialFlagHasTransmission) == 0u;
    }

    bool isPSDExclude(MaterialPTData material)
    {
#if RTXPT_ENABLE_MATERIAL_PSD_BRIDGE
        return (material.pathDecompositionFlags & kMaterialPathDecompositionFlagPSDExclude) != 0u;
#else
        return false;
#endif
    }

    uint getPSDBlockMotionVectorsAtSurfaceType(MaterialPTData material)
    {
#if RTXPT_ENABLE_MATERIAL_PSD_BRIDGE
        return (material.pathDecompositionFlags & kMaterialPathDecompositionFlagPSDBlockMotionVectorsAtSurfaceMask) >>
            kMaterialPathDecompositionFlagPSDBlockMotionVectorsAtSurfaceShift;
#else
        return 0u;
#endif
    }

    bool isPSDBlockMotionVectorsAtSurface(MaterialPTData material)
    {
        return getPSDBlockMotionVectorsAtSurfaceType(material) != 0u;
    }

    uint getPSDDominantDeltaLobeP1(MaterialPTData material)
    {
#if RTXPT_ENABLE_MATERIAL_PSD_BRIDGE
        return (material.pathDecompositionFlags & kMaterialPathDecompositionFlagPSDDominantDeltaLobeP1Mask) >>
            kMaterialPathDecompositionFlagPSDDominantDeltaLobeP1Shift;
#else
        return 0u;
#endif
    }

    bool ignoreMeshTangentSpace(MaterialPTData material)
    {
#if RTXPT_ENABLE_MATERIAL_PSD_BRIDGE
        return (material.pathDecompositionFlags & kMaterialPathDecompositionFlagIgnoreMeshTangentSpace) != 0u;
#else
        return false;
#endif
    }
} // namespace Bridge

// NOTE(RTXPT-Port): Intentional deviation that matches upstream behavior. Material texture sampling assumes a
// single UV set (TEXCOORD_0), a single immutable wrap-address sampler (s_MaterialSampler), and a carried-but-
// untransformed atlas slice. Upstream PathTracerBridgeDonut.hlsli does the same: one baked texcoord channel per
// geometry, one global AnisotropicWrap sampler bound as s_MaterialSampler for all material textures, and
// standalone bindless Texture2D loads with no atlas / UV scale-bias / KHR_texture_transform. TextureShaderAttribs
// UV selectors, wrap modes, and atlas UV transform are DiligentFX-loader concepts with no upstream analog, so
// there is nothing to re-port; honoring them would be a from-scratch addition. See RTXPT_FORK_MAPPING.md
// "Divergences". (Orthogonal, separately noted above: ray tracing cannot derive LOD here, so we sample mip 0,
// whereas upstream computes texture LOD via ray cones / STF.)

#endif // __MATERIAL_BRIDGE_HLSLI__
