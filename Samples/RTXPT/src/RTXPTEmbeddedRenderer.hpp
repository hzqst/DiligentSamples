#pragma once

#include <memory>
#include <string>
#include "RTXPTFrameConstants.hpp"
#include "RTXPTSceneRuntime.hpp"
#include "RTXPTSceneHistory.hpp"
#include "RTXPTViewPostProcess.hpp"
#include "RTXPTViewUpscaler.hpp"
#include "RenderDevice.h"
#include "DeviceContext.h"
#include "Texture.h"

namespace Diligent
{
class RTXPTScene;
class RTXPTMaterials;
struct RTXPTImportedScene;
struct RTXPTSceneFrameStats;
struct IRenderStateCache;
struct IPipelineStateCache;
struct RTXPTRealtimeStats;
struct RTXPTDisplayBloomStats;
class RTXPTGpuProfiler;
struct ISuperResolutionFactory;
struct IRayReconstructionFactory;

enum class RTXPTViewMode
{
    Reference,
    RealtimeRaw,
    //Current-frame layers denoised by RELAX with per-view, per-layer history; needs NRD (RTXPTIsNrdAvailable).
    RealtimeNative,
    //RealtimeNative at the render size DLSS reports for the display size and quality, then upscaled to the display size
    //by the view's own DLSS feature and history; needs NRD and a super resolution factory with DLSS.
    RealtimeDLSSSR,
    //Current-frame noisy layers at the render size DLSS RR reports for the display size and quality, denoised and
    //reconstructed to the display size by the view's own DLSS RR feature and history instead of NRD and super
    //resolution; needs a ray reconstruction factory with DLSS RR, not NRD.
    RealtimeDLSSRR,
};

//The DLSS RR quality of RealtimeDLSSRR, queried exactly like RTXPTUpscaleQuality. Native (DLAA) renders at the
//display size.
enum class RTXPTRayReconstructionQuality
{
    Native,
    Quality,
    Balanced,
    Performance,
};

//The embedded renderer serves hosts with their own window, device, camera and presentation. All scene loading remains
//with RTXPTScene/RTXPTMaterials. It has two layers, so the views of one scene share what depends on the scene alone:
// - RTXPTEmbeddedScene: the runtime scene of a loaded scene, its GPU frame (acceleration structures, lights, emissive
//   triangles) and its environment.
// - RTXPTEmbeddedView: the frame targets, light sampling and path tracing bindings, accumulation history, Bloom and
//   tone mapping of one camera at one size. A new size or a new camera needs only a new view.
//The host owns the render state cache and the optional native pipeline cache, so objects it recreates reuse the shaders
//and pipelines compiled for earlier ones. Scenes and views keep references to both, which the shaders and pipelines
//created from them need until they are released.

class RTXPTEmbeddedScene;

//The source of an RTXPTEmbeddedScene and how it compiles shaders. Scene, Materials, Shaders and Cache are required;
//PSOCache is optional.
struct RTXPTEmbeddedSceneCreateInfo
{
    RTXPTScene*                      Scene           = nullptr;
    RTXPTMaterials*                  Materials       = nullptr;
    IShaderSourceInputStreamFactory* Shaders         = nullptr;
    IRenderStateCache*               Cache           = nullptr;
    IPipelineStateCache*             PSOCache        = nullptr;
    bool                             EnableProfiling = false;
};

enum class RTXPTViewOutput
{
    Display,
    //Current-frame linear HDR of Reference and Raw. Native never publishes its denoised result as noisy radiance.
    NoisyRadiance,
    AccumulatedRadiance,
    //Native and DLSSSR: the merged render-size linear HDR of the denoised layers, before super resolution and tone mapping.
    DenoisedRadiance,
    //Realtime guides: ray distance is NOT view-space Z; RTXPTRealtimeStats::DynamicMotionValidated states the motion scope.
    RayDistance,
    PrimaryMotion,
    // Current-to-previous render-local pixel motion for one supported reflected path; w is validity.
    SpecularMotion,
    SpecularHitDistance,
    AverageLayerRadiance,
    LayerHeader,
    //Native and DLSSSR, per denoising layer (R8 UNORM): the history confidence NRD received, 0 where the layer's path has no
    //valid previous correspondence.
    LayerHistoryConfidence,
    //DLSSSR only: the display-size linear HDR of the upscaler, before tone mapping.
    UpscaledRadiance,
    //DLSSSR only, at render size: the depth (R32F, forward [0,1], far 1), motion (RG16F, current-to-previous render
    //pixels) and ignore-history mask (R8 UNORM) the upscaler received.
    SuperResolutionDepth,
    SuperResolutionMotion,
    SuperResolutionIgnoreHistoryMask,
    //DLSSRR only: the display-size linear HDR of the reconstruction, before tone mapping.
    ReconstructedRadiance,
    //DLSSRR only, at render size: the inputs the reconstruction received. Color is the current frame's noisy
    //scene-linear radiance (RGBA16F, brightness-clamped); depth the forward-positive linear view depth of the dominant
    //path's end (R32F, 5,000,000 for sky); motion and specular motion are current-to-previous render pixels (RG16F);
    //normals are world-space unit vectors with linear roughness in w (RGBA16F); albedos are RGBA16F.
    RayReconstructionColor,
    RayReconstructionDepth,
    RayReconstructionMotion,
    RayReconstructionSpecularMotion,
    RayReconstructionNormalRoughness,
    RayReconstructionDiffuseAlbedo,
    RayReconstructionSpecularAlbedo,
    //DLSSRR only: the 1x1 R32F exposure the reconstruction read (D4).
    RayReconstructionExposure,
    //Every mode, when the latest Render applied Bloom (RTXPTViewDisplaySettings::Bloom): the display-size linear HDR
    //after Bloom, before tone mapping (RGBA16F). The HDR of the mode that Bloom read keeps its own output above.
    BloomRadiance,
    //Every mode, when the latest Render used auto exposure: the exposure it applied (1x1 R32F, a linear multiplier),
    //which DLSSRR's reconstruction read as well, and the state its meter wrote for the next frame
    //(RTXPTAutoExposure::GetPendingState). They belong to the view's RTXPTAutoExposure: its next frame overwrites them.
    AutoExposureUsed,
    AutoExposureState,
};

enum class RTXPTOutputEncoding
{
    Linear,
    SRGB,
};

//A borrowed result of the latest Render. Texture is null unless that Render succeeded. It remains usable until the
//next Render or Initialize, or destruction of the view. Deferred consumers must hold their own resource references.
//FrameIndex is a one-based successful render count, not a scene frame ID. Realtime guides are debug inputs; their
//dynamic motion correctness is reported separately from output validity.
struct RTXPTViewOutputInfo
{
    ITexture*           Texture = nullptr;
    RTXPTViewSize       Size;
    TEXTURE_FORMAT      ResourceFormat = TEX_FORMAT_UNKNOWN;
    TEXTURE_FORMAT      SRVFormat      = TEX_FORMAT_UNKNOWN;
    RTXPTOutputEncoding Encoding       = RTXPTOutputEncoding::Linear;
    Uint32              FrameIndex     = 0;
    bool                Valid          = false;
};

// Current successful Render only; invalidated with the output on failure. Tokens are not GPU completion fences.
struct RTXPTViewHistoryInfo
{
    RTXPTPreparedSceneToken Scene;
    RTXPTPreparedSceneToken PreviousViewScene;
    bool                    CameraCorrespondenceValid     = false;
    bool                    LightFeedbackReset            = false;
    Uint32                  LightFeedbackHistoryFrames    = 0;
    Uint32                  ValidPreviousRigidInstances   = 0;
    Uint32                  ValidPreviousSkinnedInstances = 0;
    Uint32                  InvalidHistoryInstances       = 0;
    Uint64                  PreviousTransformStorageBytes = 0;
    Uint64                  PreviousSkinnedRangeBytes     = 0;
    Uint64                  SkinnedVertexStorageBytes     = 0;
};

//The DLSS RR feature of a RealtimeDLSSRR view; empty for the other modes. The fields follow
//RTXPTViewSuperResolutionInfo; Failures counts Render calls whose input preparation or Execute failed.
struct RTXPTViewRayReconstructionInfo
{
    std::string                   Variant;
    RTXPTRayReconstructionQuality Quality = RTXPTRayReconstructionQuality::Native;
    //The DLSS preset of the feature: the Core default F (D5).
    std::string   Preset;
    RTXPTViewSize RenderSize;
    RTXPTViewSize DisplaySize;
    float2        Jitter    = float2{0, 0};
    bool          LastReset = false;
    //The exposure the latest successful Render handed RR: the display exposure tone mapping applies (D4). With auto
    //exposure RR read that exposure from the GPU (RTXPTViewOutput::AutoExposureUsed): AutoExposure is set and Exposure
    //is 0.
    float Exposure     = 0;
    bool  AutoExposure = false;
    //The sign of the forward axis of the latest successful Render's view: 1 for a left-handed view, -1 for a
    //right-handed one (forward = -Z). RR's depth and matrices use that forward-positive basis.
    float  ForwardSign = 0;
    Uint32 Executes    = 0;
    Uint32 Resets      = 0;
    Uint32 Failures    = 0;
};

//Sizes are local to the view texture; host viewport offsets belong to final composition only. Every mode requires a
//nonempty display size and an sRGB RGBA8 output RTV. Reference, Raw and Native render at the display size, which
//RenderSize must equal. RealtimeDLSSSR and RealtimeDLSSRR leave RenderSize empty: the DLSS query for the display size
//and quality determines it (GetRenderSize), and the host's camera then describes that size.
struct RTXPTEmbeddedViewCreateInfo
{
    const RTXPTEmbeddedScene*        Scene    = nullptr;
    IShaderSourceInputStreamFactory* Shaders  = nullptr;
    IRenderStateCache*               Cache    = nullptr;
    IPipelineStateCache*             PSOCache = nullptr;
    RTXPTViewSize                    RenderSize;
    RTXPTViewSize                    DisplaySize;
    TEXTURE_FORMAT                   DisplayRTVFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
    RTXPTViewMode                    Mode             = RTXPTViewMode::Reference;
    bool                             EnableProfiling  = false;
    //Hosts that can reject a submitted frame explicitly CommitFrame after Render succeeds, or InvalidateOutput.
    bool DeferredFrameCommit = false;
    //Native validation control only: false makes NRD ignore the per-pixel history validity of the layers.
    bool NativeHistoryConfidence = true;
    //RealtimeDLSSSR only: the device's super resolution factory, which the view keeps, and the quality.
    ISuperResolutionFactory*     SuperResolutionFactory  = nullptr;
    RTXPTUpscaleQuality          UpscaleQuality          = RTXPTUpscaleQuality::Quality;
    RTXPTSuperResolutionProvider SuperResolutionProvider = RTXPTSuperResolutionProvider::DLSS;
    //RealtimeDLSSRR only: the device's ray reconstruction factory, which the view keeps, and the quality.
    IRayReconstructionFactory*    RayReconstructionFactory = nullptr;
    RTXPTRayReconstructionQuality ReconstructionQuality    = RTXPTRayReconstructionQuality::Native;
    //RealtimeDLSSRR validation control only (the D4 A/B): false hands RR exposure 1 instead of the display exposure.
    bool ReconstructionDisplayExposure = true;
};

//The runtime scene of RTXPTScene and RTXPTMaterials and its prepared GPU frame. It keeps the GPU objects of both that
//its frames use, so it does not need them after Initialize. It must outlive every view created for it.
//The host may add its own assets and instances to the runtime scene, and change or disable the imported instances,
//through GetRuntime; it then prepares the snapshots it commits.
class RTXPTEmbeddedScene
{
public:
    RTXPTEmbeddedScene();
    ~RTXPTEmbeddedScene();
    RTXPTEmbeddedScene(const RTXPTEmbeddedScene&) = delete;
    RTXPTEmbeddedScene& operator=(const RTXPTEmbeddedScene&) = delete;
    //Imports the scene, prepares its first frame and waits for the GPU.
    RTXPTErrorCode Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedSceneCreateInfo& CreateInfo);
    bool           IsReady() const;
    //Prepares the frame of the current scene state; views render the last prepared frame. Call once per host frame
    //before rendering its views, on the context the views render with.
    RTXPTErrorCode Prepare(IDeviceContext* Context);
    //Prepares the frame of Snapshot, a snapshot committed by GetRuntime, as Prepare does.
    RTXPTErrorCode Prepare(IDeviceContext* Context, const RTXPTSceneSnapshotPtr& Snapshot);
    //The runtime scene, with the imported scene as its first commit. Valid once the scene is ready.
    RTXPTSceneRuntime&        GetRuntime();
    const RTXPTImportedScene& GetImportedScene() const;
    //The work counters of the frames prepared for the scene since Initialize, its first frame included: each Prepare
    //prepares one frame and builds its TLAS once.
    const RTXPTSceneFrameStats& GetFrameStats() const;
    //The environment cube the views trace (radiance before the scene's radianceScale), baked from the scene's
    //EnvironmentLight source at its face size (at most 2048), or a black cube without one. Valid once ready.
    ITextureView*           GetEnvironmentMapSRV() const;
    RTXPTPreparedSceneToken GetPreparedToken() const;
    RTXPTGpuProfiler&       GetProfiler();

private:
    friend class RTXPTEmbeddedView;
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

//Reference or current-frame realtime path tracing of one camera of a ready RTXPTEmbeddedScene. Each view owns its
//targets; only Reference accumulates radiance across frames. Raw, Native, DLSSSR and DLSSRR use the shared realtime
//pipeline and publish debug guides; Native and DLSSSR keep one NRD history per layer and restart it whenever the camera
//correspondence of the frame is lost (first frame, cut, missing or failed frame, InvalidateOutput). DLSSSR and DLSSRR
//restart their DLSS history at the same points, and inject the DLSS jitter of their successful frame count into the
//camera.
class RTXPTEmbeddedView
{
public:
    RTXPTEmbeddedView();
    ~RTXPTEmbeddedView();
    RTXPTEmbeddedView(const RTXPTEmbeddedView&) = delete;
    RTXPTEmbeddedView& operator=(const RTXPTEmbeddedView&) = delete;
    //Creates new targets and history and waits for the GPU. Reinitialization invalidates previous outputs even on failure.
    RTXPTErrorCode Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedViewCreateInfo& CreateInfo);
    //Retries a failed SR object creation with its selected query; CreateInfo must name the same provider, quality
    //and display size. Auto selection is retained even if provider availability changes during recovery.
    RTXPTErrorCode RetryInitialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedViewCreateInfo& CreateInfo);
    //Display is applied to this frame alone (RTXPTViewDisplaySettings). Camera and View describe the render size without
    //jitter; DLSSSR and DLSSRR add their own. AutoExposure is the auto exposure of the view's camera, required when
    //Display enables it and ignored otherwise: Render begins its frame at Time and meters it, and the host commits that
    //frame once it has accepted the whole frame (RTXPTAutoExposure::Commit). A Render that fails aborts it.
    RTXPTErrorCode Render(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, bool ResetHistory, const RTXPTViewDisplaySettings& Display = {}, RTXPTAutoExposure* AutoExposure = nullptr);
    // Revokes submitted output and temporal history after the host observes an execution failure.
    // Does not wait for the GPU or release resources that submitted commands may still reference.
    void InvalidateOutput();
    void CommitFrame();
    //GetOutput returns the allocated display target even before Render, for recording host commands. Contents are
    //current only after Render succeeds. Its UNORM SRV copies sRGB-encoded bytes without a second decode/encode cycle.
    ITexture* GetOutput() const;
    //Layer selects the denoising layer of per-layer outputs and must be 0 for the others.
    RTXPTViewOutputInfo                   GetOutputInfo(RTXPTViewOutput Output, Uint32 Layer = 0) const;
    RTXPTViewMode                         GetMode() const;
    RTXPTViewSize                         GetRenderSize() const;
    RTXPTViewSize                         GetDisplaySize() const;
    const RTXPTViewSuperResolutionInfo&   GetSuperResolutionInfo() const;
    const RTXPTViewRayReconstructionInfo& GetRayReconstructionInfo() const;
    const RTXPTRealtimeStats&             GetRealtimeStats() const;
    //The display Bloom of the view since Initialize (RTXPTDisplayBloom).
    const RTXPTDisplayBloomStats& GetBloomStats() const;
    IBuffer*                      GetRealtimeLayerBuffer() const;
    const RTXPTViewHistoryInfo&   GetHistoryInfo() const;
    Uint32                        GetTraceCount() const;
    Uint32                        GetAccumulationCount() const;
    RTXPTGpuProfiler&             GetProfiler();

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    RTXPTErrorCode        Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedViewCreateInfo& CreateInfo, RTXPTViewUpscaler* SelectedUpscaler);
};

//Creates a render state cache for the embedded renderer that identifies shader files by name. DXC then preprocesses
//the conditional includes, so disabled optional algorithms (for example SHARC) do not need their sources deployed.
//Source edits do not change the identity: a host that persists the cache must version it by the shader contents.
void CreateRTXPTEmbeddedStateCache(IRenderDevice* Device, IRenderStateCache** Cache);
} // namespace Diligent
