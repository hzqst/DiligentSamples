#pragma once

#include "RTXPTDenoisingGuidesBaker.hpp"
#include "RTXPTFrameConstants.hpp"
#include "RTXPTLightsBaker.hpp"
#include "RTXPTPostProcessPass.hpp"
#include "RTXPTRayTracingPass.hpp"
#include "RTXPTSceneRuntime.hpp"
#include "RTXPTToneMappingPass.hpp"

namespace Diligent
{
struct RTXPTRealtimeRawStats
{
    Uint32 CompletedFrames = 0;
    Uint32 BuildDispatches = 0;
    Uint32 FillDispatches  = 0;
    Uint32 GuideBakes      = 0;
    Uint32 MergeDispatches = 0;
    bool   OutputValid     = false;
    //Previous deformed surfaces and reflection correspondence are implemented separately in M2.
    bool DynamicMotionValidated = false;
};

//One view's provider-free, full-resolution, one-sample current-frame pipeline. The host owns and binds its scene
//ray pass/light sampler; this object owns all layer/guide targets, constants upload, merge and display processing.
//Use on the same ordered context as scene preparation. No progressive accumulation, temporal AA or reconstruction.
class RTXPTRealtimeRawPipeline
{
public:
    RTXPTErrorCode               Initialize(IRenderDevice* Device, IRenderStateCache* Cache, IPipelineStateCache* PSOCache, IBuffer* Constants, Uint32 Width, Uint32 Height, TEXTURE_FORMAT DisplayFormat);
    RTXPTErrorCode               Render(IDeviceContext* Context, RTXPTRayTracingPass& Rays, RTXPTLightsBaker& Lights, const SampleConstants& Constants, const RTXPTToneMappingParameters& ToneParameters, ITextureView* DisplayRTV);
    void                         InvalidateOutput() { m_Stats.OutputValid = false; }
    const RTXPTRealtimeRawStats& GetStats() const { return m_Stats; }
    //Borrowed resources; only the current successful Render may be consumed. No accumulated radiance is allocated.
    const RTXPTRenderTargets& GetTargets() const { return m_Targets; }

private:
    RTXPTRenderTargets        m_Targets;
    RTXPTDenoisingGuidesBaker m_Guides;
    RTXPTPostProcessPass      m_Merge;
    RTXPTToneMappingPass      m_ToneMapping;
    RefCntAutoPtr<IBuffer>    m_Constants;
    RefCntAutoPtr<IBuffer>    m_PixelList;
    RTXPTRealtimeRawStats     m_Stats;
    TEXTURE_FORMAT            m_DisplayFormat = TEX_FORMAT_UNKNOWN;
    bool                      m_Ready         = false;
};
} // namespace Diligent
