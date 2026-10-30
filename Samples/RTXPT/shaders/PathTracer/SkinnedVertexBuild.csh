#include "PathTracerShared.h"

cbuffer cbSkinningConstants
{
    uint g_SourceVertexBase;
    uint g_DestVertexBase;
    uint g_JointBase;
    uint g_VertexCount;
    // Position influences start at g_SkinBase. Normal influences start at g_NormalSkinBase, which equals g_SkinBase
    // when normals follow the position influences.
    uint g_SkinBase;
    uint g_NormalSkinBase;
    uint g_Padding0;
    uint g_Padding1;
};

StructuredBuffer<PackedGeometryVertexData> t_SourceVertices;
StructuredBuffer<SkinVertexData>       t_SourceSkinData;
StructuredBuffer<float4x4>             t_JointMatrices;
RWStructuredBuffer<PackedGeometryVertexData> u_SkinnedVertices;

[numthreads(128, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint localVertex = dispatchThreadId.x;
    if (localVertex >= g_VertexCount)
        return;

    // Keep even a speculated descriptor load in bounds on Vulkan devices without robust buffer access:
    // skin joints read past this mesh would otherwise index t_JointMatrices with garbage.
    const uint               sourceVertex = min(localVertex, max(1u, g_VertexCount) - 1u);
    const GeometryVertexData src          = UnpackGeometryVertex(t_SourceVertices[g_SourceVertexBase + sourceVertex]);
    const SkinVertexData     skin         = t_SourceSkinData[g_SkinBase + sourceVertex];
    const SkinVertexData     normalSkin   = t_SourceSkinData[g_NormalSkinBase + sourceVertex];

    float4 skinnedPosition = float4(0.0, 0.0, 0.0, 0.0);
    float3 skinnedNormal   = float3(0.0, 0.0, 0.0);

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        if (skin.weights[i] > 0.0)
            skinnedPosition += mul(float4(src.position, 1.0), t_JointMatrices[g_JointBase + uint(skin.joints[i] + 0.5)]) * skin.weights[i];
        if (normalSkin.weights[i] > 0.0)
            skinnedNormal += mul(src.normal, (float3x3)t_JointMatrices[g_JointBase + uint(normalSkin.joints[i] + 0.5)]) * normalSkin.weights[i];
    }

    GeometryVertexData dst;
    dst.position          = skinnedPosition.xyz;
    const float normalLen = length(skinnedNormal);
    dst.normal            = normalLen > 1e-6 ? skinnedNormal / normalLen : src.normal;
    dst.texCoord0         = src.texCoord0;

    u_SkinnedVertices[g_DestVertexBase + localVertex] = PackGeometryVertex(dst);
}
