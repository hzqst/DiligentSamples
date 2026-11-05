#pragma once

#include "RTXPTGpuProfiler.hpp"
#include "RTXPTSceneRuntime.hpp"
#include "Buffer.h"
#include "PipelineState.h"
#include "ShaderResourceBinding.h"

namespace Diligent
{
struct IRenderStateCache;
struct IPipelineStateCache;

//The metering key of the Sample's tone mapper (RTXPT_TONEMAPPING_EXPOSURE_KEY), and the range of the auto exposure
//limits: the display exposure range, [-32, 32] stops (RTXPTMinDisplayExposure, RTXPTMaxDisplayExposure).
constexpr float RTXPTDefaultAutoExposureKey = 0.042f;
constexpr float RTXPTMinAutoExposureEV      = -32.0f;
constexpr float RTXPTMaxAutoExposureEV      = 32.0f;
//The display exposure whose pre-exposed gray luminance the clamps of the traced radiance (the realtime firefly filter,
//RR's brightness clamp and the guides' average layer radiance) use under auto exposure: a fixed scene-linear reference,
//not the exposure the GPU adapts, so adapting changes no sampling, path weight or clamp. 1 makes the clamps those of
//the default Manual exposure.
constexpr float RTXPTAutoExposureTraceClampExposure = 1.0f;
//The longest frame time one adaptation step uses: a longer pause or missing frames adapt as if this much had passed.
constexpr double RTXPTMaxAutoExposureDeltaSeconds = 1.0;

//The auto exposure of one Render (RTXPTViewDisplaySettings::AutoExposure). Exposures are log2 of the linear multiplier
//tone mapping applies, higher brighter, not EV100. The frame uses 2^clamp(b + CompensationEV, MinEV, MaxEV) for the
//base exposure b of the committed state, or SeedEV when the state restarts (RTXPTAutoExposure::RequestSeed). Key,
//BrightenSeconds and DarkenSeconds steer the adaptation of b towards log2(Key) minus the mean log2 luminance of the
//frame's HDR (RTXPTAutoExposure::Meter). Render rejects a key that is not a finite positive number, limits outside
//[RTXPTMinAutoExposureEV, RTXPTMaxAutoExposureEV] or out of order, time constants that are not finite numbers of at
//least 0 and seeds or compensations that are not finite.
struct RTXPTAutoExposureParameters
{
    bool  Enabled         = false;
    float SeedEV          = 0.0f;
    float CompensationEV  = 0.0f;
    float Key             = RTXPTDefaultAutoExposureKey;
    float MinEV           = -16.0f;
    float MaxEV           = 16.0f;
    float BrightenSeconds = 1.0f;
    float DarkenSeconds   = 0.25f;
};

//Whether Params are ones Render accepts (RTXPTAutoExposureParameters), whatever Enabled.
bool IsValidRTXPTAutoExposure(const RTXPTAutoExposureParameters& Params);

//Why a frame starts from the seed instead of the committed state. None continues the committed state.
enum class RTXPTAutoExposureSeedReason : Uint32
{
    None,
    NewView,
    NewScene,
    CameraCut,
    ManualToAuto,
};

//How the time of a frame advanced the adaptation (RTXPTAutoExposureStats::LastTimeStatus).
enum class RTXPTAutoExposureTimeStatus : Uint32
{
    //The time since the last committed frame, positive.
    Valid,
    //Positive but longer than RTXPTMaxAutoExposureDeltaSeconds, which the frame used instead.
    Clamped,
    //No frame has been committed with a finite time yet: the adaptation holds.
    NoAnchor,
    //No time passed (a paused scene): the adaptation holds.
    NotAdvanced,
    //The time went back: the adaptation holds, and the commit of the frame restarts the clock at its time.
    Backwards,
    //The time of the frame is not finite: the adaptation holds and its commit keeps the previous time.
    NonFinite,
};

const char* GetRTXPTAutoExposureSeedReasonName(RTXPTAutoExposureSeedReason Reason);
const char* GetRTXPTAutoExposureTimeStatusName(RTXPTAutoExposureTimeStatus Status);

//The auto exposure of one view since Initialize. The exposures themselves stay on the GPU: read GetUsedExposure and
//GetPendingState explicitly to see them.
struct RTXPTAutoExposureStats
{
    //Frames begun, meters that wrote a pending state, and commits and aborts of those.
    Uint32 Frames  = 0;
    Uint32 Meters  = 0;
    Uint32 Commits = 0;
    Uint32 Aborts  = 0;
    //Committed frames that started from the seed, and the latest one's reason.
    Uint32                      SeedCommits    = 0;
    RTXPTAutoExposureSeedReason LastSeedCommit = RTXPTAutoExposureSeedReason::None;
    //The seed the next frame starts from, None to continue the committed state, and the one the latest frame began with.
    RTXPTAutoExposureSeedReason PendingSeed = RTXPTAutoExposureSeedReason::None;
    RTXPTAutoExposureSeedReason FrameSeed   = RTXPTAutoExposureSeedReason::None;
    //The time step of the latest frame: its time less that of the last committed frame (0 without one), and the step the
    //adaptation used (0 when it held).
    RTXPTAutoExposureTimeStatus LastTimeStatus      = RTXPTAutoExposureTimeStatus::NoAnchor;
    double                      LastRawDeltaSeconds = 0;
    float                       LastDeltaSeconds    = 0;
    //The state texture that holds the committed state (0 or 1), and whether a metered frame waits for Commit or Abort.
    Uint32 CommittedIndex = 0;
    bool   PendingReady   = false;
    //The size the latest meter read, and the creations and texel bytes of the metering tiles.
    Uint32 MeterWidth    = 0;
    Uint32 MeterHeight   = 0;
    Uint32 TileCreations = 0;
    Uint64 ResourceBytes = 0;
};

/* The auto exposure of one stable view, which its host owns so it outlives the embedded views it serves: a view rebuilt
 * for another mode, size or quality keeps adapting. Every exposure stays on the GPU; nothing is read back to the CPU.
 * Each frame:
 *  1. BeginFrame writes the exposure the frame uses into a 1x1 R32F texture (GetUsedExposure), from the committed state
 *     or the seed, with the frame's compensation and limits. Ray reconstruction and tone mapping read it; it does not
 *     change until the next BeginFrame.
 *  2. Meter reduces the log2 luminance of the frame's linear HDR before Bloom and writes the pending state from it
 *     (RTXPTAutoExposure.csh). The frame's colors keep the exposure of step 1.
 *  3. The host commits the pending state once it has accepted the whole frame, or aborts it. Committing swaps the two
 *     state textures and takes the frame's time as the start of the next step; nothing waits for the GPU. A frame that is
 *     not committed leaves the committed state, its time and a requested seed as they were.
 * Use on one ordered context. A view must not share its state with another.
 */
class RTXPTAutoExposure
{
public:
    //Creates the shaders, pipelines and the state and exposure textures with the shader source of the calling scope
    //(RTXPTShaderSourceScope). Requests the NewView seed.
    RTXPTErrorCode Initialize(IRenderDevice* Device, IRenderStateCache* Cache, IPipelineStateCache* PSOCache);
    bool           IsReady() const { return m_Ready; }
    //Starts the next frames from the seed until a frame that started from it is committed. A pending request keeps its
    //reason.
    void RequestSeed(RTXPTAutoExposureSeedReason Reason);
    //Step 1 for a frame at Time (scene seconds); Params must be valid and enabled. Aborts an uncommitted pending state.
    RTXPTErrorCode BeginFrame(IDeviceContext* Context, const RTXPTAutoExposureParameters& Params, double Time);
    //Step 2: meters the Width x Height top left of HDR, which must cover it. Fails with AutoExposureFailed when the
    //metering tiles cannot be created or the frame was not begun.
    RTXPTErrorCode Meter(IDeviceContext* Context, ITexture* HDR, Uint32 Width, Uint32 Height, RTXPTGpuProfiler* Profiler);
    //Step 3. Both do nothing without a metered frame.
    void Commit();
    void Abort();
    //The exposure of the latest BeginFrame (1x1 R32F, a linear multiplier).
    ITexture* GetUsedExposure() const { return m_Used; }
    //The state the latest Meter wrote (RTXPT_AUTO_EXPOSURE_STATE_TEXELS x 1 RGBA32F, see RTXPTAutoExposureShared.h),
    //committed or not; null before the first Meter. GetCommittedState is the state the next frame continues.
    ITexture*                     GetPendingState() const { return m_bMetered ? m_States[m_MeteredIndex].RawPtr() : nullptr; }
    ITexture*                     GetCommittedState() const { return m_States[m_Stats.CommittedIndex]; }
    const RTXPTAutoExposureStats& GetStats() const { return m_Stats; }

private:
    RefCntAutoPtr<IPipelineState> CreatePipeline(IShaderSourceInputStreamFactory* Shaders, const char* Name, const char* EntryPoint);
    bool                          UpdateConstants(IDeviceContext* Context);
    bool                          PrepareTiles(Uint32 Width, Uint32 Height);
    bool                          Dispatch(IDeviceContext* Context, IShaderResourceBinding* SRB, IPipelineState* PSO, Uint32 GroupsX, Uint32 GroupsY);

