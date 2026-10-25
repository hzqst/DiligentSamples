/*
 *  Copyright 2026 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#include "RTXPTSuperResolutionPass.hpp"

#include <algorithm>

#include "DebugUtilities.hpp"
#include "GraphicsTypesX.hpp"
#include "RenderStateCache.h"
#include "SuperResolutionFactoryLoader.h"

namespace Diligent
{
namespace
{
constexpr const char*    kSRProviderUnavailableReason = "super resolution provider unavailable";
constexpr const char*    kSRVariantsUnavailableReason = "super resolution variants unavailable";
constexpr TEXTURE_FORMAT kSRMotionFormat              = TEX_FORMAT_RG16_FLOAT;
constexpr Uint32         kSRMotionThreadGroupSize     = 8;

bool ContainsTemporalVariant(const std::vector<SuperResolutionInfo>& Variants)
{
    return std::any_of(Variants.begin(), Variants.end(), [](const SuperResolutionInfo& Info) {
        return Info.Type == SUPER_RESOLUTION_TYPE_TEMPORAL;
    });
}

bool SupportsBindFlags(IRenderDevice* pDevice, TEXTURE_FORMAT Format, BIND_FLAGS BindFlags)
{
    return pDevice != nullptr && (pDevice->GetTextureFormatInfoExt(Format).BindFlags & BindFlags) == BindFlags;
}

ITextureView* GetDefaultView(const RefCntAutoPtr<ITexture>& Texture, TEXTURE_VIEW_TYPE ViewType)
{
    return Texture ? Texture->GetDefaultView(ViewType) : nullptr;
}

bool SetDynamicVariable(IShaderResourceBinding* pSRB, const char* Name, IDeviceObject* pObject)
{
    IShaderResourceVariable* pVar = pSRB != nullptr ? pSRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name) : nullptr;
    if (pVar == nullptr)
    {
        UNEXPECTED("RTXPT super resolution motion conversion dynamic variable is missing: ", Name);
        return false;
    }
    if (pObject == nullptr)
        return false;

    pVar->Set(pObject);
    return true;
}

bool UpscalerDescMatches(const SuperResolutionDesc& Lhs, const SuperResolutionDesc& Rhs)
{
    return Lhs.VariantId == Rhs.VariantId &&
        Lhs.InputWidth == Rhs.InputWidth &&
        Lhs.InputHeight == Rhs.InputHeight &&
        Lhs.OutputWidth == Rhs.OutputWidth &&
        Lhs.OutputHeight == Rhs.OutputHeight &&
        Lhs.OutputFormat == Rhs.OutputFormat &&
        Lhs.ColorFormat == Rhs.ColorFormat &&
        Lhs.DepthFormat == Rhs.DepthFormat &&
        Lhs.MotionFormat == Rhs.MotionFormat &&
        Lhs.Flags == Rhs.Flags;
}

RTXPTSuperResolutionFrameDesc MakeDirectFrameDesc(Uint32                              DisplayWidth,
                                                  Uint32                              DisplayHeight,
                                                  TEXTURE_FORMAT                      OutputFormat,
                                                  const RTXPTSuperResolutionSettings& Settings,
                                                  bool                                ResetHistory,
                                                  float                               TimeDeltaSeconds)
{
    RTXPTSuperResolutionFrameDesc  FrameDesc;
    const Uint32                   Width  = std::max(DisplayWidth, 1u);
    const Uint32                   Height = std::max(DisplayHeight, 1u);
    const RTXPTRenderTargetFormats Formats;

    FrameDesc.Dimensions.RenderWidth           = Width;
    FrameDesc.Dimensions.RenderHeight          = Height;
    FrameDesc.Dimensions.DisplayWidth          = Width;
    FrameDesc.Dimensions.DisplayHeight         = Height;
    FrameDesc.Dimensions.SuperResolutionActive = false;
    FrameDesc.ColorFormat                      = Formats.SuperResolutionInputColor;
    FrameDesc.DepthFormat                      = Formats.Depth;
    FrameDesc.MotionFormat                     = kSRMotionFormat;
    FrameDesc.OutputFormat                     = OutputFormat;
    FrameDesc.ResetHistory                     = ResetHistory;
    FrameDesc.Sharpness                        = Settings.Sharpness;
    FrameDesc.TimeDeltaSeconds                 = TimeDeltaSeconds;

    return FrameDesc;
}

void UpdateFrameStats(RTXPTSuperResolutionStats& Stats, const RTXPTSuperResolutionFrameDesc& FrameDesc)
{
    Stats.RenderWidth       = FrameDesc.Dimensions.RenderWidth;
    Stats.RenderHeight      = FrameDesc.Dimensions.RenderHeight;
    Stats.DisplayWidth      = FrameDesc.Dimensions.DisplayWidth;
    Stats.DisplayHeight     = FrameDesc.Dimensions.DisplayHeight;
    Stats.LastFrameTemporal = FrameDesc.Temporal;
}
} // namespace

void RTXPTSuperResolutionPass::Reset()
{
    m_Upscaler.Release();
    m_MotionConversionPSO.Release();
    m_MotionConversionSRB.Release();
    m_SRMotionVectors.Release();
    m_Factory.Release();
    m_Device.Release();
    m_Variants.clear();
    m_Stats          = {};
    m_UpscalerDesc   = {};
    m_SRMotionWidth  = 0;
    m_SRMotionHeight = 0;
}

bool RTXPTSuperResolutionPass::Initialize(IRenderDevice* pDevice, IRenderStateCache* pStateCache)
{
    Reset();

    if (pDevice == nullptr)
    {
        DEV_ERROR("RTXPT super resolution pass requires a render device");
        return false;
    }

    m_Device = pDevice;

    SuperResolutionFactoryCreateInfo FactoryCI;
    FactoryCI.pDevice = pDevice;
    LoadAndCreateSuperResolutionFactory(FactoryCI, &m_Factory);

    m_Stats.FactoryReady = m_Factory != nullptr;

    if (!m_Factory)
    {
        m_Stats.DisabledReason = kSRProviderUnavailableReason;
        return true;
    }

    Uint32 NumVariants = 0;
    m_Factory->EnumerateVariants(NumVariants, nullptr);
    m_Variants.resize(NumVariants);
    if (NumVariants > 0)
    {
        m_Factory->EnumerateVariants(NumVariants, m_Variants.data());
        m_Variants.resize(NumVariants);
    }

    m_Stats.VariantCount = static_cast<Uint32>(m_Variants.size());
    if (m_Variants.empty())
    {
        m_Stats.DisabledReason = kSRVariantsUnavailableReason;
        return true;
    }

    const bool HasTemporalVariant = ContainsTemporalVariant(m_Variants);
    if (HasTemporalVariant && !CreateMotionConversionPipeline(pDevice, pStateCache))
        return true;

    m_Stats.DisabledReason = HasTemporalVariant ? "" : "temporal super resolution variant unavailable";
    return true;
}

const SuperResolutionInfo* RTXPTSuperResolutionPass::GetActiveVariant(const RTXPTSuperResolutionSettings& Settings) const
{
    if (m_Variants.empty())
        return nullptr;

    const Int32 LastVariantIdx   = static_cast<Int32>(m_Variants.size() - 1);
    const Int32 ActiveVariantIdx = std::min(std::max(Settings.ActiveVariantIdx, 0), LastVariantIdx);
    return &m_Variants[static_cast<size_t>(ActiveVariantIdx)];
}

bool RTXPTSuperResolutionPass::HasTemporalVariant() const
{
    return m_MotionConversionPSO && m_MotionConversionSRB && ContainsTemporalVariant(m_Variants);
}

bool RTXPTSuperResolutionPass::SupportsSharpness(const SuperResolutionInfo& Info) const
{
    return (Info.Type == SUPER_RESOLUTION_TYPE_SPATIAL && (Info.SpatialCapFlags & SUPER_RESOLUTION_SPATIAL_CAP_FLAG_SHARPNESS) != 0) ||
        (Info.Type == SUPER_RESOLUTION_TYPE_TEMPORAL && (Info.TemporalCapFlags & SUPER_RESOLUTION_TEMPORAL_CAP_FLAG_SHARPNESS) != 0);
}

SUPER_RESOLUTION_FLAGS RTXPTSuperResolutionPass::GetFlags(const SuperResolutionInfo& Variant, float Sharpness) const
{
    SUPER_RESOLUTION_FLAGS Flags =
        Variant.Type == SUPER_RESOLUTION_TYPE_TEMPORAL ? SUPER_RESOLUTION_FLAG_AUTO_EXPOSURE : SUPER_RESOLUTION_FLAG_NONE;

    if (SupportsSharpness(Variant) && Sharpness > 0.0f)
        Flags = Flags | SUPER_RESOLUTION_FLAG_ENABLE_SHARPENING;

    return Flags;
}

RTXPTSuperResolutionFrameDesc RTXPTSuperResolutionPass::ResolveFrameDesc(const RTXPTSuperResolutionSettings& Settings,
                                                                         Uint32                              DisplayWidth,
                                                                         Uint32                              DisplayHeight,
                                                                         TEXTURE_FORMAT                      OutputFormat,
                                                                         bool                                ResetHistory,
                                                                         float                               TimeDeltaSeconds)
{
    m_Stats.LastExecute   = false;
    m_Stats.UpscalerReady = false;

    auto DirectFrameDesc = MakeDirectFrameDesc(DisplayWidth, DisplayHeight, OutputFormat, Settings, ResetHistory, TimeDeltaSeconds);
    m_Stats.DisabledReason.clear();
    UpdateFrameStats(m_Stats, DirectFrameDesc);

    if (!Settings.Enabled)
    {
        m_Stats.DisabledReason = "super resolution disabled by settings";
        return DirectFrameDesc;
    }
    if (!m_Factory)
    {
        m_Stats.DisabledReason = kSRProviderUnavailableReason;
        return DirectFrameDesc;
    }
    if (m_Variants.empty())
    {
        m_Stats.DisabledReason = kSRVariantsUnavailableReason;
        return DirectFrameDesc;
    }

    const SuperResolutionInfo* pVariant = GetActiveVariant(Settings);
    if (pVariant == nullptr)
    {
        DEV_ERROR("RTXPT super resolution active variant is invalid");
        return DirectFrameDesc;
    }
    if (pVariant->Type != SUPER_RESOLUTION_TYPE_TEMPORAL)
    {
        DEV_ERROR("RTXPT P6 HDR path requires temporal super resolution variant");
        return DirectFrameDesc;
    }
    if (!m_MotionConversionPSO || !m_MotionConversionSRB)
    {
        DEV_ERROR("RTXPT super resolution motion conversion pipeline is unavailable");
        return DirectFrameDesc;
    }

    SuperResolutionSourceSettingsAttribs SRAttribs;
    SRAttribs.VariantId        = pVariant->VariantId;
    SRAttribs.OutputWidth      = DirectFrameDesc.Dimensions.DisplayWidth;
    SRAttribs.OutputHeight     = DirectFrameDesc.Dimensions.DisplayHeight;
    SRAttribs.OutputFormat     = DirectFrameDesc.OutputFormat;
    SRAttribs.Flags            = GetFlags(*pVariant, Settings.Sharpness);
    SRAttribs.OptimizationType = Settings.OptimizationType;

    SuperResolutionSourceSettings SRSettings;
    m_Factory->GetSourceSettings(SRAttribs, SRSettings);
    if (SRSettings.OptimalInputWidth == 0 || SRSettings.OptimalInputHeight == 0)
    {
        DEV_ERROR("RTXPT super resolution source settings are invalid");
        return DirectFrameDesc;
    }

    RTXPTSuperResolutionFrameDesc FrameDesc    = DirectFrameDesc;
    FrameDesc.Dimensions.RenderWidth           = SRSettings.OptimalInputWidth;
    FrameDesc.Dimensions.RenderHeight          = SRSettings.OptimalInputHeight;
    FrameDesc.Dimensions.SuperResolutionActive = true;
    FrameDesc.Enabled                          = true;
    FrameDesc.Temporal                         = true;
    FrameDesc.Type                             = pVariant->Type;
    FrameDesc.VariantId                        = pVariant->VariantId;
    if (!FrameDesc.Dimensions.IsValid())
    {
        DEV_ERROR("RTXPT super resolution dimensions are invalid");
        return DirectFrameDesc;
    }

    if (!EnsureUpscaler(FrameDesc))
    {
        DEV_ERROR("RTXPT super resolution failed to prepare upscaler");
        return DirectFrameDesc;
    }

    m_Upscaler->GetJitterOffset(m_Stats.ExecuteCount, FrameDesc.Jitter.x, FrameDesc.Jitter.y);
    m_Stats.DisabledReason.clear();
    UpdateFrameStats(m_Stats, FrameDesc);
    return FrameDesc;
}

bool RTXPTSuperResolutionPass::EnsureUpscaler(const RTXPTSuperResolutionFrameDesc& FrameDesc)
{
    if (!FrameDesc.Enabled || !FrameDesc.Temporal)
        return true;

    const auto VariantIt = std::find_if(m_Variants.begin(), m_Variants.end(), [&FrameDesc](const SuperResolutionInfo& Info) {
        return Info.VariantId == FrameDesc.VariantId;
    });
    if (!m_Factory)
    {
        m_Stats.UpscalerReady  = false;
        m_Stats.DisabledReason = kSRProviderUnavailableReason;
        return false;
    }
    if (!m_Device)
    {
        m_Stats.UpscalerReady = false;
        DEV_ERROR("RTXPT super resolution pass requires a render device");
        return false;
    }
    if (VariantIt == m_Variants.end())
    {
        m_Stats.UpscalerReady = false;
        DEV_ERROR("RTXPT super resolution variant is unavailable");
        return false;
    }

    SuperResolutionDesc Desc;
    Desc.Name         = "RTXPT super resolution upscaler";
    Desc.VariantId    = FrameDesc.VariantId;
    Desc.InputWidth   = FrameDesc.Dimensions.RenderWidth;
    Desc.InputHeight  = FrameDesc.Dimensions.RenderHeight;
    Desc.OutputWidth  = FrameDesc.Dimensions.DisplayWidth;
    Desc.OutputHeight = FrameDesc.Dimensions.DisplayHeight;
    Desc.OutputFormat = FrameDesc.OutputFormat;
    Desc.ColorFormat  = FrameDesc.ColorFormat;
    Desc.DepthFormat  = FrameDesc.DepthFormat;
    Desc.MotionFormat = FrameDesc.MotionFormat;
    Desc.Flags        = GetFlags(*VariantIt, FrameDesc.Sharpness);

    if (m_Upscaler && UpscalerDescMatches(m_UpscalerDesc, Desc))
    {
        m_Stats.UpscalerReady = true;
        return true;
    }

    m_Upscaler.Release();
    m_Factory->CreateSuperResolution(Desc, &m_Upscaler);
    m_Stats.UpscalerReady = m_Upscaler != nullptr;
    if (!m_Upscaler)
    {
        DEV_ERROR("Failed to create RTXPT super resolution upscaler");
        return false;
    }

    m_UpscalerDesc = Desc;
    return true;
}

bool RTXPTSuperResolutionPass::CreateMotionConversionPipeline(IRenderDevice* pDevice, IRenderStateCache* pStateCache)
{
    if (pDevice->GetDeviceInfo().Features.ComputeShaders != DEVICE_FEATURE_STATE_ENABLED)
    {
        DEV_ERROR("RTXPT super resolution motion conversion requires compute shader support");
        return false;
    }

    if (!SupportsBindFlags(pDevice, kSRMotionFormat, BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS))
    {
        DEV_ERROR("RG16F SRV/UAV texture is not supported for RTXPT super resolution motion vectors");
        return false;
    }

    IEngineFactory* pEngineFactory = pDevice->GetEngineFactory();
    if (pEngineFactory == nullptr)
    {
        DEV_ERROR("RTXPT super resolution motion conversion requires an engine factory");
        return false;
    }

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    pEngineFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders\\PostProcessing", &pShaderSourceFactory);
    if (!pShaderSourceFactory)
    {
        DEV_ERROR("Failed to create RTXPT super resolution motion shader source factory");
        return false;
    }

    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.Desc.Name                  = "RTXPT super resolution motion conversion";
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = "PostProcessing/RTXPTSuperResolutionMotion.csh";
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    RefCntAutoPtr<IShader> pCS;
    pStateCache->CreateShader(ShaderCI, &pCS);
    VERIFY(pCS, "Failed to create RTXPT super resolution motion conversion shader");
    if (!pCS)
    {
        DEV_ERROR("Failed to create RTXPT super resolution motion conversion shader");
        return false;
    }

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = "RTXPT super resolution motion conversion PSO";
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.pCS                  = pCS;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout
        .AddVariable(SHADER_TYPE_COMPUTE, "t_RTXPTMotionVectors", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_SRMotionVectors", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    pStateCache->CreateComputePipelineState(PSOCreateInfo, &m_MotionConversionPSO);
    VERIFY(m_MotionConversionPSO, "Failed to create RTXPT super resolution motion conversion PSO");
    if (!m_MotionConversionPSO)
    {
        DEV_ERROR("Failed to create RTXPT super resolution motion conversion PSO");
        return false;
    }

    m_MotionConversionPSO->CreateShaderResourceBinding(&m_MotionConversionSRB, true);
    VERIFY(m_MotionConversionSRB, "Failed to create RTXPT super resolution motion conversion SRB");
    if (!m_MotionConversionSRB)
    {
        DEV_ERROR("Failed to create RTXPT super resolution motion conversion SRB");
        return false;
    }

    return true;
}

bool RTXPTSuperResolutionPass::EnsureMotionConversionResources(IRenderDevice* pDevice, const RTXPTRenderTargets& RenderTargets)
{
    const Uint32 Width  = RenderTargets.GetRenderWidth();
    const Uint32 Height = RenderTargets.GetRenderHeight();

    if (Width == 0 || Height == 0)
    {
        DEV_ERROR("RTXPT super resolution motion conversion dimensions are invalid");
        return false;
    }

    if (m_SRMotionVectors && m_SRMotionWidth == Width && m_SRMotionHeight == Height)
        return true;

    m_SRMotionVectors.Release();
    m_SRMotionWidth  = 0;
    m_SRMotionHeight = 0;

    TextureDesc MotionDesc;
    MotionDesc.Name      = "RTXPT SuperResolution motion vectors";
    MotionDesc.Type      = RESOURCE_DIM_TEX_2D;
    MotionDesc.Width     = Width;
    MotionDesc.Height    = Height;
    MotionDesc.Format    = kSRMotionFormat;
    MotionDesc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    pDevice->CreateTexture(MotionDesc, nullptr, &m_SRMotionVectors);
    if (!GetSRMotionSRV() || !GetSRMotionUAV())
    {
        m_SRMotionVectors.Release();
        DEV_ERROR("Failed to create RTXPT super resolution motion texture");
        return false;
    }

    m_SRMotionWidth  = Width;
    m_SRMotionHeight = Height;
    return true;
}

ITextureView* RTXPTSuperResolutionPass::GetSRMotionSRV() const
{
    return GetDefaultView(m_SRMotionVectors, TEXTURE_VIEW_SHADER_RESOURCE);
}

ITextureView* RTXPTSuperResolutionPass::GetSRMotionUAV() const
{
    return GetDefaultView(m_SRMotionVectors, TEXTURE_VIEW_UNORDERED_ACCESS);
}

bool RTXPTSuperResolutionPass::ConvertMotionVectors(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets)
{
    if (!EnsureMotionConversionResources(m_Device, RenderTargets))
        return false;

    const bool Bound =
        SetDynamicVariable(m_MotionConversionSRB, "t_RTXPTMotionVectors", RenderTargets.GetScreenMotionVectorsSRV()) &&
        SetDynamicVariable(m_MotionConversionSRB, "u_SRMotionVectors", GetSRMotionUAV());
    if (!Bound)
    {
        DEV_ERROR("RTXPT super resolution motion conversion failed to bind resources");
        return false;
    }

    pContext->SetPipelineState(m_MotionConversionPSO);
    pContext->CommitShaderResources(m_MotionConversionSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->DispatchCompute(DispatchComputeAttribs{(m_SRMotionWidth + kSRMotionThreadGroupSize - 1u) / kSRMotionThreadGroupSize,
                                                     (m_SRMotionHeight + kSRMotionThreadGroupSize - 1u) / kSRMotionThreadGroupSize,
                                                     1u});
    return true;
}

bool RTXPTSuperResolutionPass::Execute(IDeviceContext*                      pContext,
                                       const RTXPTRenderTargets&            RenderTargets,
                                       const RTXPTSuperResolutionFrameDesc& FrameDesc,
                                       float                                CameraNear,
                                       float                                CameraFar,
                                       float                                CameraFovAngleVert,
                                       ITextureView*                        pColorSRV,
                                       ITextureView*                        pOutputUAV)
{
    m_Stats.LastExecute = false;
    if (!FrameDesc.Enabled)
        return true;

    if (!EnsureUpscaler(FrameDesc))
    {
        DEV_ERROR("RTXPT super resolution pass failed to prepare upscaler");
        return false;
    }

    ExecuteSuperResolutionAttribs Attribs;
    Attribs.pContext            = pContext;
    Attribs.pColorTextureSRV    = pColorSRV != nullptr ? pColorSRV : RenderTargets.GetSuperResolutionColorSRV();
    Attribs.pDepthTextureSRV    = RenderTargets.GetDepthSRV();
    Attribs.pMotionVectorsSRV   = RenderTargets.GetScreenMotionVectorsSRV();
    Attribs.pOutputTextureView  = pOutputUAV != nullptr ? pOutputUAV : RenderTargets.GetSuperResolutionOutputUAV();
    Attribs.StateTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    Attribs.JitterX             = FrameDesc.Jitter.x;
    Attribs.JitterY             = FrameDesc.Jitter.y;
    Attribs.PreExposure         = 1.0f;
    Attribs.MotionVectorScaleX  = 1.0f;
    Attribs.MotionVectorScaleY  = 1.0f;
    Attribs.ExposureScale       = 1.0f;
    Attribs.Sharpness           = FrameDesc.Sharpness;
    Attribs.CameraNear          = CameraNear;
    Attribs.CameraFar           = CameraFar;
    Attribs.CameraFovAngleVert  = CameraFovAngleVert;
    Attribs.TimeDeltaInSeconds  = FrameDesc.TimeDeltaSeconds;
    Attribs.ResetHistory        = FrameDesc.ResetHistory;

    const auto FailExecute = [](const char* Reason) {
        DEV_ERROR("RTXPT super resolution pass failed: ", Reason);
        return false;
    };
    if (Attribs.pContext == nullptr)
        return FailExecute("device context is null");
    if (!RenderTargets.IsSuperResolutionActive())
        return FailExecute("super resolution render targets are inactive");
    if (!(RenderTargets.GetDimensions() == FrameDesc.Dimensions))
        return FailExecute("super resolution render target dimensions do not match frame desc");
    if (Attribs.pColorTextureSRV == nullptr)
        return FailExecute("super resolution color SRV is null");
    if (Attribs.pDepthTextureSRV == nullptr)
        return FailExecute("super resolution depth SRV is null");
    if (RenderTargets.GetScreenMotionVectorsSRV() == nullptr)
        return FailExecute("super resolution motion vectors SRV is null");
    if (Attribs.pOutputTextureView == nullptr)
        return FailExecute("super resolution output UAV is null");
    if (m_Upscaler == nullptr)
        return FailExecute("super resolution upscaler is null");
    if (!ConvertMotionVectors(pContext, RenderTargets))
        return false;

    Attribs.pMotionVectorsSRV = GetSRMotionSRV();
    if (Attribs.pMotionVectorsSRV == nullptr)
        return FailExecute("super resolution converted motion vectors SRV is null");

    m_Upscaler->Execute(Attribs);
    m_Stats.DisabledReason.clear();
    m_Stats.LastExecute = true;
    ++m_Stats.ExecuteCount;
    return true;
}
} // namespace Diligent
