#pragma once

#include "RTXPTDenoisingGuidesBaker.hpp"
#include "RTXPTFrameConstants.hpp"
#include "RTXPTLightsBaker.hpp"
#include "RTXPTNrdDenoiser.hpp"
#include "RTXPTPostProcessPass.hpp"
#include "RTXPTRayTracingPass.hpp"
#include "RTXPTSceneRuntime.hpp"
#include "RTXPTSuperResolutionInputPass.hpp"
#include "RTXPTToneMappingPass.hpp"
#include "RTXPTGpuProfiler.hpp"

namespace Diligent
{
struct ISuperResolution;

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
    //Super resolution pipelines: accepted upscaler executions, and those that restarted its history.
    Uint32 SuperResolutionExecutes = 0;
    Uint32 SuperResolutionResets   = 0;
    bool   OutputValid             = false;
    //Motion and history validity of direct rigid/skinned surfaces and single rigid planar reflections have numerical
    //GPU tests. Other paths are reported invalid instead of approximated. This alone is not a denoiser claim.
    bool DynamicMotionValidated = true;
};

//Every argument but PSOCache is required. Native also needs NRD (RTXPTIsNrdAvailable).
//Width and Height are the render size. With SuperResolution, the layers, guides and denoiser stay at render size and
//the upscaled HDR, tone mapping and display target are DisplayWidth x DisplayHeight (at least the render size);
//without it the display target is the render size and the display size must be 0.
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
    bool   HistoryConfidence = true;
    bool   SuperResolution   = false;
    Uint32 DisplayWidth      = 0;
    Uint32 DisplayHeight     = 0;
};

//The temporal state the host determined for one frame. Raw ignores the denoiser part.
struct RTXPTRealtimeHistoryAttribs
{
    //Restart every layer's NRD history (CLEAR_AND_RESTART) and the upscaler's: first frame, cut, missing or failed
    //previous frame.
    bool Reset = true;
    //Negative lets NRD measure the frame time itself; the upscaler then receives 0.
    float TimeDeltaSeconds = -1.0f;
    //Required by a SuperResolution pipeline: the host's upscaler, created for the render and display sizes with
    //RTXPTSuperResolutionInputs::MakeDesc. The frame constants carry its jitter (ApplyJitter). Its history advances
    //with an accepted Execute, so a later failure of this frame leaves the host to reset the next one.
    ISuperResolution* Upscaler = nullptr;
};

//One view's one-sample current-frame pipeline. The host owns and binds its scene ray pass/light sampler; this object
//owns all layer/guide/denoiser targets, constants upload, merge, super resolution inputs and display processing.
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
    //A SuperResolution pipeline's converted inputs of the current successful Render.
    const RTXPTSuperResolutionInputPass& GetSuperResolutionInputs() const { return m_SuperResolutionInputs; }

private:
    RTXPTErrorCode Denoise(IDeviceContext* Context, const SampleConstants& Constants, const RTXPTRealtimeHistoryAttribs& History, RTXPTGpuProfiler* Profiler);
    RTXPTErrorCode Upscale(IDeviceContext* Context, const SampleConstants& Constants, const RTXPTRealtimeHistoryAttribs& History, RTXPTGpuProfiler* Profiler);

    RTXPTRenderTargets            m_Targets;
    RTXPTDenoisingGuidesBaker     m_Guides;
    RTXPTPostProcessPass          m_Merge;
    RTXPTNrdDenoiser              m_Nrd;
    RTXPTRealtimeSettings         m_NrdSettings;
    RTXPTToneMappingPass          m_ToneMapping;
    RTXPTSuperResolutionInputPass m_SuperResolutionInputs;
    RefCntAutoPtr<IBuffer>        m_Constants;
    RefCntAutoPtr<IBuffer>        m_PixelList;
    RTXPTRealtimeStats            m_Stats;
    RTXPTRealtimeDenoiser         m_Denoiser        = RTXPTRealtimeDenoiser::None;
    TEXTURE_FORMAT                m_DisplayFormat   = TEX_FORMAT_UNKNOWN;
    bool                          m_SuperResolution = false;
    bool                          m_Ready           = false;
};
} // namespace Diligent
