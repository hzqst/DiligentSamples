// Upstream LightLink and environment-node history mapping, without GPU readback.
#include "Lighting/LightingTypes.hlsli"
#include "Lighting/EnvironmentLight.hlsli"

cbuffer g_HistoryConstants
{
    uint CurrentCount;
    uint PreviousCount;
    uint CurrentEnvCount;
    uint PreviousEnvCount;
    uint TableChanged;
    uint HistoryAvailable;
    uint2 Padding;
};
RWStructuredBuffer<LightingControlData> u_LightingControl;
RWStructuredBuffer<uint> u_CurrentToPast;
RWStructuredBuffer<uint> u_PastToCurrent;
StructuredBuffer<EnvironmentQuadLight> t_CurrentEnvironment;
StructuredBuffer<EnvironmentQuadLight> t_PreviousEnvironment;
Texture2D<uint> t_CurrentLookup;
Texture2D<uint> t_PreviousLookup;

[numthreads(256, 1, 1)]
void MapLightHistory(uint index : SV_DispatchThreadID)
{
    if (index == 0u) u_LightingControl[0].HistoricTotalLightCount = HistoryAvailable != 0u ? PreviousCount : 0u;
    if (HistoryAvailable == 0u || TableChanged == 0u)
    {
        const uint mapped = HistoryAvailable != 0u && index < min(CurrentCount, PreviousCount) ? index : RTXPT_INVALID_LIGHT_INDEX;
        if (index < CurrentCount) u_CurrentToPast[index] = mapped;
        if (index < PreviousCount) u_PastToCurrent[index] = mapped;
        return;
    }
    // Source ranges for analytic/triangle lights were uploaded by the CPU. Environment
    // subdivisions are not one-to-one: map the upper-left point in each direction.
    if (index < CurrentEnvCount)
    {
        uint mapped = RTXPT_INVALID_LIGHT_INDEX;
        if (PreviousEnvCount != 0u)
        {
            uint w, h; t_PreviousLookup.GetDimensions(w, h);
            const EnvironmentQuadLight node = t_CurrentEnvironment[index];
            mapped = t_PreviousLookup[uint2(node.NodeX, node.NodeY) * w / node.NodeDim];
        }
        u_CurrentToPast[index] = mapped;
    }
    if (index < PreviousEnvCount)
    {
        uint mapped = RTXPT_INVALID_LIGHT_INDEX;
        if (CurrentEnvCount != 0u)
        {
            uint w, h; t_CurrentLookup.GetDimensions(w, h);
            const EnvironmentQuadLight node = t_PreviousEnvironment[index];
            mapped = t_CurrentLookup[uint2(node.NodeX, node.NodeY) * w / node.NodeDim];
        }
        u_PastToCurrent[index] = mapped;
    }
}
