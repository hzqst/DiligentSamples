#pragma once

#include <memory>
#include <string>
#include "RTXPTFrameConstants.hpp"
#include "RTXPTSceneRuntime.hpp"
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
    RTXPTScene*                      Scene     = nullptr;
    RTXPTMaterials*                  Materials = nullptr;
    IShaderSourceInputStreamFactory* Shaders   = nullptr;
    IRenderStateCache*               Cache     = nullptr;
    IPipelineStateCache*             PSOCache  = nullptr;
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
};

enum class RTXPTOutputEncoding
{
    Linear,
    SRGB,
};

//A borrowed result of the latest Render. Texture is null unless that Render succeeded. It remains usable until the
//next Render or Initialize, or destruction of the view. Deferred consumers must hold their own resource references.
//FrameIndex is a one-based successful render count, not a scene frame ID. No depth/motion guides are exposed yet.
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

//Sizes are local to the view texture; host viewport offsets belong to final composition only.
//Reference currently requires equal nonempty render/display sizes and an sRGB RGBA8 output RTV.
struct RTXPTEmbeddedViewCreateInfo
{
    const RTXPTEmbeddedScene*        Scene    = nullptr;
    IShaderSourceInputStreamFactory* Shaders  = nullptr;
    IRenderStateCache*               Cache    = nullptr;
    IPipelineStateCache*             PSOCache = nullptr;
    RTXPTViewSize                    RenderSize;
    RTXPTViewSize                    DisplaySize;
    TEXTURE_FORMAT                   DisplayRTVFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
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

private:
    friend class RTXPTEmbeddedView;
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

//Reference path tracing of one camera of a ready RTXPTEmbeddedScene. One instance owns one camera's accumulation
//history; the views of a scene accumulate independently.
class RTXPTEmbeddedView
{
public:
    RTXPTEmbeddedView();
    ~RTXPTEmbeddedView();
    RTXPTEmbeddedView(const RTXPTEmbeddedView&) = delete;
    RTXPTEmbeddedView& operator=(const RTXPTEmbeddedView&) = delete;
    //Creates new targets and history and waits for the GPU. Reinitialization invalidates previous outputs even on failure.
    RTXPTErrorCode Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedViewCreateInfo& CreateInfo);
    RTXPTErrorCode Render(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, bool ResetHistory);
    ITexture*      GetOutput() const;
    //GetOutput returns the allocated display target even before Render, for recording host commands. Contents are
    //current only after Render succeeds. Its UNORM SRV copies sRGB-encoded bytes without a second decode/encode cycle.
    RTXPTViewOutputInfo GetOutputInfo(RTXPTViewOutput Output) const;
    Uint32              GetTraceCount() const;
    Uint32              GetAccumulationCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

//Creates a render state cache for the embedded renderer that identifies shader files by name. DXC then preprocesses
//the conditional includes, so disabled optional algorithms (for example SHARC) do not need their sources deployed.
//Source edits do not change the identity: a host that persists the cache must version it by the shader contents.
void CreateRTXPTEmbeddedStateCache(IRenderDevice* Device, IRenderStateCache** Cache);
} // namespace Diligent
