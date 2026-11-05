#include "RTXPTDisplayBloom.hpp"
#include "RTXPTShaderSource.hpp"
#include "GraphicsAccessories.hpp"
#include "RenderStateCache.h"

namespace Diligent
{
namespace
{
//The textures of RTXPTBloomPass at the size of its second downscale: that downscale and both blur targets.
constexpr Uint64 kBloomQuarterSizeTextures = 3;
} // namespace

bool RTXPTDisplayBloom::Initialize(IRenderDevice* Device, IRenderStateCache* Cache, Uint32 Width, Uint32 Height)
{
    m_Pass.Reset();
    m_Target.Release();
    m_Shaders.Release();
    m_Stats          = {};
    m_ResourcesReady = false;
    m_Device         = Device;
    m_Cache          = Cache;
    m_Width          = Width;
    m_Height         = Height;
    if (!Device || !Cache || !Width || !Height)
        return false;
    //The search paths of RTXPTBloomPass apply where no host scope supplies the source.
    CreateRTXPTShaderSourceFactory(Device->GetEngineFactory(), "shaders;shaders\\PostProcessing", &m_Shaders);
    return m_Shaders != nullptr;
}

//Creates the pass with its shaders, pipelines and intermediate textures, and the display HDR target, once for the view.
//A failed creation is retried by the next Apply that draws Bloom.
bool RTXPTDisplayBloom::CreateResources()
{
    if (m_ResourcesReady)
        return true;
    RTXPTShaderSourceScope ShaderScope{m_Shaders};
    if (!m_Pass.IsReady() && !m_Pass.Initialize(m_Device, m_Device->GetEngineFactory(), m_Cache))
        return false;
    if (!m_Target)
    {
        TextureDesc Desc;
        Desc.Name      = "RTXPT display Bloom HDR";
        Desc.Type      = RESOURCE_DIM_TEX_2D;
        Desc.Width     = m_Width;
        Desc.Height    = m_Height;
        Desc.MipLevels = 1;
        Desc.Format    = kRTXPTDisplayHDRFormat;
        Desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
        m_Device->CreateTexture(Desc, nullptr, &m_Target);
        if (!m_Target)
            return false;
    }
    if (!m_Pass.ResizeResources(m_Device, m_Width, m_Height, kRTXPTDisplayHDRFormat))
        return false;
    //Texel bytes of the target and the pass's textures; the allocations may be larger.
    const auto&  Pass       = m_Pass.GetStats();
    const auto&  Format     = GetTextureFormatAttribs(kRTXPTDisplayHDRFormat);
    const Uint64 TexelBytes = Uint64{Format.ComponentSize} * Format.NumComponents;
    const Uint64 Texels     = Uint64{m_Width} * m_Height + Uint64{Pass.DownscaleWidth} * Pass.DownscaleHeight +
        kBloomQuarterSizeTextures * Pass.BlurWidth * Pass.BlurHeight;
    m_Stats.ResourceBytes = Texels * TexelBytes;
    ++m_Stats.ResourceCreations;
    m_ResourcesReady = true;
    return true;
}

RTXPTErrorCode RTXPTDisplayBloom::Apply(IDeviceContext* Context, ITexture* Source, const RTXPTBloomParameters& Params, RTXPTGpuProfiler* Profiler, ITextureView*& ToneSource)
{
    m_Stats.LastApplied = false;
    ToneSource          = nullptr;
    if (!Context || !Source || !m_Device)
        return RTXPTErrorCode::InvalidArgument;
    const auto& SourceDesc = Source->GetDesc();
    if (SourceDesc.Width != m_Width || SourceDesc.Height != m_Height || SourceDesc.Format != kRTXPTDisplayHDRFormat)
        return RTXPTErrorCode::InvalidArgument;
    if (!IsRTXPTBloomActive(Params))
    {
        ToneSource = Source->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        return RTXPTErrorCode::Ok;
    }
    if (!CreateResources())
        return RTXPTErrorCode::BloomSetupFailed;

    RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::Bloom};
    //The final mix blends into its target, which therefore starts as the original HDR: same format and size, so the
    //copy neither converts nor clamps it.
    CopyTextureAttribs Copy{Source, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, m_Target, RESOURCE_STATE_TRANSITION_MODE_TRANSITION};
    Context->CopyTexture(Copy);
    RTXPTBloomRenderAttribs Attribs;
    Attribs.pSourceSRV = m_Target->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    Attribs.pTargetRTV = m_Target->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    Attribs.Width      = m_Width;
    Attribs.Height     = m_Height;
    Attribs.Format     = kRTXPTDisplayHDRFormat;
    Attribs.Params     = Params;
    if (!m_Pass.Render(Context, Attribs))
        return RTXPTErrorCode::BloomFailed;
    ++m_Stats.Renders;
    m_Stats.LastApplied = true;
    ToneSource          = Attribs.pSourceSRV;
    return RTXPTErrorCode::Ok;
}
} // namespace Diligent
