#ifndef __EMISSIVE_TRIANGLE_BUILD_HLSL__
#define __EMISSIVE_TRIANGLE_BUILD_HLSL__

#include "PathTracerShared.h"
#include "Lighting/LightPacking.hlsli"

struct EmissiveTriangleBuildConstants
{
    uint  SubInstanceCount;
    uint3 Padding;
};

ConstantBuffer<EmissiveTriangleBuildConstants> g_EmissiveTriangleBuildConstants;

StructuredBuffer<MaterialPTData>     t_PTMaterialData;
StructuredBuffer<SubInstanceData>    t_SubInstanceData;
StructuredBuffer<float4x4>           t_SubInstanceTransforms;
StructuredBuffer<PackedGeometryVertexData> t_VertexBuffer;
StructuredBuffer<PackedGeometryVertexData> t_SkinnedVertexBuffer;
Buffer<uint>                         t_IndexBuffer;
RWStructuredBuffer<PolymorphicLightInfo> u_EmissiveTriangles;

#ifdef ENABLE_MATERIAL_TEXTURES
// One Texture2DArray per loaded GLTF texture, mirroring BSDFs/MaterialBridge.hlsli.
// MATERIAL_TEXTURE_COUNT is supplied at compile time and equals RTXPTMaterials::GetTextureCount().
Texture2DArray t_BindlessTextures[MATERIAL_TEXTURE_COUNT];
SamplerState   s_MaterialSampler;
#endif

uint3 getTriangleIndices(SubInstanceData subInstance, uint localPrimitiveIndex)
{
    const uint baseIndex = localPrimitiveIndex * 3u;
    if ((subInstance.Flags & kSubInstanceFlagIndexed) != 0u)
    {
        return uint3(
            t_IndexBuffer[subInstance.IndexOffset + baseIndex + 0u],
            t_IndexBuffer[subInstance.IndexOffset + baseIndex + 1u],
            t_IndexBuffer[subInstance.IndexOffset + baseIndex + 2u]);
    }
    return uint3(baseIndex + 0u, baseIndex + 1u, baseIndex + 2u);
}

GeometryVertexData getGeometryVertex(SubInstanceData subInstance, uint vertexIndex)
{
    if ((subInstance.Flags & kSubInstanceFlagSkinned) != 0u)
        return UnpackGeometryVertex(t_SkinnedVertexBuffer[subInstance.VertexOffset + vertexIndex]);

    return UnpackGeometryVertex(t_VertexBuffer[subInstance.VertexOffset + vertexIndex]);
}

float3 transformPosition(float4x4 transform, float3 position)
{
    return mul(float4(position, 1.0), transform).xyz;
}

// Sign of the transform's linear (3x3) part; negative means the instance flips coordinate-system handedness.
float transformDeterminant(float4x4 transform)
{
    return determinant(float3x3(transform[0].xyz, transform[1].xyz, transform[2].xyz));
}

