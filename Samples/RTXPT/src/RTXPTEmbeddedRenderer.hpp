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

//Static-scene reference renderer for hosts with their own window, device, camera and presentation. All scene
//loading remains with RTXPTScene/RTXPTMaterials. One instance owns one camera's accumulation history.
class RTXPTEmbeddedRenderer
{
public:
    RTXPTEmbeddedRenderer();
    ~RTXPTEmbeddedRenderer();
    RTXPTEmbeddedRenderer(const RTXPTEmbeddedRenderer&) = delete;
    RTXPTEmbeddedRenderer& operator=(const RTXPTEmbeddedRenderer&) = delete;
    bool                   Initialize(IRenderDevice* Device, IDeviceContext* Context, RTXPTScene& Scene, RTXPTMaterials& Materials, IShaderSourceInputStreamFactory* Shaders, Uint32 Width, Uint32 Height, std::string& Error);
    bool                   Render(const PathTracerCameraData& Camera, const PathTracerViewData& View, double Time, bool ResetHistory, std::string& Error);
    ITexture*              GetOutput() const;
    Uint32                 GetTraceCount() const;
    Uint32                 GetAccumulationCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
} // namespace Diligent
