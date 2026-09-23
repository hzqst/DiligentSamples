// Translation of RTXPT-fork LightsBaker::EnvLightsSubdivideBase/Boost/FillLookupMap.
#include "Config.h"
#include "Lighting/LightingTypes.hlsli"
#include "Lighting/EnvironmentLight.hlsli"

StructuredBuffer<LightingControlData> t_LightingControl;
Texture2D<float4> t_EnvironmentRadianceMap;
RWStructuredBuffer<uint> u_EnvironmentBaseNodes;
RWStructuredBuffer<PolymorphicLightInfo> u_EnvironmentLights;
VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> u_EnvironmentLightLookup;

#define ENV_LIGHTS_BAKE_THREADS 128
groupshared uint g_Nodes[RTXPT_NEEAT_ENVMAP_QT_UNBOOSTED_NODE_COUNT];
groupshared uint g_NodePackedWeights[RTXPT_NEEAT_ENVMAP_QT_UNBOOSTED_NODE_COUNT];
groupshared uint g_FindMaxPacked;

uint EnvironmentComputeWeightForQTBuild(uint dim, uint x, uint y, uint index, uint depthLimit)
{
    const uint mip = t_LightingControl[0].BakerConstants.EnvMapImportanceMapMIPCount - uint(firstbithigh(dim)) - 1u;
    const float area = float(1u << (mip * 2u));
    float weight = area * t_EnvironmentRadianceMap.Load(int3(x, y, mip)).a;
    weight = max(0.0001 * float(mip), weight);
    weight *= mip > depthLimit;
    return EQTNodeWeightPack(weight, index);
}

void Subdivide(uint threadIndex, uint nodeCount, uint subdivisions, uint depthLimit)
{
    if (threadIndex == 0u) g_FindMaxPacked = 0u;
    GroupMemoryBarrierWithGroupSync();
    for (uint split = 0u; split < subdivisions; ++split)
    {
        uint localMax = 0u;
        for (uint node = threadIndex; node < nodeCount; node += ENV_LIGHTS_BAKE_THREADS)
            localMax = max(localMax, g_NodePackedWeights[node]);
        // Same maximum packed weight/index and tie-breaking as the upstream wave reduction.
        InterlockedMax(g_FindMaxPacked, localMax);
        GroupMemoryBarrierWithGroupSync();
        const uint parent = g_FindMaxPacked & 0xfffu;
        uint dim, x, y;
        EQTNodeUnpack(g_Nodes[parent], dim, x, y);
        GroupMemoryBarrierWithGroupSync();
        if (threadIndex == 0u) g_FindMaxPacked = 0u;
        if (threadIndex < 4u)
        {
            dim *= 2u;
            x = x * 2u + threadIndex % 2u;
            y = y * 2u + threadIndex / 2u;
            const uint child = threadIndex == 0u ? parent : nodeCount + threadIndex - 1u;
            g_Nodes[child] = EQTNodePack(dim, x, y);
            g_NodePackedWeights[child] = EnvironmentComputeWeightForQTBuild(dim, x, y, child, depthLimit);
        }
        GroupMemoryBarrierWithGroupSync();
        nodeCount += 3u;
    }
}

[numthreads(ENV_LIGHTS_BAKE_THREADS, 1, 1)]
void EnvLightsSubdivideBase(uint threadIndex : SV_GroupIndex)
{
    const uint dim = RTXPT_NEEAT_ENVMAP_QT_BASE_RESOLUTION;
    if (threadIndex < dim * dim)
    {
        const uint x = threadIndex / dim;
        const uint y = threadIndex % dim;
        g_Nodes[threadIndex] = EQTNodePack(dim, x, y);
        g_NodePackedWeights[threadIndex] = EnvironmentComputeWeightForQTBuild(dim, x, y, threadIndex, RTXPT_NEEAT_ENVMAP_QT_BOOST_SUBDIVISION_DPT);
    }
    Subdivide(threadIndex, dim * dim, RTXPT_NEEAT_ENVMAP_QT_SUBDIVISIONS, RTXPT_NEEAT_ENVMAP_QT_BOOST_SUBDIVISION_DPT);
    if (threadIndex < RTXPT_NEEAT_ENVMAP_QT_UNBOOSTED_NODE_COUNT)
        u_EnvironmentBaseNodes[threadIndex] = g_Nodes[threadIndex];
}

[numthreads(ENV_LIGHTS_BAKE_THREADS, 1, 1)]
void EnvLightsSubdivideBoost(uint threadIndex : SV_GroupIndex, uint3 group : SV_GroupID)
{
    if (threadIndex == 0u)
    {
        uint dim, x, y;
        g_Nodes[0] = u_EnvironmentBaseNodes[group.x];
        EQTNodeUnpack(g_Nodes[0], dim, x, y);
        g_NodePackedWeights[0] = EnvironmentComputeWeightForQTBuild(dim, x, y, 0u, 0u);
    }
    Subdivide(threadIndex, 1u, RTXPT_NEEAT_ENVMAP_QT_BOOST_SUBDIVISION, 0u);
    if (threadIndex < RTXPT_NEEAT_ENVMAP_QT_BOOST_NODES_MULT)
    {
        EnvironmentQuadLight light = (EnvironmentQuadLight)0;
        EQTNodeUnpack(g_Nodes[threadIndex], light.NodeDim, light.NodeX, light.NodeY);
        const LightsBakerConstants constants = t_LightingControl[0].BakerConstants;
        const uint mip = constants.EnvMapImportanceMapMIPCount - uint(firstbithigh(light.NodeDim)) - 1u;
        const float4 value = t_EnvironmentRadianceMap.Load(int3(light.NodeX, light.NodeY, mip));
        light.Radiance = value.rgb * constants.EnvMapParams.ColorMultiplier * constants.EnvMapParams.Enabled;
        const float avgColor = dot(constants.EnvMapParams.ColorMultiplier, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
        light.Weight = float(1u << (mip * 2u)) * max(0.0, value.a * avgColor * constants.DistantVsLocalRelativeImportance) * constants.EnvMapParams.Enabled;
        u_EnvironmentLights[group.x * RTXPT_NEEAT_ENVMAP_QT_BOOST_NODES_MULT + threadIndex] = PackEnvironmentLight(light);
    }
}

[numthreads(8, 8, 1)]
void EnvLightsFillLookupMap(uint3 group : SV_GroupID, uint3 threadIndex : SV_GroupThreadID)
{
    const EnvironmentQuadLight light = UnpackEnvironmentLight(u_EnvironmentLights[group.x]);
    const uint scale = t_LightingControl[0].BakerConstants.EnvMapImportanceMapResolution / light.NodeDim;
    for (uint y = threadIndex.y; y < scale; y += 8u)
        for (uint x = threadIndex.x; x < scale; x += 8u)
            u_EnvironmentLightLookup[uint2(light.NodeX, light.NodeY) * scale + uint2(x, y)] = group.x;
}
