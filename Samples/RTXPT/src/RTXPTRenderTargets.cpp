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

#include "RTXPTRenderTargets.hpp"
#include "DebugUtilities.hpp"
#include "GraphicsAccessories.hpp"

#include <algorithm>

#include "AsyncPrefix.hpp"

namespace Diligent
{

namespace
{

bool FormatsMatch(const RTXPTRenderTargetFormats& Lhs, const RTXPTRenderTargetFormats& Rhs)
{
    return Lhs.OutputColor == Rhs.OutputColor &&
        Lhs.AccumulatedRadiance == Rhs.AccumulatedRadiance &&
        Lhs.SuperResolutionInputColor == Rhs.SuperResolutionInputColor &&
        Lhs.ProcessedOutputColor == Rhs.ProcessedOutputColor &&
        Lhs.LdrColor == Rhs.LdrColor &&
        Lhs.ComputeColor == Rhs.ComputeColor &&
        Lhs.Depth == Rhs.Depth &&
        Lhs.PreviousDepth == Rhs.PreviousDepth &&
        Lhs.ScreenMotionVectors == Rhs.ScreenMotionVectors &&
        Lhs.TemporalFeedback == Rhs.TemporalFeedback &&
        Lhs.CombinedHistoryClampRelax == Rhs.CombinedHistoryClampRelax &&
        Lhs.StableRadiance == Rhs.StableRadiance &&
        Lhs.DenoisingLayersHeader == Rhs.DenoisingLayersHeader &&
        Lhs.Throughput == Rhs.Throughput &&
        Lhs.SpecularHitT == Rhs.SpecularHitT &&
        Lhs.ScratchFloat1 == Rhs.ScratchFloat1 &&
        Lhs.DenoiserViewspaceZ == Rhs.DenoiserViewspaceZ &&
        Lhs.DenoiserMotionVectors == Rhs.DenoiserMotionVectors &&
        Lhs.DenoiserNormalRoughness == Rhs.DenoiserNormalRoughness &&
        Lhs.DenoiserDiffRadianceHitDist == Rhs.DenoiserDiffRadianceHitDist &&
        Lhs.DenoiserSpecRadianceHitDist == Rhs.DenoiserSpecRadianceHitDist &&
        Lhs.DenoiserDisocclusionThresholdMix == Rhs.DenoiserDisocclusionThresholdMix &&
        Lhs.DenoiserOutDiffRadianceHitDist == Rhs.DenoiserOutDiffRadianceHitDist &&
        Lhs.DenoiserOutSpecRadianceHitDist == Rhs.DenoiserOutSpecRadianceHitDist &&
        Lhs.DenoiserOutValidation == Rhs.DenoiserOutValidation &&
        Lhs.DenoiserAvgLayerRadianceHalfRes == Rhs.DenoiserAvgLayerRadianceHalfRes;
}

// Array slices of DenoisingLayersHeader, one branch ID plane each (see DenoisingLayers.hlsli).
constexpr Uint32 kDenoisingLayersHeaderPlaneCount = 4;

// A single-mip, single-sample 2D target; callers adjust the type and array size of texture arrays.
TextureDesc MakeTargetDesc(const char* Name, Uint32 Width, Uint32 Height, TEXTURE_FORMAT Format, BIND_FLAGS BindFlags)
{
    TextureDesc Desc;
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    Desc.Name                 = Name;
    Desc.Type                 = RESOURCE_DIM_TEX_2D;
    Desc.Width                = Width;
    Desc.Height               = Height;
    Desc.MipLevels            = 1;
    Desc.SampleCount          = 1;
    Desc.Format               = Format;
    Desc.BindFlags            = BindFlags;
    return Desc;
}

Uint64 ComputeDenoisingLayersElementCount(Uint32 ImageWidth, Uint32 ImageHeight)
{
    return ComputeRTXPTLayerPlaneStride(ImageWidth, ImageHeight) * kRTXPTDenoisingLayerCount;
}

} // namespace

void RTXPTRenderTargets::Reset()
{
    m_OutputColor.Release();
    m_AccumulatedRadiance.Release();
    m_SuperResolutionInputColor.Release();
    m_ProcessedOutputColor.Release();
    m_LdrColor.Release();
    m_ComputeColor.Release();
    m_Depth.Release();
    m_PreviousDepth.Release();
    m_ScreenMotionVectors.Release();
    m_SpecularMotion.Release();
    m_PreviousAvgLayerRadiance.Release();
    m_TemporalFeedback1.Release();
    m_TemporalFeedback2.Release();
    m_CombinedHistoryClampRelax.Release();
    m_StableRadiance.Release();
    m_DenoisingLayersHeader.Release();
    m_DenoisingLayersBuffer.Release();
    m_Throughput.Release();
    m_SpecularHitT.Release();
    m_ScratchFloat1.Release();
    m_DenoiserViewspaceZ.Release();
    m_DenoiserMotionVectors.Release();
    m_DenoiserNormalRoughness.Release();
    m_DenoiserDiffRadianceHitDist.Release();
    m_DenoiserSpecRadianceHitDist.Release();
    m_DenoiserDisocclusionThresholdMix.Release();
    for (auto& Texture : m_DenoiserOutDiffRadianceHitDist)
        Texture.Release();
    for (auto& Texture : m_DenoiserOutSpecRadianceHitDist)
        Texture.Release();
    m_DenoiserOutValidation.Release();
    m_DenoiserAvgLayerRadianceHalfRes.Release();
    m_RRDiffuseAlbedo.Release();
    m_RRSpecAlbedo.Release();
    m_RRNormalsAndRoughness.Release();
    m_RRDepth.Release();
    m_RRMotionVectors.Release();
    m_RRSpecMotionVectors.Release();
    m_RayReconstructionResourcesRequested = false;
    m_AccumulatedRadianceUnavailable      = false;
    m_AccumulatedRadianceRequested        = false;
    m_RealtimeResourcesRequested          = false;
    m_DenoiserValidationRequested         = false;
    m_PendingResizeRequestValid           = false;
    m_PendingRealtimeResources            = false;
    m_PendingDenoiserValidation           = false;
    m_DenoisingLayersElementCount         = 0;
    m_LastFailureReason.clear();
    m_Dimensions = {0, 0, 0, 0, false};
    m_Formats    = {};
}

void RTXPTCreateZeroedTexture2D(IRenderDevice* pDevice, const TextureDesc& Desc, std::vector<Uint8>& Zeros, ITexture** ppTexture)
{
    VERIFY(Desc.Type == RESOURCE_DIM_TEX_2D && Desc.MipLevels == 1 && Desc.SampleCount == 1, "Only single-mip, single-sample 2D textures are supported");
    VERIFY(GetTextureFormatAttribs(Desc.Format).ComponentType != COMPONENT_TYPE_COMPRESSED, "Compressed formats are not supported");

    const Uint64 RowBytes = Uint64{Desc.Width} * GetTextureFormatAttribs(Desc.Format).GetElementSize();
    const size_t Bytes    = static_cast<size_t>(RowBytes * Desc.Height);
    if (Zeros.size() < Bytes)
        Zeros.resize(Bytes, 0);

    TextureSubResData ZeroSubresource{Zeros.data(), RowBytes};
    TextureData       InitData{&ZeroSubresource, 1};
    pDevice->CreateTexture(Desc, &InitData, ppTexture);
}

bool RTXPTRenderTargets::CreateTarget(IRenderDevice* pDevice, const TextureDesc& Desc, RefCntAutoPtr<ITexture>& Target)
{
    Target.Release();
    pDevice->CreateTexture(Desc, nullptr, &Target);
    return ValidateTargetViews(Desc, Target);
}

bool RTXPTRenderTargets::CreateZeroedTarget(IRenderDevice* pDevice, const TextureDesc& Desc, std::vector<Uint8>& Zeros, RefCntAutoPtr<ITexture>& Target)
{
    Target.Release();
    RTXPTCreateZeroedTexture2D(pDevice, Desc, Zeros, &Target);
    return ValidateTargetViews(Desc, Target);
}

bool RTXPTRenderTargets::ValidateTargetViews(const TextureDesc& Desc, RefCntAutoPtr<ITexture>& Target)
{
    if (!Target)
    {
        LOG_ERROR_MESSAGE("Failed to create ", Desc.Name);
        return false;
    }

    if (((Desc.BindFlags & BIND_SHADER_RESOURCE) != 0 && Target->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) == nullptr) ||
        ((Desc.BindFlags & BIND_UNORDERED_ACCESS) != 0 && Target->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) == nullptr) ||
        ((Desc.BindFlags & BIND_RENDER_TARGET) != 0 && Target->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET) == nullptr))
    {
        LOG_ERROR_MESSAGE(Desc.Name, " is missing one or more required default views");
        Target.Release();
        return false;
    }

    return true;
}

