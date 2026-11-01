#pragma once

#include "RTXPTDenoisingGuidesBaker.hpp"
#include "RTXPTFrameConstants.hpp"
#include "RTXPTLightsBaker.hpp"
#include "RTXPTNrdDenoiser.hpp"
#include "RTXPTPostProcessPass.hpp"
#include "RTXPTRayTracingPass.hpp"
#include "RTXPTSceneRuntime.hpp"
#include "RTXPTToneMappingPass.hpp"
#include "RTXPTGpuProfiler.hpp"

namespace Diligent
{
//How the current-frame layers become linear HDR.
enum class RTXPTRealtimeDenoiser
{
    //Merge the noisy layers of the current frame (RealtimeRaw).
    None,
    //Denoise each layer with its own RELAX instance and history, then merge (RealtimeNative).
    NrdRelax,
};

struct RTXPTRealtimeStats
{
    Uint32 CompletedFrames   = 0;
    Uint32 BuildDispatches   = 0;
    Uint32 FillDispatches    = 0;
    Uint32 GuideBakes        = 0;
    Uint32 MergeDispatches   = 0;
    Uint32 DenoiseDispatches = 0;
    //Frames whose NRD layers started from CLEAR_AND_RESTART; LastHistoryReset is the latest successful frame's mode.
    Uint32 HistoryResets    = 0;
    bool   LastHistoryReset = false;
    //One NRD instance per denoising layer of this view; its permanent and transient pool textures.
    Uint32 NrdInstances      = 0;
    Uint64 NrdPoolBytes      = 0;
    bool   HistoryConfidence = false;
    bool   OutputValid       = false;
    //Motion and history validity of direct rigid/skinned surfaces and single rigid planar reflections have numerical
    //GPU tests. Other paths are reported invalid instead of approximated. This alone is not a denoiser claim.
    bool DynamicMotionValidated = true;
};

//Every argument but PSOCache is required. Native also needs NRD (RTXPTIsNrdAvailable).
struct RTXPTRealtimePipelineCreateInfo
{
    IRenderDevice*        Device        = nullptr;
    IRenderStateCache*    Cache         = nullptr;
    IPipelineStateCache*  PSOCache      = nullptr;
    IBuffer*              Constants     = nullptr;
    Uint32                Width         = 0;
    Uint32                Height        = 0;
    TEXTURE_FORMAT        DisplayFormat = TEX_FORMAT_UNKNOWN;
    RTXPTRealtimeDenoiser Denoiser      = RTXPTRealtimeDenoiser::None;
    //Validation control only: false makes NRD ignore the per-pixel history validity of the layers.
    bool HistoryConfidence = true;
};

//The temporal state the host determined for one frame. Raw ignores it.
struct RTXPTRealtimeHistoryAttribs
{
    //Restart every layer's NRD history (CLEAR_AND_RESTART): first frame, cut, missing or failed previous frame.
    bool Reset = true;
    //Negative lets NRD measure the frame time itself.
    float TimeDeltaSeconds = -1.0f;
};

//One view's full-resolution, one-sample current-frame pipeline. The host owns and binds its scene ray pass/light
//sampler; this object owns all layer/guide/denoiser targets, constants upload, merge and display processing.
//Use on the same ordered context as scene preparation. No progressive accumulation, temporal AA or reconstruction.
class RTXPTRealtimePipeline
{
public:
    RTXPTErrorCode            Initialize(const RTXPTRealtimePipelineCreateInfo& CreateInfo);
    RTXPTErrorCode            Render(IDeviceContext* Context, RTXPTRayTracingPass& Rays, RTXPTLightsBaker& Lights, const SampleConstants& Constants, const RTXPTToneMappingParameters& ToneParameters, const RTXPTRealtimeHistoryAttribs& History, ITextureView* DisplayRTV, RTXPTGpuProfiler* Profiler = nullptr);
    void                      InvalidateOutput() { m_Stats.OutputValid = false; }
    RTXPTRealtimeDenoiser     GetDenoiser() const { return m_Denoiser; }
    const RTXPTRealtimeStats& GetStats() const { return m_Stats; }
    //Borrowed resources; only the current successful Render may be consumed. No accumulated radiance is allocated.
    const RTXPTRenderTargets& GetTargets() const { return m_Targets; }

private:
    RTXPTErrorCode Denoise(IDeviceContext* Context, const SampleConstants& Constants, const RTXPTRealtimeHistoryAttribs& History, RTXPTGpuProfiler* Profiler);

    RTXPTRenderTargets        m_Targets;
    RTXPTDenoisingGuidesBaker m_Guides;
    RTXPTPostProcessPass      m_Merge;
    RTXPTNrdDenoiser          m_Nrd;
    RTXPTRealtimeSettings     m_NrdSettings;
    RTXPTToneMappingPass      m_ToneMapping;
    RefCntAutoPtr<IBuffer>    m_Constants;
    RefCntAutoPtr<IBuffer>    m_PixelList;
    RTXPTRealtimeStats        m_Stats;
    RTXPTRealtimeDenoiser     m_Denoiser      = RTXPTRealtimeDenoiser::None;
    TEXTURE_FORMAT            m_DisplayFormat = TEX_FORMAT_UNKNOWN;
    bool                      m_Ready         = false;
};
} // namespace Diligent
