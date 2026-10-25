// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once
#include "RTXPTRayTracingPass.hpp"
#include "BasicMath.hpp"

namespace Diligent
{
struct SharcSettings
{
    bool  Enabled              = false;
    bool  EnableUpdate         = true;
    bool  EnableResolve        = true;
    int   CapacityLog2         = 20;
    int   DownscaleFactor      = 4;
    int   AccumulationFrameNum = 10;
    int   StaleFrameNum        = 64;
    float SceneScale           = 35.f;
    float TerminateThreshold   = 3.5f;
    float MinRoughnessOverride = .4f;
    int   DebugMode            = 0;
};

class SharcIntegration
{
public:
    static bool IsSupported(IRenderDevice* pDevice);
    bool        Initialize(IRenderDevice* pDevice, IEngineFactory* pFactory, IRenderStateCache* pCache);
    bool        Prepare(IRenderDevice* pDevice, IDeviceContext* pContext, const SharcSettings& Settings, Uint32 Width, Uint32 Height, const float3& CameraPosition);
    bool        Resolve(IDeviceContext* pContext);
    void        Bind(RTXPTRayTracingDispatch& Dispatch) const;
    void        Invalidate() { m_NeedsClear = true; }
    void        Reset();
    void        Barrier(IDeviceContext* pContext);

private:
    bool                                  DispatchCompute(IDeviceContext* pContext, bool Clear);
    RefCntAutoPtr<IBuffer>                m_Constants, m_Hash, m_Accumulation, m_Resolved, m_PathState;
    RefCntAutoPtr<IPipelineState>         m_ClearPSO, m_ResolvePSO;
    RefCntAutoPtr<IShaderResourceBinding> m_ClearSRB, m_ResolveSRB;
    Uint32                                m_Capacity = 0, m_PathSlots = 0;
    float                                 m_SceneScale     = 0;
    float3                                m_PreviousCamera = {};
    bool                                  m_NeedsClear     = true;
};
} // namespace Diligent
