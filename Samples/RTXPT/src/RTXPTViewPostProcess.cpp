#include "RTXPTViewPostProcess.hpp"
#include "RenderStateCache.h"
#include "PipelineStateCache.h"
#include <cmath>

namespace Diligent
{
bool IsValidRTXPTViewDisplaySettings(const RTXPTViewDisplaySettings& Display)
{
    //NaN fails every comparison, so it is rejected like a value out of range.
    const auto IsWhitePoint = [](float White) {
        return White >= RTXPTMinToneMappingWhite && White <= RTXPTMaxToneMappingWhite;
    };
    const auto IsBloomValue = [](float Value, float Max) {
        return Value >= 0.0f && Value <= Max;
    };
    const bool bExposure = Display.Exposure >= RTXPTMinDisplayExposure && Display.Exposure <= RTXPTMaxDisplayExposure;
    const bool bCurve    = Display.ToneMapOperator <= RTXPTToneMapperOperator::Aces && IsWhitePoint(Display.WhiteMaxLuminance) && IsWhitePoint(Display.WhiteScale);
    const bool bBloom    = IsBloomValue(Display.Bloom.Radius, RTXPTMaxBloomRadius) && IsBloomValue(Display.Bloom.Intensity, RTXPTMaxBloomIntensity);
    return bExposure && bCurve && bBloom && IsValidRTXPTAutoExposure(Display.AutoExposure);
}

RTXPTToneMappingParameters MakeRTXPTToneMappingParameters(const RTXPTViewDisplaySettings& Display)
{
    RTXPTToneMappingParameters Params;
    Params.ToneMapOperator      = Display.ToneMapOperator;
    Params.ExposureCompensation = Display.AutoExposure.Enabled ? 0.0f : std::log2(Display.Exposure);
    Params.WhiteMaxLuminance    = Display.WhiteMaxLuminance;
    Params.WhiteScale           = Display.WhiteScale;
    return Params;
}

bool CreateRTXPTDisplayTarget(IRenderDevice* Device, Uint32 Width, Uint32 Height, const char* Name, ITexture** Texture, ITextureView** RTV)
{
    *Texture = nullptr;
    *RTV     = nullptr;
    if (!Device || !Width || !Height)
        return false;
    TextureDesc Desc;
    Desc.Name      = Name;
    Desc.Type      = RESOURCE_DIM_TEX_2D;
    Desc.Width     = Width;
    Desc.Height    = Height;
    Desc.MipLevels = 1;
    Desc.Format    = TEX_FORMAT_RGBA8_TYPELESS;
    Desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    RefCntAutoPtr<ITexture> Target;
    Device->CreateTexture(Desc, nullptr, &Target);
    if (!Target)
        return false;
    //Encode once on the RTV. The UNORM SRV lets the host copy encoded display bytes without decoding them again.
    TextureViewDesc View;
    View.ViewType = TEXTURE_VIEW_RENDER_TARGET;
    View.Format   = TEX_FORMAT_RGBA8_UNORM_SRGB;
    RefCntAutoPtr<ITextureView> TargetRTV;
    Target->CreateView(View, &TargetRTV);
    if (!TargetRTV)
        return false;
    *Texture = Target.Detach();
    *RTV     = TargetRTV.Detach();
    return true;
}

bool RTXPTViewPostProcess::Initialize(IRenderDevice* Device, IRenderStateCache* Cache, IPipelineStateCache* PSOCache, Uint32 Width, Uint32 Height, TEXTURE_FORMAT DisplayFormat)
{
    m_Ready         = false;
    m_Width         = Width;
    m_Height        = Height;
    m_DisplayFormat = DisplayFormat;
    m_ToneMapping.Reset();
    if (!Device || !Cache || !Width || !Height)
        return false;
    m_Ready = m_Bloom.Initialize(Device, Cache, Width, Height) &&
        m_ToneMapping.Initialize(Device, Device->GetEngineFactory(), Cache, PSOCache, DisplayFormat, true) &&
        m_ToneMapping.ResizeResources(Device, Width, Height, kRTXPTDisplayHDRFormat);
    return m_Ready;
}

RTXPTErrorCode RTXPTViewPostProcess::Apply(IDeviceContext* Context, ITexture* HDR, const RTXPTToneMappingParameters& ToneParameters, const RTXPTBloomParameters& Bloom, RTXPTAutoExposure* AutoExposure, ITextureView* DisplayRTV, RTXPTGpuProfiler* Profiler)
{
    if (!m_Ready)
        return RTXPTErrorCode::NotInitialized;
    if (!Context || !HDR || !DisplayRTV || ToneParameters.AutoExposure || (AutoExposure && !AutoExposure->GetUsedExposure()))
        return RTXPTErrorCode::InvalidArgument;
    const auto& HDRDesc     = HDR->GetDesc();
    const auto& DisplayDesc = DisplayRTV->GetTexture()->GetDesc();
    if (HDRDesc.Width != m_Width || HDRDesc.Height != m_Height || HDRDesc.Format != kRTXPTDisplayHDRFormat ||
        DisplayRTV->GetDesc().Format != m_DisplayFormat || DisplayDesc.Width != m_Width || DisplayDesc.Height != m_Height)
        return RTXPTErrorCode::InvalidArgument;
    //Auto exposure meters the HDR before Bloom, which leaves that HDR as the frame's captures publish it.
    if (AutoExposure)
    {
        const auto MeterCode = AutoExposure->Meter(Context, HDR, m_Width, m_Height, Profiler);
        if (MeterCode != RTXPTErrorCode::Ok)
            return MeterCode;
    }
    ITextureView* ToneSource = nullptr;
    const auto    BloomCode  = m_Bloom.Apply(Context, HDR, Bloom, Profiler, ToneSource);
    if (BloomCode != RTXPTErrorCode::Ok)
        return BloomCode;
    RTXPTToneMappingRenderAttribs Tone;
    Tone.pSourceSRV = ToneSource;
    Tone.pLdrRTV    = DisplayRTV;
    Tone.Width      = m_Width;
    Tone.Height     = m_Height;
    Tone.pParams    = &ToneParameters;
    if (AutoExposure)
        Tone.pExposureSRV = AutoExposure->GetUsedExposure()->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::ToneMapping};
    if (!m_ToneMapping.Render(Context, Tone))
        return RTXPTErrorCode::ToneMappingFailed;
    return RTXPTErrorCode::Ok;
}