bool RTXPTRenderTargets::CreateDenoisingLayersBuffer(IRenderDevice*          pDevice,
                                                     Uint64                  ElementCount,
                                                     RefCntAutoPtr<IBuffer>& Target)
{
    BufferDesc Desc;
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    Desc.Name                 = "RTXPT DenoisingLayersBuffer";
    Desc.Usage                = USAGE_DEFAULT;
    Desc.BindFlags            = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    Desc.Mode                 = BUFFER_MODE_STRUCTURED;
    Desc.ElementByteStride    = sizeof(RTXPTDenoisingLayerData);
    Desc.Size                 = std::max<Uint64>(ElementCount, Uint64{1}) * sizeof(RTXPTDenoisingLayerData);

    Target.Release();
    pDevice->CreateBuffer(Desc, nullptr, &Target);
    if (!Target)
    {
        LOG_ERROR_MESSAGE("Failed to create RTXPT DenoisingLayersBuffer");
        return false;
    }

    if (Target->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE) == nullptr ||
        Target->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS) == nullptr)
    {
        LOG_ERROR_MESSAGE("RTXPT DenoisingLayersBuffer is missing one or more required default views");
        Target.Release();
        return false;
    }

    return true;
}

bool RTXPTRenderTargets::SupportsBindFlags(IRenderDevice* pDevice, TEXTURE_FORMAT TargetFormat, BIND_FLAGS BindFlags) const
{
    const auto& FmtInfo = pDevice->GetTextureFormatInfoExt(TargetFormat);
    return (FmtInfo.BindFlags & BindFlags) == BindFlags;
}

bool RTXPTRenderTargets::FailResize(const char* Reason)
{
    const bool  RealtimeResourcesRequested  = m_PendingResizeRequestValid ? m_PendingRealtimeResources : m_RealtimeResourcesRequested;
    const bool  DenoiserValidationRequested = m_PendingResizeRequestValid ? m_PendingDenoiserValidation : m_DenoiserValidationRequested;
    std::string Failure                     = Reason != nullptr ? Reason : "RTXPT render target resize failed";
    Reset();
    m_RealtimeResourcesRequested  = RealtimeResourcesRequested;
    m_DenoiserValidationRequested = DenoiserValidationRequested;
    m_LastFailureReason           = Failure;
    LOG_ERROR_MESSAGE(m_LastFailureReason.c_str());
    return false;
}

