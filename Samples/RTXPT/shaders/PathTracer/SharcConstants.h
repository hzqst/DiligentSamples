// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#ifndef RTXPT_SHARC_CONSTANTS_H
#define RTXPT_SHARC_CONSTANTS_H
#define RTXPT_SHARC_PATH_STATE_STRIDE 64
#define RTXPT_SHARC_GROUP_SIZE        256
#define RTXPT_SHARC_GROUPS_PER_ROW    256
struct SharcConstants
{
    uint   EntriesNum;
    uint   DownscaleFactor;
    uint   AccumulationFrameNum;
    uint   StaleFrameNum;
    float  SceneScale;
    float  TerminateThreshold;
    float  MinRoughnessOverride;
    uint   DebugMode;
    float4 CameraPosition;
    float4 CameraPositionPrev;
};
#endif