RTXPTErrorCode RTXPTViewPostProcess::Render(IDeviceContext* Context, ITexture* HDR, const RTXPTViewDisplaySettings& Display, RTXPTAutoExposure* AutoExposure, double Time, ITextureView* DisplayRTV, RTXPTGpuProfiler* Profiler)
{
    const bool bAuto = Display.AutoExposure.Enabled;
    if (!IsValidRTXPTViewDisplaySettings(Display) || (bAuto && (!AutoExposure || !AutoExposure->IsReady())))
        return RTXPTErrorCode::InvalidArgument;
    RTXPTAutoExposure* const FrameAutoExposure = bAuto ? AutoExposure : nullptr;
    RTXPTAutoExposureFrame   AutoExposureGuard{FrameAutoExposure};
    if (FrameAutoExposure)
    {
        const auto BeginCode = FrameAutoExposure->BeginFrame(Context, Display.AutoExposure, Time);
        if (BeginCode != RTXPTErrorCode::Ok)
            return BeginCode;
    }
    const auto Code = Apply(Context, HDR, MakeRTXPTToneMappingParameters(Display), Display.Bloom, FrameAutoExposure, DisplayRTV, Profiler);
    if (Code == RTXPTErrorCode::Ok)
        AutoExposureGuard.Succeed();
    return Code;
}
} // namespace Diligent
