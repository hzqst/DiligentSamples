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

//Static-scene reference renderer for hosts with their own window, device, camera and presentation. All scene
//loading remains with RTXPTScene/RTXPTMaterials. One instance owns one camera's accumulation history.
//The host owns the render state cache and the optional native pipeline cache, so instances it recreates reuse
//the shaders and pipelines compiled for earlier ones; both must outlive the instance.
class RTXPTEmbeddedRenderer
{
public:
    RTXPTEmbeddedRenderer();
    ~RTXPTEmbeddedRenderer();
    RTXPTEmbeddedRenderer(const RTXPTEmbeddedRenderer&) = delete;
    RTXPTEmbeddedRenderer& operator=(const RTXPTEmbeddedRenderer&) = delete;
    bool                   Initialize(IRenderDevice* Device, IDeviceContext* Context, RTXPTScene& Scene, RTXPTMaterials& Materials, IShaderSourceInputStreamFactory* Shaders, IRenderStateCache* Cache, IPipelineStateCache* PSOCache, Uint32 Width, Uint32 Height, std::string& Error);
    bool                   Render(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, bool ResetHistory, std::string& Error);
    ITexture*              GetOutput() const;
    Uint32                 GetTraceCount() const;
    Uint32                 GetAccumulationCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

//Creates a render state cache for RTXPTEmbeddedRenderer that identifies shader files by name. DXC then preprocesses
//the conditional includes, so disabled optional algorithms (for example SHARC) do not need their sources deployed.
//Source edits do not change the identity: a host that persists the cache must version it by the shader contents.
void CreateRTXPTEmbeddedStateCache(IRenderDevice* Device, IRenderStateCache** Cache);
} // namespace Diligent