bool RTXPTRenderTargets::Resize(IRenderDevice* pDevice, const RTXPTRenderTargetCreateInfo& CreateInfo)
{
    const RTXPTRenderTargetDimensions& Dimensions                       = CreateInfo.Dimensions;
    const RTXPTRenderTargetFormats&    Formats                          = CreateInfo.Formats;
    const bool                         CreateComputeOutput              = CreateInfo.CreateComputeOutput;
    const bool                         CreateAccumulatedRadiance        = CreateInfo.CreateAccumulatedRadiance;
    const bool                         CreateRealtimeResources          = CreateInfo.CreateRealtimeResources;
    const bool                         CreateDenoiserValidation         = CreateInfo.CreateDenoiserValidation;
    const bool                         CreateRayReconstructionResources = CreateInfo.CreateRayReconstructionResources;

    m_PendingResizeRequestValid = true;
    m_PendingRealtimeResources  = CreateRealtimeResources;
    m_PendingDenoiserValidation = CreateDenoiserValidation;

    if (pDevice == nullptr)
        return FailResize("RTXPT render targets require a render device");
    if (!Dimensions.IsValid())
        return FailResize("RTXPT render target dimensions are invalid");
    if (CreateRayReconstructionResources &&
        (!CreateRealtimeResources || CreateAccumulatedRadiance || CreateDenoiserValidation ||
         Formats.OutputColor != TEX_FORMAT_RGBA16_FLOAT ||
         Formats.ProcessedOutputColor != TEX_FORMAT_RGBA16_FLOAT ||
         Formats.Depth != TEX_FORMAT_R32_FLOAT ||
         Formats.ScreenMotionVectors != TEX_FORMAT_RGBA16_FLOAT))
        return FailResize("RTXPT RR requires realtime resources, unaccumulated RGBA16F color/output, R32F ray distance and RGBA16F motion");

    const bool RequiresSuperResolutionTargets = Dimensions.SuperResolutionActive;
    const bool RequiresHistoryClampRelax      = RequiresSuperResolutionTargets || CreateRealtimeResources;
    const bool HasCorePostProcessTargets =
        m_OutputColor != nullptr &&
        m_ProcessedOutputColor != nullptr &&
        m_LdrColor != nullptr &&
        m_Depth != nullptr &&
        (!CreateRealtimeResources || m_PreviousDepth != nullptr) &&
        m_ScreenMotionVectors != nullptr &&
        (!RequiresSuperResolutionTargets || m_SuperResolutionInputColor != nullptr);
    const bool HasP6PostProcessTargets =
        HasCorePostProcessTargets &&
        (!RequiresSuperResolutionTargets ||
         (m_TemporalFeedback1 != nullptr &&
          m_TemporalFeedback2 != nullptr)) &&
        (!RequiresHistoryClampRelax || m_CombinedHistoryClampRelax != nullptr);
    const bool HasRealtimeTargets =
        !CreateRealtimeResources || HasRealtimeRenderTargets();
    const bool HasRequestedTargets =
        HasP6PostProcessTargets &&
        (!CreateComputeOutput || m_ComputeColor != nullptr) &&
        (CreateComputeOutput || m_ComputeColor == nullptr) &&
        m_AccumulatedRadianceRequested == CreateAccumulatedRadiance &&
        m_RealtimeResourcesRequested == CreateRealtimeResources &&
        m_DenoiserValidationRequested == CreateDenoiserValidation &&
        m_RayReconstructionResourcesRequested == CreateRayReconstructionResources &&
        (!CreateRayReconstructionResources || HasRayReconstructionRenderTargets()) &&
        HasRealtimeTargets &&
        (!CreateAccumulatedRadiance || m_AccumulatedRadiance != nullptr || m_AccumulatedRadianceUnavailable) &&
        (CreateAccumulatedRadiance || m_AccumulatedRadiance == nullptr);

    if (HasRequestedTargets && m_Dimensions == Dimensions && FormatsMatch(m_Formats, Formats))
    {
        m_PendingResizeRequestValid = false;
        return true;
    }

    const BIND_FLAGS UavFlags     = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    const BIND_FLAGS HdrRtFlags   = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS | BIND_RENDER_TARGET;
    const BIND_FLAGS LdrRtFlags   = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS | BIND_RENDER_TARGET;
    const BIND_FLAGS DepthRtFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;

    if (CreateRayReconstructionResources &&
        (!SupportsBindFlags(pDevice, TEX_FORMAT_RGBA16_FLOAT, UavFlags) ||
         !SupportsBindFlags(pDevice, TEX_FORMAT_R32_FLOAT, UavFlags) ||
         !SupportsBindFlags(pDevice, TEX_FORMAT_RG16_FLOAT, UavFlags)))
        return FailResize("RTXPT RR requires RGBA16F, R32F and RG16F SRV/UAV support");

    if (!SupportsBindFlags(pDevice, Formats.OutputColor, UavFlags))
        return FailResize("RGBA16F UAV OutputColor is not supported; RTXPT post-processing resource graph is unavailable");

    if (CreateAccumulatedRadiance && !SupportsBindFlags(pDevice, Formats.AccumulatedRadiance, UavFlags))
        return FailResize("RGBA32F UAV AccumulatedRadiance is not supported; reference accumulation is unavailable");

    if (RequiresSuperResolutionTargets && !SupportsBindFlags(pDevice, Formats.SuperResolutionInputColor, UavFlags))
        return FailResize("RGBA16F UAV SuperResolutionInputColor is not supported; RTXPT post-processing resource graph is unavailable");

    if (!SupportsBindFlags(pDevice, Formats.Depth, UavFlags))
        return FailResize("R32F UAV Depth is not supported; RTXPT post-processing resource graph is unavailable");

    if (CreateRealtimeResources && !SupportsBindFlags(pDevice, Formats.PreviousDepth, DepthRtFlags))
        return FailResize("R32F SRV/RTV PreviousDepth is not supported; RTXPT realtime TAA is unavailable");

    if (!SupportsBindFlags(pDevice, Formats.ScreenMotionVectors, UavFlags))
        return FailResize("RGBA16F UAV ScreenMotionVectors is not supported; RTXPT post-processing resource graph is unavailable");

    if (!SupportsBindFlags(pDevice, Formats.ProcessedOutputColor, HdrRtFlags))
        return FailResize("HDR UAV/RTV ProcessedOutputColor is not supported; RTXPT post-processing resource graph is unavailable");

    if (!SupportsBindFlags(pDevice, Formats.LdrColor, LdrRtFlags))
        return FailResize("LDR UAV/RTV targets are not supported; RTXPT post-processing resource graph is unavailable");

    if (RequiresSuperResolutionTargets && !SupportsBindFlags(pDevice, Formats.TemporalFeedback, UavFlags))
        return FailResize("RGBA16_SNORM UAV TemporalFeedback is not supported; RTXPT post-processing resource graph is unavailable");

    if (RequiresHistoryClampRelax && !SupportsBindFlags(pDevice, Formats.CombinedHistoryClampRelax, UavFlags))
        return FailResize("R8 UAV CombinedHistoryClampRelax is not supported; RTXPT post-processing resource graph is unavailable");

    if (CreateComputeOutput && !SupportsBindFlags(pDevice, Formats.ComputeColor, UavFlags))
        return FailResize("RGBA8 UAV ComputeColor is not supported; RTXPT post-processing resource graph is unavailable");

    if (CreateRealtimeResources)
    {
        if (!SupportsBindFlags(pDevice, Formats.StableRadiance, UavFlags))
            return FailResize("RGBA16F UAV StableRadiance is not supported; RTXPT realtime resources are unavailable");
        if (!SupportsBindFlags(pDevice, Formats.DenoisingLayersHeader, UavFlags))
            return FailResize("R32_UINT UAV DenoisingLayersHeader is not supported; RTXPT realtime resources are unavailable");
        if (!SupportsBindFlags(pDevice, Formats.Throughput, UavFlags))
            return FailResize("R32_UINT UAV Throughput is not supported; RTXPT realtime resources are unavailable");
        if (!SupportsBindFlags(pDevice, Formats.SpecularHitT, UavFlags))
            return FailResize("R32F UAV SpecularHitT is not supported; RTXPT realtime resources are unavailable");
        if (!SupportsBindFlags(pDevice, Formats.ScratchFloat1, UavFlags))
            return FailResize("R32F UAV ScratchFloat1 is not supported; RTXPT realtime resources are unavailable");
        if (!CreateRayReconstructionResources)
        {
            if (!SupportsBindFlags(pDevice, Formats.DenoiserViewspaceZ, UavFlags))
                return FailResize("R32F UAV DenoiserViewspaceZ is not supported; RTXPT realtime denoiser inputs are unavailable");
            if (!SupportsBindFlags(pDevice, Formats.DenoiserMotionVectors, UavFlags))
                return FailResize("RGBA16F UAV DenoiserMotionVectors is not supported; RTXPT realtime denoiser inputs are unavailable");
            if (!SupportsBindFlags(pDevice, Formats.DenoiserNormalRoughness, UavFlags))
                return FailResize("RGB10A2 UAV DenoiserNormalRoughness is not supported; RTXPT realtime denoiser inputs are unavailable");
            if (!SupportsBindFlags(pDevice, Formats.DenoiserDiffRadianceHitDist, UavFlags))
                return FailResize("RGBA16F UAV DenoiserDiffRadianceHitDist is not supported; RTXPT realtime denoiser inputs are unavailable");
            if (!SupportsBindFlags(pDevice, Formats.DenoiserSpecRadianceHitDist, UavFlags))
                return FailResize("RGBA16F UAV DenoiserSpecRadianceHitDist is not supported; RTXPT realtime denoiser inputs are unavailable");
            if (!SupportsBindFlags(pDevice, Formats.DenoiserDisocclusionThresholdMix, UavFlags))
                return FailResize("R8 UAV DenoiserDisocclusionThresholdMix is not supported; RTXPT realtime denoiser inputs are unavailable");
            if (!SupportsBindFlags(pDevice, Formats.DenoiserOutDiffRadianceHitDist, UavFlags))
                return FailResize("RGBA16F UAV DenoiserOutDiffRadianceHitDist is not supported; RTXPT realtime denoiser outputs are unavailable");
            if (!SupportsBindFlags(pDevice, Formats.DenoiserOutSpecRadianceHitDist, UavFlags))
                return FailResize("RGBA16F UAV DenoiserOutSpecRadianceHitDist is not supported; RTXPT realtime denoiser outputs are unavailable");
        }
        if (!SupportsBindFlags(pDevice, Formats.DenoiserAvgLayerRadianceHalfRes, UavFlags))
            return FailResize("RGBA16F UAV DenoiserAvgLayerRadianceHalfRes is not supported; RTXPT realtime guide resources are unavailable");
        if (CreateDenoiserValidation && !SupportsBindFlags(pDevice, Formats.DenoiserOutValidation, UavFlags))
            return FailResize("RGBA8 UAV DenoiserOutValidation is not supported; RTXPT realtime NRD validation output is unavailable");
    }

    const Uint32 RenderWidth   = Dimensions.RenderWidth;
    const Uint32 RenderHeight  = Dimensions.RenderHeight;
    const Uint32 DisplayWidth  = Dimensions.DisplayWidth;
    const Uint32 DisplayHeight = Dimensions.DisplayHeight;

    RefCntAutoPtr<ITexture>                                        OutputColor;
    RefCntAutoPtr<ITexture>                                        AccumulatedRadiance;
    RefCntAutoPtr<ITexture>                                        SuperResolutionInputColor;
    RefCntAutoPtr<ITexture>                                        ProcessedOutputColor;
    RefCntAutoPtr<ITexture>                                        LdrColor;
    RefCntAutoPtr<ITexture>                                        ComputeColor;
    RefCntAutoPtr<ITexture>                                        Depth;
    RefCntAutoPtr<ITexture>                                        PreviousDepth;
    RefCntAutoPtr<ITexture>                                        ScreenMotionVectors;
    RefCntAutoPtr<ITexture>                                        SpecularMotion;
    RefCntAutoPtr<ITexture>                                        PreviousAvgLayerRadiance;
    RefCntAutoPtr<ITexture>                                        TemporalFeedback1;
    RefCntAutoPtr<ITexture>                                        TemporalFeedback2;
    RefCntAutoPtr<ITexture>                                        CombinedHistoryClampRelax;
    RefCntAutoPtr<ITexture>                                        StableRadiance;
    RefCntAutoPtr<ITexture>                                        DenoisingLayersHeader;
    RefCntAutoPtr<IBuffer>                                         DenoisingLayersBuffer;
    RefCntAutoPtr<ITexture>                                        Throughput;
    RefCntAutoPtr<ITexture>                                        SpecularHitT;
    RefCntAutoPtr<ITexture>                                        ScratchFloat1;
    RefCntAutoPtr<ITexture>                                        DenoiserViewspaceZ;
    RefCntAutoPtr<ITexture>                                        DenoiserMotionVectors;
    RefCntAutoPtr<ITexture>                                        DenoiserNormalRoughness;
    RefCntAutoPtr<ITexture>                                        DenoiserDiffRadianceHitDist;
    RefCntAutoPtr<ITexture>                                        DenoiserSpecRadianceHitDist;
    RefCntAutoPtr<ITexture>                                        DenoiserDisocclusionThresholdMix;
    std::array<RefCntAutoPtr<ITexture>, kRTXPTDenoisingLayerCount> DenoiserOutDiffRadianceHitDist;
    std::array<RefCntAutoPtr<ITexture>, kRTXPTDenoisingLayerCount> DenoiserOutSpecRadianceHitDist;
    RefCntAutoPtr<ITexture>                                        DenoiserOutValidation;
    RefCntAutoPtr<ITexture>                                        DenoiserAvgLayerRadianceHalfRes;
    RefCntAutoPtr<ITexture>                                        RRDiffuseAlbedo, RRSpecAlbedo, RRNormalsAndRoughness, RRDepth, RRMotionVectors;
    RefCntAutoPtr<ITexture>                                        RRSpecMotionVectors;

    if (CreateRayReconstructionResources &&
        (!CreateTarget(pDevice, MakeTargetDesc("RTXPT RRDiffuseAlbedo", RenderWidth, RenderHeight, TEX_FORMAT_RGBA16_FLOAT, UavFlags), RRDiffuseAlbedo) ||
         !CreateTarget(pDevice, MakeTargetDesc("RTXPT RRSpecAlbedo", RenderWidth, RenderHeight, TEX_FORMAT_RGBA16_FLOAT, UavFlags), RRSpecAlbedo) ||
         !CreateTarget(pDevice, MakeTargetDesc("RTXPT RRNormalsAndRoughness", RenderWidth, RenderHeight, TEX_FORMAT_RGBA16_FLOAT, UavFlags), RRNormalsAndRoughness) ||
         !CreateTarget(pDevice, MakeTargetDesc("RTXPT RRDepth", RenderWidth, RenderHeight, TEX_FORMAT_R32_FLOAT, UavFlags), RRDepth) ||
         !CreateTarget(pDevice, MakeTargetDesc("RTXPT RRMotionVectors", RenderWidth, RenderHeight, TEX_FORMAT_RG16_FLOAT, UavFlags), RRMotionVectors) ||
         !CreateTarget(pDevice, MakeTargetDesc("RTXPT RRSpecMotionVectors", RenderWidth, RenderHeight, TEX_FORMAT_RG16_FLOAT, UavFlags), RRSpecMotionVectors)))
        return FailResize("Failed to create RTXPT RR inputs");

    if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT OutputColor", RenderWidth, RenderHeight, Formats.OutputColor, UavFlags), OutputColor))
        return FailResize("Failed to create RTXPT OutputColor");

    if (CreateAccumulatedRadiance &&
        !CreateTarget(pDevice, MakeTargetDesc("RTXPT AccumulatedRadiance", RenderWidth, RenderHeight, Formats.AccumulatedRadiance, UavFlags), AccumulatedRadiance))
        return FailResize("Failed to create RTXPT AccumulatedRadiance");

    if (RequiresSuperResolutionTargets &&
        !CreateTarget(pDevice, MakeTargetDesc("RTXPT SuperResolutionInputColor", RenderWidth, RenderHeight, Formats.SuperResolutionInputColor, UavFlags), SuperResolutionInputColor))
        return FailResize("Failed to create RTXPT SuperResolutionInputColor");

    if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT Depth", RenderWidth, RenderHeight, Formats.Depth, UavFlags), Depth))
        return FailResize("Failed to create RTXPT Depth");

    if (CreateRealtimeResources &&
        !CreateTarget(pDevice, MakeTargetDesc("RTXPT PreviousDepth", RenderWidth, RenderHeight, Formats.PreviousDepth, DepthRtFlags), PreviousDepth))
        return FailResize("Failed to create RTXPT PreviousDepth");

    if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT ScreenMotionVectors", RenderWidth, RenderHeight, Formats.ScreenMotionVectors, UavFlags), ScreenMotionVectors))
        return FailResize("Failed to create RTXPT ScreenMotionVectors");
    if (CreateRealtimeResources &&
        !CreateTarget(pDevice, MakeTargetDesc("RTXPT SpecularMotion", RenderWidth, RenderHeight, TEX_FORMAT_RGBA16_FLOAT, UavFlags), SpecularMotion))
        return FailResize("Failed to create RTXPT SpecularMotion");

    if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT ProcessedOutputColor", DisplayWidth, DisplayHeight, Formats.ProcessedOutputColor, HdrRtFlags), ProcessedOutputColor))
        return FailResize("Failed to create RTXPT ProcessedOutputColor");

    if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT LdrColor", DisplayWidth, DisplayHeight, Formats.LdrColor, LdrRtFlags), LdrColor))
        return FailResize("Failed to create RTXPT LdrColor");

    if (RequiresSuperResolutionTargets &&
        !CreateTarget(pDevice, MakeTargetDesc("RTXPT TemporalFeedback1", DisplayWidth, DisplayHeight, Formats.TemporalFeedback, UavFlags), TemporalFeedback1))
        return FailResize("Failed to create RTXPT TemporalFeedback1");

    if (RequiresSuperResolutionTargets &&
        !CreateTarget(pDevice, MakeTargetDesc("RTXPT TemporalFeedback2", DisplayWidth, DisplayHeight, Formats.TemporalFeedback, UavFlags), TemporalFeedback2))
        return FailResize("Failed to create RTXPT TemporalFeedback2");

    if (RequiresHistoryClampRelax &&
        !CreateTarget(pDevice, MakeTargetDesc("RTXPT CombinedHistoryClampRelax", DisplayWidth, DisplayHeight, Formats.CombinedHistoryClampRelax, UavFlags), CombinedHistoryClampRelax))
        return FailResize("Failed to create RTXPT CombinedHistoryClampRelax");

    if (CreateComputeOutput &&
        !CreateTarget(pDevice, MakeTargetDesc("RTXPT ComputeColor", DisplayWidth, DisplayHeight, Formats.ComputeColor, UavFlags), ComputeColor))
        return FailResize("Failed to create RTXPT ComputeColor");

    const Uint32 HalfRenderWidth             = (RenderWidth + 1u) / 2u;
    const Uint32 HalfRenderHeight            = (RenderHeight + 1u) / 2u;
    Uint64       DenoisingLayersElementCount = 0;

    if (CreateRealtimeResources)
    {
        DenoisingLayersElementCount = ComputeDenoisingLayersElementCount(RenderWidth, RenderHeight);

        if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT StableRadiance", RenderWidth, RenderHeight, Formats.StableRadiance, UavFlags), StableRadiance))
            return FailResize("Failed to create RTXPT StableRadiance");
        TextureDesc DenoisingLayersHeaderDesc = MakeTargetDesc("RTXPT DenoisingLayersHeader", RenderWidth, RenderHeight, Formats.DenoisingLayersHeader, UavFlags);
        DenoisingLayersHeaderDesc.Type        = RESOURCE_DIM_TEX_2D_ARRAY;
        DenoisingLayersHeaderDesc.ArraySize   = kDenoisingLayersHeaderPlaneCount;
        if (!CreateTarget(pDevice, DenoisingLayersHeaderDesc, DenoisingLayersHeader))
            return FailResize("Failed to create RTXPT DenoisingLayersHeader");
        if (!CreateDenoisingLayersBuffer(pDevice, DenoisingLayersElementCount, DenoisingLayersBuffer))
            return FailResize("Failed to create RTXPT DenoisingLayersBuffer");
        if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT Throughput", RenderWidth, RenderHeight, Formats.Throughput, UavFlags), Throughput))
            return FailResize("Failed to create RTXPT Throughput");
        if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT SpecularHitT", RenderWidth, RenderHeight, Formats.SpecularHitT, UavFlags), SpecularHitT))
            return FailResize("Failed to create RTXPT SpecularHitT");
        if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT ScratchFloat1", RenderWidth, RenderHeight, Formats.ScratchFloat1, UavFlags), ScratchFloat1))
            return FailResize("Failed to create RTXPT ScratchFloat1");
        if (!CreateRayReconstructionResources)
        {
            // NRD reads its inputs and outputs before every texel has been written; see RTXPTCreateZeroedTexture2D.
            std::vector<Uint8> Zeros;
            if (!CreateZeroedTarget(pDevice, MakeTargetDesc("RTXPT DenoiserViewspaceZ", RenderWidth, RenderHeight, Formats.DenoiserViewspaceZ, UavFlags), Zeros, DenoiserViewspaceZ))
                return FailResize("Failed to create RTXPT DenoiserViewspaceZ");
            if (!CreateZeroedTarget(pDevice, MakeTargetDesc("RTXPT DenoiserMotionVectors", RenderWidth, RenderHeight, Formats.DenoiserMotionVectors, UavFlags), Zeros, DenoiserMotionVectors))
                return FailResize("Failed to create RTXPT DenoiserMotionVectors");
            if (!CreateZeroedTarget(pDevice, MakeTargetDesc("RTXPT DenoiserNormalRoughness", RenderWidth, RenderHeight, Formats.DenoiserNormalRoughness, UavFlags), Zeros, DenoiserNormalRoughness))
                return FailResize("Failed to create RTXPT DenoiserNormalRoughness");
            if (!CreateZeroedTarget(pDevice, MakeTargetDesc("RTXPT DenoiserDiffRadianceHitDist", RenderWidth, RenderHeight, Formats.DenoiserDiffRadianceHitDist, UavFlags), Zeros, DenoiserDiffRadianceHitDist))
                return FailResize("Failed to create RTXPT DenoiserDiffRadianceHitDist");
            if (!CreateZeroedTarget(pDevice, MakeTargetDesc("RTXPT DenoiserSpecRadianceHitDist", RenderWidth, RenderHeight, Formats.DenoiserSpecRadianceHitDist, UavFlags), Zeros, DenoiserSpecRadianceHitDist))
                return FailResize("Failed to create RTXPT DenoiserSpecRadianceHitDist");
            if (!CreateZeroedTarget(pDevice, MakeTargetDesc("RTXPT DenoiserDisocclusionThresholdMix", RenderWidth, RenderHeight, Formats.DenoiserDisocclusionThresholdMix, UavFlags), Zeros, DenoiserDisocclusionThresholdMix))
                return FailResize("Failed to create RTXPT DenoiserDisocclusionThresholdMix");

            for (Uint32 PlaneIndex = 0; PlaneIndex < kRTXPTDenoisingLayerCount; ++PlaneIndex)
            {
                const std::string DiffName = "RTXPT DenoiserOutDiffRadianceHitDist[" + std::to_string(PlaneIndex) + "]";
                const std::string SpecName = "RTXPT DenoiserOutSpecRadianceHitDist[" + std::to_string(PlaneIndex) + "]";
                if (!CreateZeroedTarget(pDevice, MakeTargetDesc(DiffName.c_str(), RenderWidth, RenderHeight, Formats.DenoiserOutDiffRadianceHitDist, UavFlags), Zeros, DenoiserOutDiffRadianceHitDist[PlaneIndex]))
                    return FailResize("Failed to create RTXPT DenoiserOutDiffRadianceHitDist");
                if (!CreateZeroedTarget(pDevice, MakeTargetDesc(SpecName.c_str(), RenderWidth, RenderHeight, Formats.DenoiserOutSpecRadianceHitDist, UavFlags), Zeros, DenoiserOutSpecRadianceHitDist[PlaneIndex]))
                    return FailResize("Failed to create RTXPT DenoiserOutSpecRadianceHitDist");
            }

            if (CreateDenoiserValidation &&
                !CreateZeroedTarget(pDevice, MakeTargetDesc("RTXPT DenoiserOutValidation", RenderWidth, RenderHeight, Formats.DenoiserOutValidation, UavFlags), Zeros, DenoiserOutValidation))
                return FailResize("Failed to create RTXPT DenoiserOutValidation");
        }

        if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT DenoiserAvgLayerRadianceHalfRes", HalfRenderWidth, HalfRenderHeight, Formats.DenoiserAvgLayerRadianceHalfRes, UavFlags), DenoiserAvgLayerRadianceHalfRes))
            return FailResize("Failed to create RTXPT DenoiserAvgLayerRadianceHalfRes");
        if (!CreateTarget(pDevice, MakeTargetDesc("RTXPT PreviousAvgLayerRadiance", HalfRenderWidth, HalfRenderHeight, Formats.DenoiserAvgLayerRadianceHalfRes, BIND_SHADER_RESOURCE), PreviousAvgLayerRadiance))
            return FailResize("Failed to create RTXPT PreviousAvgLayerRadiance");
    }

    Reset();
    m_OutputColor                         = OutputColor;
    m_AccumulatedRadiance                 = AccumulatedRadiance;
    m_SuperResolutionInputColor           = SuperResolutionInputColor;
    m_ProcessedOutputColor                = ProcessedOutputColor;
    m_LdrColor                            = LdrColor;
    m_ComputeColor                        = ComputeColor;
    m_Depth                               = Depth;
    m_PreviousDepth                       = PreviousDepth;
    m_ScreenMotionVectors                 = ScreenMotionVectors;
    m_SpecularMotion                      = SpecularMotion;
    m_PreviousAvgLayerRadiance            = PreviousAvgLayerRadiance;
    m_TemporalFeedback1                   = TemporalFeedback1;
    m_TemporalFeedback2                   = TemporalFeedback2;
    m_CombinedHistoryClampRelax           = CombinedHistoryClampRelax;
    m_StableRadiance                      = StableRadiance;
    m_DenoisingLayersHeader               = DenoisingLayersHeader;
    m_DenoisingLayersBuffer               = DenoisingLayersBuffer;
    m_Throughput                          = Throughput;
    m_SpecularHitT                        = SpecularHitT;
    m_ScratchFloat1                       = ScratchFloat1;
    m_DenoiserViewspaceZ                  = DenoiserViewspaceZ;
    m_DenoiserMotionVectors               = DenoiserMotionVectors;
    m_DenoiserNormalRoughness             = DenoiserNormalRoughness;
    m_DenoiserDiffRadianceHitDist         = DenoiserDiffRadianceHitDist;
    m_DenoiserSpecRadianceHitDist         = DenoiserSpecRadianceHitDist;
    m_DenoiserDisocclusionThresholdMix    = DenoiserDisocclusionThresholdMix;
    m_DenoiserOutDiffRadianceHitDist      = DenoiserOutDiffRadianceHitDist;
    m_DenoiserOutSpecRadianceHitDist      = DenoiserOutSpecRadianceHitDist;
    m_DenoiserOutValidation               = DenoiserOutValidation;
    m_DenoiserAvgLayerRadianceHalfRes     = DenoiserAvgLayerRadianceHalfRes;
    m_RRDiffuseAlbedo                     = RRDiffuseAlbedo;
    m_RRSpecAlbedo                        = RRSpecAlbedo;
    m_RRNormalsAndRoughness               = RRNormalsAndRoughness;
    m_RRDepth                             = RRDepth;
    m_RRMotionVectors                     = RRMotionVectors;
    m_RRSpecMotionVectors                 = RRSpecMotionVectors;
    m_RayReconstructionResourcesRequested = CreateRayReconstructionResources;
    m_AccumulatedRadianceUnavailable      = false;
    m_AccumulatedRadianceRequested        = CreateAccumulatedRadiance;
    m_RealtimeResourcesRequested          = CreateRealtimeResources;
    m_DenoiserValidationRequested         = CreateDenoiserValidation;
    m_DenoisingLayersElementCount         = DenoisingLayersElementCount;
    m_LastFailureReason.clear();
    m_Dimensions = Dimensions;
    m_Formats    = Formats;

    return true;
}