    //Declared first, so they are released last: the pipelines are created from them.
    RefCntAutoPtr<IRenderStateCache>      m_Cache;
    RefCntAutoPtr<IPipelineStateCache>    m_PSOCache;
    RefCntAutoPtr<IRenderDevice>          m_Device;
    RefCntAutoPtr<IBuffer>                m_Constants;
    RefCntAutoPtr<IPipelineState>         m_BeginPSO, m_MeterPSO, m_AdaptPSO;
    RefCntAutoPtr<IShaderResourceBinding> m_BeginSRB, m_MeterSRB, m_AdaptSRB;
    RefCntAutoPtr<ITexture>               m_Used;
    RefCntAutoPtr<ITexture>               m_States[2];
    RefCntAutoPtr<IBuffer>                m_Tiles;
    RTXPTAutoExposureParameters           m_Params;
    RTXPTAutoExposureStats                m_Stats;
    //The time of the last committed frame with a finite time, and that of the frame in flight.
    double m_AnchorTime  = 0;
    double m_FrameTime   = 0;
    bool   m_bAnchored   = false;
    bool   m_bFrameBegun = false;
    //Whether the frame in flight started from the seed and how far its time advanced the adaptation.
    bool m_bFrameSeed  = false;
    bool m_bValidDelta = false;
    //The state texture the latest Meter wrote, which a commit makes the committed one.
    Uint32 m_MeteredIndex = 0;
    bool   m_bMetered     = false;
    Uint32 m_TilesX       = 0;
    Uint32 m_TileCount    = 0;
    bool   m_Ready        = false;
};
} // namespace Diligent
