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
struct IRayReconstruction;

//How the current-frame layers become linear HDR.
enum class RTXPTRealtimeDenoiser
{
    //Merge the noisy layers of the current frame (RealtimeRaw).
    None,
    //Denoise each layer with its own RELAX instance and history, then merge (RealtimeNative).
    NrdRelax,
};

//What reconstructs the display-size HDR from the noisy layers instead of a denoiser and super resolution.
enum class RTXPTRealtimeReconstructor
{
    None,
    //DLSS ray reconstruction denoises the current frame's noisy radiance with its guides and history, and reconstructs
    //the display size (RealtimeDLSSRR).
    DLSSRR,
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
    //Ray reconstruction pipelines: input preparations, accepted executions, those that restarted its history, and the
    //exposure the latest accepted one received (D4: the display exposure tone mapping applies) with the forward sign of
    //its view (1 left-handed, -1 right-handed).
    Uint32 RayReconstructionPrepares    = 0;
    Uint32 RayReconstructionExecutes    = 0;
    Uint32 RayReconstructionResets      = 0;
    float  RayReconstructionExposure    = 0;
    float  RayReconstructionForwardSign = 0;
    bool   OutputValid                  = false;
    //Motion and history validity of direct rigid/skinned surfaces and single rigid planar reflections have numerical
    //GPU tests. Other paths are reported invalid instead of approximated. This alone is not a denoiser claim.
    bool DynamicMotionValidated = true;
};

//Every argument but PSOCache is required. Native also needs NRD (RTXPTIsNrdAvailable).
//Width and Height are the render size. With SuperResolution or the DLSSRR reconstructor, the layers and guides (and the
//denoiser of SuperResolution) stay at render size and the reconstructed HDR, tone mapping and display target are
//DisplayWidth x DisplayHeight (at least the render size); otherwise the display target is the render size and the
//display size must be 0. DLSSRR replaces the denoiser and super resolution: Denoiser must be None and SuperResolution
//false, and the pipeline neither creates nor needs NRD.
struct RTXPTRealtimePipelineCreateInfo
{
    IRenderDevice*             Device        = nullptr;
    IRenderStateCache*         Cache         = nullptr;
    IPipelineStateCache*       PSOCache      = nullptr;
    IBuffer*                   Constants     = nullptr;
    Uint32                     Width         = 0;
    Uint32                     Height        = 0;
    TEXTURE_FORMAT             DisplayFormat = TEX_FORMAT_UNKNOWN;
    RTXPTRealtimeDenoiser      Denoiser      = RTXPTRealtimeDenoiser::None;
    RTXPTRealtimeReconstructor Reconstructor = RTXPTRealtimeReconstructor::None;
    //Validation control only: false makes NRD ignore the per-pixel history validity of the layers.
    bool HistoryConfidence = true;
    //DLSSRR validation control only (the D4 A/B): false hands RR exposure 1 instead of the display exposure.
    bool   ReconstructionDisplayExposure = true;
    bool   SuperResolution               = false;
    Uint32 DisplayWidth                  = 0;
    Uint32 DisplayHeight                 = 0;
};

//The temporal state the host determined for one frame. Raw ignores the denoiser part.
struct RTXPTRealtimeHistoryAttribs
{
    //Restart every layer's NRD history (CLEAR_AND_RESTART) and the upscaler's or reconstruction's: first frame, cut,
    //missing or failed previous frame.
    bool Reset = true;
    //Negative lets NRD measure the frame time itself; the upscaler then receives 0.
    float TimeDeltaSeconds = -1.0f;
    //Required by a SuperResolution pipeline: the host's upscaler, created for the render and display sizes with
    //RTXPTSuperResolutionInputs::MakeDesc. The frame constants carry its jitter (ApplyJitter). Its history advances
    //with an accepted Execute, so a later failure of this frame leaves the host to reset the next one.
    ISuperResolution* Upscaler = nullptr;
    //Required by a DLSSRR pipeline: the host's reconstruction, created for the render and display sizes with
    //RTXPTRayReconstructionInputs::MakeDesc and explicit exposure, which the pipeline supplies. The frame constants
    //carry its jitter alone (ApplyJitter, no micro-jitter). Its history advances like the upscaler's.
    IRayReconstruction* Reconstruction = nullptr;
};

