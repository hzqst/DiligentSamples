// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once
#include "RTXPTRayTracingPass.hpp"
#include "RTXPTRealtimeSettings.hpp"

namespace Diligent
{
// Runtime constants preserve the upstream rate/pattern mapping without rebuilding RT PSOs.
class AdaptiveSampling
{
public:
    bool Initialize(IRenderDevice* Device, IEngineFactory* Factory, IRenderStateCache* Cache, IBuffer* FrameConstants);
    bool PrepareSubSampling(IRenderDevice* Device, IDeviceContext* Context, const RTXPTRealtimeSettings& Settings, SampleMiniConstants& Mini, RTXPTRayTracingDispatch& Dispatch);

private:
    RefCntAutoPtr<IPipelineState>         m_CompactionPSO;
    RefCntAutoPtr<IShaderResourceBinding> m_CompactionSRB;
    RefCntAutoPtr<IBuffer>                m_MiniConstants;
    RefCntAutoPtr<IBuffer>                m_PixelList;
};
} // namespace Diligent
