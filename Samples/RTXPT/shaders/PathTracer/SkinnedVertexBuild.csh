#include "PathTracerShared.h"

cbuffer cbSkinningConstants
{
    uint g_SourceVertexBase;
    uint g_DestVertexBase;
    uint g_JointBase;
    uint g_VertexCount;
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
    const uint               sourceIndex = g_SourceVertexBase + min(localVertex, max(1u, g_VertexCount) - 1u);
    const GeometryVertexData src         = UnpackGeometryVertex(t_SourceVertices[sourceIndex]);
    const SkinVertexData     skin        = t_SourceSkinData[sourceIndex];

    float4 skinnedPosition = float4(0.0, 0.0, 0.0, 0.0);
    float3 skinnedNormal   = float3(0.0, 0.0, 0.0);

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        const float weight = skin.weights[i];
        if (weight <= 0.0)
            continue;

        const uint     jointIndex  = g_JointBase + uint(skin.joints[i] + 0.5);
        const float4x4 jointMatrix = t_JointMatrices[jointIndex];
        skinnedPosition += mul(float4(src.position, 1.0), jointMatrix) * weight;
        skinnedNormal += mul(src.normal, (float3x3)jointMatrix) * weight;
    }

    GeometryVertexData dst;
    dst.position          = skinnedPosition.xyz;
    const float normalLen = length(skinnedNormal);
    dst.normal            = normalLen > 1e-6 ? skinnedNormal / normalLen : src.normal;
    dst.texCoord0         = src.texCoord0;

    u_SkinnedVertices[g_DestVertexBase + localVertex] = PackGeometryVertex(dst);
}