bool RTXPTRenderTargets::Resize(IRenderDevice*                     pDevice,
                                const RTXPTRenderTargetDimensions& Dimensions,
                                const RTXPTRenderTargetFormats&    Formats,
                                bool                               CreateComputeOutput,
                                bool                               CreateAccumulatedRadiance)
{
    RTXPTRenderTargetCreateInfo CreateInfo;
    CreateInfo.Dimensions                = Dimensions;
    CreateInfo.Formats                   = Formats;
    CreateInfo.CreateComputeOutput       = CreateComputeOutput;
    CreateInfo.CreateAccumulatedRadiance = CreateAccumulatedRadiance;
    return Resize(pDevice, CreateInfo);
}

bool RTXPTRenderTargets::Resize(IRenderDevice*                  pDevice,
                                Uint32                          Width,
                                Uint32                          Height,
                                const RTXPTRenderTargetFormats& Formats,
                                bool                            CreateComputeOutput,
                                bool                            CreateAccumulatedRadiance)
{
    RTXPTRenderTargetCreateInfo CreateInfo;
    CreateInfo.Dimensions.RenderWidth           = Width;
    CreateInfo.Dimensions.RenderHeight          = Height;
    CreateInfo.Dimensions.DisplayWidth          = Width;
    CreateInfo.Dimensions.DisplayHeight         = Height;
    CreateInfo.Dimensions.SuperResolutionActive = false;
    CreateInfo.Formats                          = Formats;
    CreateInfo.CreateComputeOutput              = CreateComputeOutput;
    CreateInfo.CreateAccumulatedRadiance        = CreateAccumulatedRadiance;
    return Resize(pDevice, CreateInfo);
}

