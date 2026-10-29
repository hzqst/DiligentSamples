#pragma once

#include <memory>
#include <string>
#include "RTXPTFrameConstants.hpp"
#include "RenderDevice.h"
#include "DeviceContext.h"
#include "Texture.h"

namespace Diligent
{
class RTXPTScene;
class RTXPTMaterials;
struct IRenderStateCache;
struct IPipelineStateCache;

//The embedded renderer serves hosts with their own window, device, camera and presentation. All scene loading remains
//with RTXPTScene/RTXPTMaterials. It has two layers, so the views of one scene share what depends on the scene alone:
// - RTXPTEmbeddedScene: the static acceleration structures, lights, emissive triangles and environment of a scene.
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

//The scene and size of an RTXPTEmbeddedView and how it compiles shaders. Scene, Shaders, Cache and a nonempty size are
//required; PSOCache is optional.
struct RTXPTEmbeddedViewCreateInfo
{
    const RTXPTEmbeddedScene*        Scene    = nullptr;
    IShaderSourceInputStreamFactory* Shaders  = nullptr;
    IRenderStateCache*               Cache    = nullptr;
    IPipelineStateCache*             PSOCache = nullptr;
    Uint32                           Width    = 0;
    Uint32                           Height   = 0;
};

//Static-scene data of RTXPTScene and RTXPTMaterials. It keeps the GPU objects of both that its views bind, so it
//does not need them after Initialize. It must outlive every view created for it.
class RTXPTEmbeddedScene
{
public:
    RTXPTEmbeddedScene();
    ~RTXPTEmbeddedScene();
    RTXPTEmbeddedScene(const RTXPTEmbeddedScene&) = delete;
    RTXPTEmbeddedScene& operator=(const RTXPTEmbeddedScene&) = delete;
    //Builds the scene and waits for the GPU.
    bool Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedSceneCreateInfo& CreateInfo, std::string& Error);
    bool IsReady() const;

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
    //Creates the view of CreateInfo.Scene at CreateInfo.Width x CreateInfo.Height and waits for the GPU.
    bool      Initialize(IRenderDevice* Device, IDeviceContext* Context, const RTXPTEmbeddedViewCreateInfo& CreateInfo, std::string& Error);
    bool      Render(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, bool ResetHistory, std::string& Error);
    ITexture* GetOutput() const;
    Uint32    GetTraceCount() const;
    Uint32    GetAccumulationCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

//Creates a render state cache for the embedded renderer that identifies shader files by name. DXC then preprocesses
//the conditional includes, so disabled optional algorithms (for example SHARC) do not need their sources deployed.
//Source edits do not change the identity: a host that persists the cache must version it by the shader contents.
void CreateRTXPTEmbeddedStateCache(IRenderDevice* Device, IRenderStateCache** Cache);
} // namespace Diligent
