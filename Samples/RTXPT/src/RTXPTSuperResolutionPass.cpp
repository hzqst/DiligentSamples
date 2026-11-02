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
constexpr TEXTURE_FORMAT kSRMotionFormat              = RTXPTSuperResolutionInputPass::MotionVectorsFormat;

bool ContainsTemporalVariant(const std::vector<SuperResolutionInfo>& Variants)
{
    return std::any_of(Variants.begin(), Variants.end(), [](const SuperResolutionInfo& Info) {
        return Info.Type == SUPER_RESOLUTION_TYPE_TEMPORAL;
    });
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
    FrameDesc.DepthFormat                      = RTXPTSuperResolutionInputPass::DepthFormat;
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
    m_Inputs.Reset();
    m_InputsReady = false;
    m_Factory.Release();
    m_Device.Release();
    m_Variants.clear();
    m_Stats        = {};
    m_UpscalerDesc = {};
}

bool RTXPTSuperResolutionPass::Initialize(IRenderDevice* pDevice, IRenderStateCache* pStateCache, IBuffer* pFrameConstants)
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
    if (HasTemporalVariant)
    {
        m_InputsReady = m_Inputs.Initialize(pDevice, pDevice->GetEngineFactory(), pStateCache, pFrameConstants);
        if (!m_InputsReady)
        {
            DEV_ERROR("RTXPT super resolution input preparation is unavailable");
            return true;
        }
    }

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
    return m_InputsReady && ContainsTemporalVariant(m_Variants);
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
    if (!m_InputsReady)
    {
        DEV_ERROR("RTXPT super resolution input preparation is unavailable");
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
    if (!m_Factory->GetSourceSettings(SRAttribs, SRSettings))
    {
        // The provider never substitutes another quality mode; report the unavailable selection.
        m_Stats.DisabledReason = "super resolution quality mode unavailable for the output resolution";
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

//The depth reconstructs the guides' camera rays with the uploaded constants, so it is exact for guides traced with
//sampleBaseIndex (one sample per pixel, without sub-sampling); other configurations get the ray of that sample.
bool RTXPTSuperResolutionPass::PrepareInputs(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets)
{
    const Uint32 Width  = RenderTargets.GetRenderWidth();
    const Uint32 Height = RenderTargets.GetRenderHeight();
    if ((m_Inputs.GetWidth() != Width || m_Inputs.GetHeight() != Height) && !m_Inputs.Resize(m_Device, Width, Height))
    {
        DEV_ERROR("Failed to create RTXPT super resolution inputs");
        return false;
    }
    if (!m_Inputs.Prepare(pContext, RenderTargets.GetDepthSRV(), RenderTargets.GetScreenMotionVectorsSRV()))
    {
        DEV_ERROR("RTXPT super resolution input preparation failed");
        return false;
    }
    return true;
}

bool RTXPTSuperResolutionPass::Execute(IDeviceContext* pContext, const RTXPTSuperResolutionExecuteAttribs& ExecuteAttribs)
{
    m_Stats.LastExecute = false;
    if (ExecuteAttribs.pRenderTargets == nullptr || ExecuteAttribs.pFrameDesc == nullptr)
    {
        DEV_ERROR("RTXPT super resolution pass requires render targets and a frame description");
        return false;
    }
    const RTXPTRenderTargets&            RenderTargets = *ExecuteAttribs.pRenderTargets;
    const RTXPTSuperResolutionFrameDesc& FrameDesc     = *ExecuteAttribs.pFrameDesc;
    if (!FrameDesc.Enabled)
        return true;

    if (!EnsureUpscaler(FrameDesc))
    {
        DEV_ERROR("RTXPT super resolution pass failed to prepare upscaler");
        return false;
    }

    ExecuteSuperResolutionAttribs Attribs;
    Attribs.pContext            = pContext;
    Attribs.pColorTextureSRV    = ExecuteAttribs.pColorSRV != nullptr ? ExecuteAttribs.pColorSRV : RenderTargets.GetSuperResolutionColorSRV();
    Attribs.pOutputTextureView  = ExecuteAttribs.pOutputUAV != nullptr ? ExecuteAttribs.pOutputUAV : RenderTargets.GetSuperResolutionOutputUAV();
    Attribs.StateTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    Attribs.JitterX             = FrameDesc.Jitter.x;
    Attribs.JitterY             = FrameDesc.Jitter.y;
    Attribs.PreExposure         = 1.0f;
    Attribs.MotionVectorScaleX  = 1.0f;
    Attribs.MotionVectorScaleY  = 1.0f;
    Attribs.ExposureScale       = 1.0f;
    Attribs.Sharpness           = FrameDesc.Sharpness;
    Attribs.CameraNear          = ExecuteAttribs.CameraNear;
    Attribs.CameraFar           = ExecuteAttribs.CameraFar;
    Attribs.CameraFovAngleVert  = ExecuteAttribs.CameraFovAngleVert;
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
    if (RenderTargets.GetDepthSRV() == nullptr)
        return FailExecute("super resolution ray distance SRV is null");
    if (RenderTargets.GetScreenMotionVectorsSRV() == nullptr)
        return FailExecute("super resolution motion vectors SRV is null");
    if (Attribs.pOutputTextureView == nullptr)
        return FailExecute("super resolution output UAV is null");
    if (m_Upscaler == nullptr)
        return FailExecute("super resolution upscaler is null");
    if (!PrepareInputs(pContext, RenderTargets))
        return false;

    // Projected depth, not the ray distance of the guides, which DLSS would misread as depth.
    Attribs.pDepthTextureSRV             = m_Inputs.GetDepthSRV();
    Attribs.pMotionVectorsSRV            = m_Inputs.GetMotionVectorsSRV();
    Attribs.pIgnoreHistoryMaskTextureSRV = m_Inputs.GetIgnoreHistoryMaskSRV();

    if (!m_Upscaler->Execute(Attribs))
        return FailExecute("super resolution upscaler execute failed");

    m_Stats.DisabledReason.clear();
    m_Stats.LastExecute = true;
    ++m_Stats.ExecuteCount;
    return true;
}
} // namespace Diligent