// Per-triangle emitted radiance. Without an emissive texture (or when material textures are unavailable) this
// is the constant emissive factor. With one, match RTXPT-fork PrepareLights.hlsl: sample the emissive texture
// once at the triangle centroid using an anisotropic footprint roughly inscribed in the triangle (the sample
// ellipse axes follow the short edge and the median from the opposite vertex), then modulate the factor.
float3 computeTriangleRadiance(MaterialPTData material, float2 uv0, float2 uv1, float2 uv2)
{
    float3 radiance = material.emissiveFactor;
#ifdef ENABLE_MATERIAL_TEXTURES
    if ((material.flags & kMaterialFlagHasEmissiveTexture) != 0u)
    {
        float2 edges[3];
        edges[0] = uv1 - uv0;
        edges[1] = uv2 - uv1;
        edges[2] = uv0 - uv2;

        float3 edgeLengths;
        edgeLengths[0] = length(edges[0]);
        edgeLengths[1] = length(edges[1]);
        edgeLengths[2] = length(edges[2]);

        float2 shortEdge;
        float2 longEdge1;
        float2 longEdge2;
        if (edgeLengths[0] < edgeLengths[1] && edgeLengths[0] < edgeLengths[2])
        {
            shortEdge = edges[0];
            longEdge1 = edges[1];
            longEdge2 = edges[2];
        }
        else if (edgeLengths[1] < edgeLengths[2])
        {
            shortEdge = edges[1];
            longEdge1 = edges[2];
            longEdge2 = edges[0];
        }
        else
        {
            shortEdge = edges[2];
            longEdge1 = edges[0];
            longEdge2 = edges[1];
        }

        const float2 shortGradient = shortEdge * (2.0 / 3.0);
        const float2 longGradient  = (longEdge1 + longEdge2) / 3.0;
        const float2 centerUV      = (uv0 + uv1 + uv2) / 3.0;

        const float3 emissiveMask = t_BindlessTextures[NonUniformResourceIndex(material.emissiveTextureIndex)]
            .SampleGrad(s_MaterialSampler, float3(centerUV, material.emissiveTextureSlice), shortGradient, longGradient).rgb;
        radiance *= emissiveMask;
    }
#endif
    return max(0.0, radiance);
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint subInstanceCount = g_EmissiveTriangleBuildConstants.SubInstanceCount;
    if (dispatchThreadId.x >= subInstanceCount)
        return;

    // Keep even a speculated descriptor load in bounds on Vulkan devices without robust buffer access: the NVIDIA
    // driver (Vulkan 1.4) also executes the dependent t_SubInstanceData -> t_PTMaterialData loads for guarded-out
    // lanes, and a MaterialID read past the sub-instance buffer faults the device.
    const uint           subInstanceIndex = min(dispatchThreadId.x, max(1u, subInstanceCount) - 1u);
    const SubInstanceData subInstance     = t_SubInstanceData[subInstanceIndex];
    const MaterialPTData  material        = t_PTMaterialData[subInstance.MaterialID];
    if ((material.flags & kMaterialFlagEmissiveAreaLight) == 0u)
        return;

    const uint triangleCount = ((subInstance.Flags & kSubInstanceFlagIndexed) != 0u ? subInstance.IndexCount : subInstance.VertexCount) / 3u;
    if (triangleCount == 0u)
        return;

    const float4x4 transform = t_SubInstanceTransforms[subInstanceIndex];
    const bool     flipped   = transformDeterminant(transform) < 0.0;

    [loop]
    for (uint localPrimitiveIndex = 0u; localPrimitiveIndex < triangleCount; ++localPrimitiveIndex)
    {
        const uint3 indices = getTriangleIndices(subInstance, localPrimitiveIndex);
        const GeometryVertexData v0 = getGeometryVertex(subInstance, indices.x);
        const GeometryVertexData v1 = getGeometryVertex(subInstance, indices.y);
        const GeometryVertexData v2 = getGeometryVertex(subInstance, indices.z);

        const float3 p0 = transformPosition(transform, v0.position);
        const float3 p1 = transformPosition(transform, v1.position);
        const float3 p2 = transformPosition(transform, v2.position);

        // Match RTXPT-fork PrepareLights.hlsl: keep the emitter normal (cross(edge1, edge2)) aligned with the
        // geometric front face by swapping edges when the transform flips handedness, so the one-sided NEE/MIS
        // test agrees with the closest-hit geometric normal for mirrored instances.
        float3 edge1;
        float3 edge2;
        if (!flipped)
        {
            edge1 = p1 - p0;
            edge2 = p2 - p0;
        }
        else
        {
            edge1 = p2 - p0;
            edge2 = p1 - p0;
        }

        const float3 radiance = computeTriangleRadiance(material, v0.texCoord0, v1.texCoord0, v2.texCoord0);

        EmissiveTriangle tri;
        tri.base     = float4(p0, 0.0);
        tri.edge1    = float4(edge1, 0.0);
        tri.edge2    = float4(edge2, 0.0);
        tri.radiance = float4(radiance, 0.0);
        u_EmissiveTriangles[subInstance.emissiveTriangleOffset + localPrimitiveIndex] = PackTriangleLight(tri);
    }
}

#endif // __EMISSIVE_TRIANGLE_BUILD_HLSL__