//One view's one-sample current-frame pipeline. The host owns and binds its scene ray pass/light sampler; this object
//owns all layer/guide/denoiser targets, constants upload, merge, super resolution and ray reconstruction inputs, and
//display processing. Use on the same ordered context as scene preparation. No progressive accumulation or temporal AA.
class RTXPTRealtimePipeline
{
public:
    RTXPTErrorCode             Initialize(const RTXPTRealtimePipelineCreateInfo& CreateInfo);
    RTXPTErrorCode             Render(IDeviceContext* Context, RTXPTRayTracingPass& Rays, RTXPTLightsBaker& Lights, const SampleConstants& Constants, const RTXPTToneMappingParameters& ToneParameters, const RTXPTRealtimeHistoryAttribs& History, ITextureView* DisplayRTV, RTXPTGpuProfiler* Profiler = nullptr);
    void                       InvalidateOutput() { m_Stats.OutputValid = false; }
    RTXPTRealtimeDenoiser      GetDenoiser() const { return m_Denoiser; }
    RTXPTRealtimeReconstructor GetReconstructor() const { return m_Reconstructor; }
    const RTXPTRealtimeStats&  GetStats() const { return m_Stats; }
    //Borrowed resources; only the current successful Render may be consumed. No accumulated radiance is allocated.
    const RTXPTRenderTargets& GetTargets() const { return m_Targets; }
    //A SuperResolution pipeline's converted inputs of the current successful Render.
    const RTXPTSuperResolutionInputPass& GetSuperResolutionInputs() const { return m_SuperResolutionInputs; }
    //A DLSSRR pipeline's 1x1 R32F exposure texture, which its latest Execute read.
    ITexture* GetReconstructionExposure() const { return m_ReconstructionExposure; }

private:
    RTXPTErrorCode Denoise(IDeviceContext* Context, const SampleConstants& Constants, const RTXPTRealtimeHistoryAttribs& History, RTXPTGpuProfiler* Profiler);
    RTXPTErrorCode Upscale(IDeviceContext* Context, const SampleConstants& Constants, const RTXPTRealtimeHistoryAttribs& History, RTXPTGpuProfiler* Profiler);
    RTXPTErrorCode Reconstruct(IDeviceContext* Context, const SampleConstants& Constants, const RTXPTToneMappingParameters& ToneParameters, const RTXPTRealtimeHistoryAttribs& History, RTXPTGpuProfiler* Profiler);

    RTXPTRenderTargets            m_Targets;
    RTXPTDenoisingGuidesBaker     m_Guides;
    RTXPTPostProcessPass          m_Merge;
    RTXPTNrdDenoiser              m_Nrd;
    RTXPTRealtimeSettings         m_NrdSettings;
    RTXPTToneMappingPass          m_ToneMapping;
    RTXPTSuperResolutionInputPass m_SuperResolutionInputs;
    RefCntAutoPtr<IBuffer>        m_Constants;
    RefCntAutoPtr<IBuffer>        m_PixelList;
    RefCntAutoPtr<ITexture>       m_ReconstructionExposure;
    RTXPTRealtimeStats            m_Stats;
    RTXPTRealtimeDenoiser         m_Denoiser                      = RTXPTRealtimeDenoiser::None;
    RTXPTRealtimeReconstructor    m_Reconstructor                 = RTXPTRealtimeReconstructor::None;
    TEXTURE_FORMAT                m_DisplayFormat                 = TEX_FORMAT_UNKNOWN;
    float                         m_ReconstructionExposureValue   = 0;
    bool                          m_ReconstructionDisplayExposure = true;
    bool                          m_SuperResolution               = false;
    bool                          m_Ready                         = false;
};
} // namespace Diligent
