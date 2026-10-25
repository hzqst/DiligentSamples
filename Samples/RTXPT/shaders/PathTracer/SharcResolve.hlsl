// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#include "SharcBindings.hlsli"
[numthreads(RTXPT_SHARC_GROUP_SIZE, 1, 1)]
void Resolve(uint3 tid : SV_DispatchThreadID)
{
    uint index = tid.x + tid.y * RTXPT_SHARC_GROUP_SIZE * RTXPT_SHARC_GROUPS_PER_ROW;
    if (index >= g_Sharc.EntriesNum) return;
    SharcResolveParameters p = (SharcResolveParameters)0;
    p.accumulationFrameNum = g_Sharc.AccumulationFrameNum;
    p.staleFrameNumMax = g_Sharc.StaleFrameNum;
    p.cameraPositionPrev = g_Sharc.CameraPositionPrev.xyz;
    SharcResolveEntry(index, GetSharcParameters(), p);
}
[numthreads(RTXPT_SHARC_GROUP_SIZE, 1, 1)]
void Clear(uint3 tid : SV_DispatchThreadID)
{
    uint index = tid.x + tid.y * RTXPT_SHARC_GROUP_SIZE * RTXPT_SHARC_GROUPS_PER_ROW;
    if (index >= g_Sharc.EntriesNum) return;
    u_SharcHash[index] = 0;
    u_SharcAccumulation[index] = (SharcAccumulationData)0;
    u_SharcResolved[index] = (SharcPackedData)0;
}