bool RTXPTRenderTargets::HasPostProcessTargets() const
{
    return m_OutputColor != nullptr &&
        (!m_AccumulatedRadianceRequested || m_AccumulatedRadiance != nullptr) &&
        m_ProcessedOutputColor != nullptr &&
        m_LdrColor != nullptr &&
        m_Depth != nullptr &&
        m_ScreenMotionVectors != nullptr &&
        (!m_Dimensions.SuperResolutionActive ||
         (m_TemporalFeedback1 != nullptr &&
          m_TemporalFeedback2 != nullptr &&
          m_CombinedHistoryClampRelax != nullptr)) &&
        (!m_Dimensions.SuperResolutionActive || m_SuperResolutionInputColor != nullptr);
}

bool RTXPTRenderTargets::HasRealtimeRenderTargets() const
{
    if (!m_RealtimeResourcesRequested)
        return false;

    const bool HasPerPlaneOutputs =
        std::all_of(m_DenoiserOutDiffRadianceHitDist.begin(), m_DenoiserOutDiffRadianceHitDist.end(),
                    [](const RefCntAutoPtr<ITexture>& Texture) { return Texture != nullptr; }) &&
        std::all_of(m_DenoiserOutSpecRadianceHitDist.begin(), m_DenoiserOutSpecRadianceHitDist.end(),
                    [](const RefCntAutoPtr<ITexture>& Texture) { return Texture != nullptr; });

    return m_PreviousDepth != nullptr &&
        m_SpecularMotion != nullptr &&
        m_StableRadiance != nullptr &&
        m_DenoisingLayersHeader != nullptr &&
        m_DenoisingLayersBuffer != nullptr &&
        m_Throughput != nullptr &&
        m_SpecularHitT != nullptr &&
        m_ScratchFloat1 != nullptr &&
        (m_RayReconstructionResourcesRequested ||
         (m_DenoiserViewspaceZ != nullptr &&
          m_DenoiserMotionVectors != nullptr &&
          m_DenoiserNormalRoughness != nullptr &&
          m_DenoiserDiffRadianceHitDist != nullptr &&
          m_DenoiserSpecRadianceHitDist != nullptr &&
          m_DenoiserDisocclusionThresholdMix != nullptr &&
          HasPerPlaneOutputs &&
          (!m_DenoiserValidationRequested || m_DenoiserOutValidation != nullptr))) &&
        m_DenoiserAvgLayerRadianceHalfRes != nullptr &&
        m_PreviousAvgLayerRadiance != nullptr &&
        m_DenoisingLayersElementCount > 0;
}

