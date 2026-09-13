/* Copyright 2026 Diligent Graphics LLC
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <string>
#include "RefCntAutoPtr.hpp"
#include "RTXPTRayReconstructionInputs.hpp"

namespace Diligent
{

struct RTXPTRayReconstructionSettings
{
    RAY_RECONSTRUCTION_QUALITY     Quality = RAY_RECONSTRUCTION_QUALITY_QUALITY;
    RAY_RECONSTRUCTION_DLSS_PRESET Preset  = RAY_RECONSTRUCTION_DLSS_PRESET_F;
};

enum class RTXPTRayReconstructionStatus
{
    Unavailable,
    Available,
    Ready,
    SourceQueryFailed,
    CreationFailed,
    ExecuteFailed
};

struct RTXPTRayReconstructionStats
{
    RTXPTRayReconstructionStatus Status       = RTXPTRayReconstructionStatus::Unavailable;
    bool                         Available    = false;
    bool                         LastExecute  = false;
    Uint32                       ExecuteCount = 0;
    Uint32                       ResetCount   = 0;
    Uint32                       CreateCount  = 0;
    std::string                  Message;
};

class RTXPTRayReconstructionPass
{
public:
    ~RTXPTRayReconstructionPass() { Reset(); }
    void                               Reset();
    void                               Initialize(IRenderDevice* pDevice);
    void                               Deactivate();
    bool                               PrepareFrame(IDeviceContext* pContext, const RTXPTRayReconstructionSettings& Settings, Uint32 Width, Uint32 Height, Uint32 FrameIndex);
    bool                               Execute(IDeviceContext* pContext, const RTXPTRenderTargets& Targets, const SampleConstants& Frame, bool ResetHistory);
    void                               InvalidateHistory() { m_ResetHistory = true; }
    void                               NotifyInputPreparationFailed();
    const RayReconstructionDesc&       GetDesc() const { return m_Desc; }
    float2                             GetJitter() const { return m_Jitter; }
    const RTXPTRayReconstructionStats& GetStats() const { return m_Stats; }

private:
    void                                     ReleaseReconstruction();
    bool                                     Fail(RTXPTRayReconstructionStatus Status, const char* Message);
    RefCntAutoPtr<IRayReconstructionFactory> m_Factory;
    RefCntAutoPtr<IRayReconstruction>        m_Reconstruction;
    RefCntAutoPtr<IDeviceContext>            m_Context;
    RayReconstructionDesc                    m_Desc;
    RTXPTRayReconstructionStats              m_Stats;
    float2                                   m_Jitter       = {};
    bool                                     m_ResetHistory = true;
    bool                                     m_Failed       = false;
};

} // namespace Diligent
