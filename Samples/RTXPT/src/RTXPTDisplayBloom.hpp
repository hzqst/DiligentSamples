#pragma once

#include "RTXPTBloomPass.hpp"
#include "RTXPTGpuProfiler.hpp"
#include "RTXPTSceneRuntime.hpp"

namespace Diligent
{
struct IRenderStateCache;

//The format of the display HDR every embedded mode tone maps (RTXPTRenderTargetFormats::OutputColor and
//ProcessedOutputColor, and the processed Reference radiance), of the display Bloom target and of the intermediate
//textures of RTXPTBloomPass, as the Sample runs Bloom on ProcessedOutputColor.
constexpr TEXTURE_FORMAT kRTXPTDisplayHDRFormat = TEX_FORMAT_RGBA16_FLOAT;

//The Bloom of one view since Initialize: the Apply calls that drew it, the creations of its resources (the display HDR
//target with the pass, its shaders and intermediate textures) and their GPU bytes, and whether the latest Apply drew it.
struct RTXPTDisplayBloomStats
{
    Uint32 Renders           = 0;
    Uint32 ResourceCreations = 0;
    Uint64 ResourceBytes     = 0;
    bool   LastApplied       = false;
};

//The display Bloom of one embedded view: RTXPTBloomPass, the Sample's Bloom, on the final scene-linear HDR of the view
//at its display size, before tone mapping. Bloom never writes the HDR it reads. An Apply that draws Bloom copies it
//bit-exactly into the view's display HDR target, then downscales, blurs and mixes that target in place exactly as the
//Sample does with ProcessedOutputColor, and tone mapping reads the target. The resources are created by the first Apply
//that draws Bloom and kept for the view at its one display size; an Apply that bypasses Bloom does no GPU work.
class RTXPTDisplayBloom
{
public:
    //Keeps the device, the cache and the shader source of the calling scope (RTXPTShaderSourceScope) for the resources
    //Apply creates. Width and Height are the display size.
    bool Initialize(IRenderDevice* Device, IRenderStateCache* Cache, Uint32 Width, Uint32 Height);
    //Sets ToneSource to the HDR tone mapping reads: Source itself when Params bypass Bloom (IsRTXPTBloomActive),
    //otherwise the display HDR target with Bloom applied. Source must be a display-size kRTXPTDisplayHDRFormat texture.
    //Fails with BloomSetupFailed when the resources cannot be created and BloomFailed when the pass fails, and then
    //leaves ToneSource null.
    RTXPTErrorCode Apply(IDeviceContext* Context, ITexture* Source, const RTXPTBloomParameters& Params, RTXPTGpuProfiler* Profiler, ITextureView*& ToneSource);
    //The display HDR target when the latest Apply drew Bloom into it; null otherwise.
    ITexture*                     GetOutput() const { return m_Stats.LastApplied ? m_Target.RawPtr() : nullptr; }
    const RTXPTDisplayBloomStats& GetStats() const { return m_Stats; }

private:
    bool CreateResources();

    //Declared first, so it is released last: the pipelines of the pass are created from it.
    RefCntAutoPtr<IRenderStateCache>               m_Cache;
    RefCntAutoPtr<IRenderDevice>                   m_Device;
    RefCntAutoPtr<IShaderSourceInputStreamFactory> m_Shaders;
    RTXPTBloomPass                                 m_Pass;
    RefCntAutoPtr<ITexture>                        m_Target;
    RTXPTDisplayBloomStats                         m_Stats;
    Uint32                                         m_Width          = 0;
    Uint32                                         m_Height         = 0;
    bool                                           m_ResourcesReady = false;
};
} // namespace Diligent