namespace
{

ITextureView* GetTextureUAV(const RefCntAutoPtr<ITexture>& Texture)
{
    return Texture ? Texture->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* GetTextureSRV(const RefCntAutoPtr<ITexture>& Texture)
{
    return Texture ? Texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

IBufferView* GetBufferUAV(const RefCntAutoPtr<IBuffer>& Buffer)
{
    return Buffer ? Buffer->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS) : nullptr;
}

IBufferView* GetBufferSRV(const RefCntAutoPtr<IBuffer>& Buffer)
{
    return Buffer ? Buffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE) : nullptr;
}

} // namespace

ITextureView* RTXPTRenderTargets::GetOutputColorUAV() const
{
    return m_OutputColor ? m_OutputColor->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetOutputColorSRV() const
{
    return m_OutputColor ? m_OutputColor->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetAccumulatedRadianceUAV() const
{
    return m_AccumulatedRadiance ? m_AccumulatedRadiance->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetAccumulatedRadianceSRV() const
{
    return m_AccumulatedRadiance ? m_AccumulatedRadiance->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetProcessedOutputColorUAV() const
{
    return m_ProcessedOutputColor ? m_ProcessedOutputColor->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetProcessedOutputColorSRV() const
{
    return m_ProcessedOutputColor ? m_ProcessedOutputColor->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetProcessedOutputColorRTV() const
{
    return m_ProcessedOutputColor ? m_ProcessedOutputColor->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetLdrColorUAV() const
{
    return m_LdrColor ? m_LdrColor->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetLdrColorSRV() const
{
    return m_LdrColor ? m_LdrColor->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetLdrColorRTV() const
{
    return m_LdrColor ? m_LdrColor->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET) : nullptr;
}

ITexture* RTXPTRenderTargets::GetLdrColorTexture() const
{
    return m_LdrColor;
}

ITextureView* RTXPTRenderTargets::GetComputeColorUAV() const
{
    return m_ComputeColor ? m_ComputeColor->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetComputeColorSRV() const
{
    return m_ComputeColor ? m_ComputeColor->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetPresentationSRV() const
{
    return GetLdrColorSRV();
}

ITextureView* RTXPTRenderTargets::GetSuperResolutionInputColorUAV() const
{
    return m_SuperResolutionInputColor ? m_SuperResolutionInputColor->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetSuperResolutionInputColorSRV() const
{
    return m_SuperResolutionInputColor ? m_SuperResolutionInputColor->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetDepthUAV() const
{
    return m_Depth ? m_Depth->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetDepthSRV() const
{
    return m_Depth ? m_Depth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetPreviousDepthSRV() const
{
    return m_PreviousDepth ? m_PreviousDepth->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetPreviousDepthRTV() const
{
    return m_PreviousDepth ? m_PreviousDepth->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetScreenMotionVectorsUAV() const
{
    return m_ScreenMotionVectors ? m_ScreenMotionVectors->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetScreenMotionVectorsSRV() const
{
    return m_ScreenMotionVectors ? m_ScreenMotionVectors->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetSpecularMotionUAV() const
{
    return m_SpecularMotion ? m_SpecularMotion->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetSpecularMotionSRV() const
{
    return m_SpecularMotion ? m_SpecularMotion->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetPreviousAvgLayerRadianceSRV() const
{
    return m_PreviousAvgLayerRadiance ? m_PreviousAvgLayerRadiance->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetTemporalFeedback1UAV() const
{
    return m_TemporalFeedback1 ? m_TemporalFeedback1->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetTemporalFeedback1SRV() const
{
    return m_TemporalFeedback1 ? m_TemporalFeedback1->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetTemporalFeedback2UAV() const
{
    return m_TemporalFeedback2 ? m_TemporalFeedback2->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetTemporalFeedback2SRV() const
{
    return m_TemporalFeedback2 ? m_TemporalFeedback2->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetCombinedHistoryClampRelaxUAV() const
{
    return m_CombinedHistoryClampRelax ? m_CombinedHistoryClampRelax->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetCombinedHistoryClampRelaxSRV() const
{
    return m_CombinedHistoryClampRelax ? m_CombinedHistoryClampRelax->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

bool RTXPTRenderTargets::HasRayReconstructionRenderTargets() const
{
    return m_RayReconstructionResourcesRequested && HasPostProcessTargets() && HasRealtimeRenderTargets() &&
        m_RRDiffuseAlbedo && m_RRSpecAlbedo && m_RRNormalsAndRoughness && m_RRDepth && m_RRMotionVectors && m_RRSpecMotionVectors;
}

ITextureView* RTXPTRenderTargets::GetRRDiffuseAlbedoUAV() const { return GetTextureUAV(m_RRDiffuseAlbedo); }
ITextureView* RTXPTRenderTargets::GetRRDiffuseAlbedoSRV() const { return GetTextureSRV(m_RRDiffuseAlbedo); }
ITextureView* RTXPTRenderTargets::GetRRSpecAlbedoUAV() const { return GetTextureUAV(m_RRSpecAlbedo); }
ITextureView* RTXPTRenderTargets::GetRRSpecAlbedoSRV() const { return GetTextureSRV(m_RRSpecAlbedo); }
ITextureView* RTXPTRenderTargets::GetRRNormalsAndRoughnessUAV() const { return GetTextureUAV(m_RRNormalsAndRoughness); }
ITextureView* RTXPTRenderTargets::GetRRNormalsAndRoughnessSRV() const { return GetTextureSRV(m_RRNormalsAndRoughness); }
ITextureView* RTXPTRenderTargets::GetRRDepthUAV() const { return GetTextureUAV(m_RRDepth); }
ITextureView* RTXPTRenderTargets::GetRRDepthSRV() const { return GetTextureSRV(m_RRDepth); }
ITextureView* RTXPTRenderTargets::GetRRMotionVectorsUAV() const { return GetTextureUAV(m_RRMotionVectors); }
ITextureView* RTXPTRenderTargets::GetRRMotionVectorsSRV() const { return GetTextureSRV(m_RRMotionVectors); }
ITextureView* RTXPTRenderTargets::GetRRSpecMotionVectorsUAV() const { return GetTextureUAV(m_RRSpecMotionVectors); }
ITextureView* RTXPTRenderTargets::GetRRSpecMotionVectorsSRV() const { return GetTextureSRV(m_RRSpecMotionVectors); }

ITextureView* RTXPTRenderTargets::GetStableRadianceUAV() const { return GetTextureUAV(m_StableRadiance); }
ITextureView* RTXPTRenderTargets::GetStableRadianceSRV() const { return GetTextureSRV(m_StableRadiance); }
ITextureView* RTXPTRenderTargets::GetDenoisingLayersHeaderUAV() const { return GetTextureUAV(m_DenoisingLayersHeader); }
ITextureView* RTXPTRenderTargets::GetDenoisingLayersHeaderSRV() const { return GetTextureSRV(m_DenoisingLayersHeader); }
IBufferView*  RTXPTRenderTargets::GetDenoisingLayersBufferUAV() const { return GetBufferUAV(m_DenoisingLayersBuffer); }
IBufferView*  RTXPTRenderTargets::GetDenoisingLayersBufferSRV() const { return GetBufferSRV(m_DenoisingLayersBuffer); }
IBuffer*      RTXPTRenderTargets::GetDenoisingLayersBuffer() const { return m_DenoisingLayersBuffer; }
ITextureView* RTXPTRenderTargets::GetThroughputUAV() const { return GetTextureUAV(m_Throughput); }
ITextureView* RTXPTRenderTargets::GetThroughputSRV() const { return GetTextureSRV(m_Throughput); }
ITextureView* RTXPTRenderTargets::GetSpecularHitTUAV() const { return GetTextureUAV(m_SpecularHitT); }
ITextureView* RTXPTRenderTargets::GetSpecularHitTSRV() const { return GetTextureSRV(m_SpecularHitT); }
ITextureView* RTXPTRenderTargets::GetScratchFloat1UAV() const { return GetTextureUAV(m_ScratchFloat1); }
ITextureView* RTXPTRenderTargets::GetScratchFloat1SRV() const { return GetTextureSRV(m_ScratchFloat1); }
ITextureView* RTXPTRenderTargets::GetDenoiserViewspaceZUAV() const { return GetTextureUAV(m_DenoiserViewspaceZ); }
ITextureView* RTXPTRenderTargets::GetDenoiserViewspaceZSRV() const { return GetTextureSRV(m_DenoiserViewspaceZ); }
ITextureView* RTXPTRenderTargets::GetDenoiserMotionVectorsUAV() const { return GetTextureUAV(m_DenoiserMotionVectors); }
ITextureView* RTXPTRenderTargets::GetDenoiserMotionVectorsSRV() const { return GetTextureSRV(m_DenoiserMotionVectors); }
ITextureView* RTXPTRenderTargets::GetDenoiserNormalRoughnessUAV() const { return GetTextureUAV(m_DenoiserNormalRoughness); }
ITextureView* RTXPTRenderTargets::GetDenoiserNormalRoughnessSRV() const { return GetTextureSRV(m_DenoiserNormalRoughness); }
ITextureView* RTXPTRenderTargets::GetDenoiserDiffRadianceHitDistUAV() const { return GetTextureUAV(m_DenoiserDiffRadianceHitDist); }
ITextureView* RTXPTRenderTargets::GetDenoiserDiffRadianceHitDistSRV() const { return GetTextureSRV(m_DenoiserDiffRadianceHitDist); }
ITextureView* RTXPTRenderTargets::GetDenoiserSpecRadianceHitDistUAV() const { return GetTextureUAV(m_DenoiserSpecRadianceHitDist); }
ITextureView* RTXPTRenderTargets::GetDenoiserSpecRadianceHitDistSRV() const { return GetTextureSRV(m_DenoiserSpecRadianceHitDist); }
ITextureView* RTXPTRenderTargets::GetDenoiserDisocclusionThresholdMixUAV() const { return GetTextureUAV(m_DenoiserDisocclusionThresholdMix); }
ITextureView* RTXPTRenderTargets::GetDenoiserDisocclusionThresholdMixSRV() const { return GetTextureSRV(m_DenoiserDisocclusionThresholdMix); }

ITextureView* RTXPTRenderTargets::GetDenoiserOutDiffRadianceHitDistUAV(Uint32 PlaneIndex) const
{
    return PlaneIndex < m_DenoiserOutDiffRadianceHitDist.size() ? GetTextureUAV(m_DenoiserOutDiffRadianceHitDist[PlaneIndex]) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetDenoiserOutDiffRadianceHitDistSRV(Uint32 PlaneIndex) const
{
    return PlaneIndex < m_DenoiserOutDiffRadianceHitDist.size() ? GetTextureSRV(m_DenoiserOutDiffRadianceHitDist[PlaneIndex]) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetDenoiserOutSpecRadianceHitDistUAV(Uint32 PlaneIndex) const
{
    return PlaneIndex < m_DenoiserOutSpecRadianceHitDist.size() ? GetTextureUAV(m_DenoiserOutSpecRadianceHitDist[PlaneIndex]) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetDenoiserOutSpecRadianceHitDistSRV(Uint32 PlaneIndex) const
{
    return PlaneIndex < m_DenoiserOutSpecRadianceHitDist.size() ? GetTextureSRV(m_DenoiserOutSpecRadianceHitDist[PlaneIndex]) : nullptr;
}

ITextureView* RTXPTRenderTargets::GetDenoiserOutValidationUAV() const { return GetTextureUAV(m_DenoiserOutValidation); }
ITextureView* RTXPTRenderTargets::GetDenoiserOutValidationSRV() const { return GetTextureSRV(m_DenoiserOutValidation); }
ITextureView* RTXPTRenderTargets::GetDenoiserAvgLayerRadianceHalfResUAV() const { return GetTextureUAV(m_DenoiserAvgLayerRadianceHalfRes); }
ITextureView* RTXPTRenderTargets::GetDenoiserAvgLayerRadianceHalfResSRV() const { return GetTextureSRV(m_DenoiserAvgLayerRadianceHalfRes); }

ITextureView* RTXPTRenderTargets::GetAccumulationOutputUAV() const
{
    return IsSuperResolutionActive() ? GetSuperResolutionInputColorUAV() : GetProcessedOutputColorUAV();
}

ITextureView* RTXPTRenderTargets::GetSuperResolutionColorSRV() const
{
    return IsSuperResolutionActive() ? GetSuperResolutionInputColorSRV() : GetProcessedOutputColorSRV();
}

ITextureView* RTXPTRenderTargets::GetSuperResolutionOutputUAV() const
{
    return GetProcessedOutputColorUAV();
}

} // namespace Diligent
