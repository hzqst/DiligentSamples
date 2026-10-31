#pragma once

#include <memory>
#include <string>
#include "RTXPTFrameConstants.hpp"
#include "RTXPTSceneRuntime.hpp"
#include "RTXPTSceneHistory.hpp"
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
struct RTXPTRealtimeRawStats;
class RTXPTGpuProfiler;

enum class RTXPTViewMode
{
    Reference,
    RealtimeRaw,
};

//The embedded renderer serves hosts with their own window, device, camera and presentation. All scene loading remains
//with RTXPTScene/RTXPTMaterials. It has two layers, so the views of one scene share what depends on the scene alone:
// - RTXPTEmbeddedScene: the runtime scene of a loaded scene, its GPU frame (acceleration structures, lights, emissive
//   triangles) and its environment.
// - RTXPTEmbeddedView: the frame targets, light sampling and path tracing bindings, accumulation history and tone
//   mapping of one camera at one size. A new size or a new camera needs only a new view.
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

struct RTXPTViewSize
{
    Uint32 Width  = 0;
    Uint32 Height = 0;
};

enum class RTXPTViewOutput
{
    Display,
    NoisyRadiance,
    AccumulatedRadiance,
    //Raw debug inputs: ray distance is NOT view-space Z; motion remains unvalidated for dynamic surfaces until M2.
    RayDistance,
    PrimaryMotion,
    SpecularHitDistance,
    AverageLayerRadiance,
    LayerHeader,
};

enum class RTXPTOutputEncoding
{
    Linear,
    SRGB,
};

//A borrowed result of the latest Render. Texture is null unless that Render succeeded. It remains usable until the
//next Render or Initialize, or destruction of the view. Deferred consumers must hold their own resource references.
//FrameIndex is a one-based successful render count, not a scene frame ID. Raw guides are debug inputs; their dynamic
//motion correctness is reported separately from output validity.
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

//Sizes are local to the view texture; host viewport offsets belong to final composition only.
//Both modes currently require equal nonempty render/display sizes and an sRGB RGBA8 output RTV.
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
    RTXPTPreparedSceneToken     GetPreparedToken() const;
    RTXPTGpuProfiler&           GetProfiler();

private:
    friend class RTXPTEmbeddedView;
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

//Reference or current-frame Raw path tracing of one camera of a ready RTXPTEmbeddedScene. Each view owns its targets;
//only Reference accumulates across frames. Raw uses the shared provider-free pipeline and publishes debug guides.
class RTXPTEmbeddedView
{
public:
    RTXPTEmbeddedView();
    ~RTXPTEmbeddedView();
    RTXPTEmbeddedView(const RTXPTEmbeddedView&) = delete;
    RTXPTEmbeddedView& operator=(const RTXPTEmbeddedView&) = delete;
    //Creates new targets and history and waits for the GPU. Reinitialization invalidates previous outputs even on failure.
    RTXPTErrorCode Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedViewCreateInfo& CreateInfo);
    //Exposure is a linear display multiplier in [2^-32, 2^32]; it never rescales accumulated/noisy HDR.
    RTXPTErrorCode Render(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, bool ResetHistory, float Exposure = 1.0f);
    ITexture*      GetOutput() const;
    //GetOutput returns the allocated display target even before Render, for recording host commands. Contents are
    //current only after Render succeeds. Its UNORM SRV copies sRGB-encoded bytes without a second decode/encode cycle.
    RTXPTViewOutputInfo          GetOutputInfo(RTXPTViewOutput Output) const;
    const RTXPTRealtimeRawStats& GetRealtimeRawStats() const;
    IBuffer*                     GetRealtimeLayerBuffer() const;
    const RTXPTViewHistoryInfo&  GetHistoryInfo() const;
    Uint32                       GetTraceCount() const;
    Uint32                       GetAccumulationCount() const;
    RTXPTGpuProfiler&            GetProfiler();

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

//Creates a render state cache for the embedded renderer that identifies shader files by name. DXC then preprocesses
//the conditional includes, so disabled optional algorithms (for example SHARC) do not need their sources deployed.
//Source edits do not change the identity: a host that persists the cache must version it by the shader contents.
void CreateRTXPTEmbeddedStateCache(IRenderDevice* Device, IRenderStateCache** Cache);
} // namespace Diligent
