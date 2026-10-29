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

#pragma once

#include <array>
#include <cstddef>
#include <string>

#include "BasicMath.hpp"
#include "Buffer.h"
#include "RenderDevice.h"
#include "RefCntAutoPtr.hpp"
#include "RTXPTRealtimeSettings.hpp"
#include "Texture.h"
#include "TextureView.h"

namespace Diligent
{

struct RTXPTRenderTargetFormats
{
    TEXTURE_FORMAT OutputColor               = TEX_FORMAT_RGBA16_FLOAT;
    TEXTURE_FORMAT AccumulatedRadiance       = TEX_FORMAT_RGBA32_FLOAT;
    TEXTURE_FORMAT SuperResolutionInputColor = TEX_FORMAT_RGBA16_FLOAT;
    TEXTURE_FORMAT ProcessedOutputColor      = TEX_FORMAT_RGBA16_FLOAT;
    TEXTURE_FORMAT LdrColor                  = TEX_FORMAT_RGBA8_UNORM;
    TEXTURE_FORMAT ComputeColor              = TEX_FORMAT_RGBA8_UNORM;
    TEXTURE_FORMAT Depth                     = TEX_FORMAT_R32_FLOAT;
    TEXTURE_FORMAT PreviousDepth             = TEX_FORMAT_R32_FLOAT;
    TEXTURE_FORMAT ScreenMotionVectors       = TEX_FORMAT_RGBA16_FLOAT;
    TEXTURE_FORMAT TemporalFeedback          = TEX_FORMAT_RGBA16_SNORM;
    TEXTURE_FORMAT CombinedHistoryClampRelax = TEX_FORMAT_R8_UNORM;

    TEXTURE_FORMAT StableRadiance                   = TEX_FORMAT_RGBA16_FLOAT;
    TEXTURE_FORMAT DenoisingLayersHeader            = TEX_FORMAT_R32_UINT;
    TEXTURE_FORMAT Throughput                       = TEX_FORMAT_R32_UINT;
    TEXTURE_FORMAT SpecularHitT                     = TEX_FORMAT_R32_FLOAT;
    TEXTURE_FORMAT ScratchFloat1                    = TEX_FORMAT_R32_FLOAT;
    TEXTURE_FORMAT DenoiserViewspaceZ               = TEX_FORMAT_R32_FLOAT;
    TEXTURE_FORMAT DenoiserMotionVectors            = TEX_FORMAT_RGBA16_FLOAT;
    TEXTURE_FORMAT DenoiserNormalRoughness          = TEX_FORMAT_RGB10A2_UNORM;
    TEXTURE_FORMAT DenoiserDiffRadianceHitDist      = TEX_FORMAT_RGBA16_FLOAT;
    TEXTURE_FORMAT DenoiserSpecRadianceHitDist      = TEX_FORMAT_RGBA16_FLOAT;
    TEXTURE_FORMAT DenoiserDisocclusionThresholdMix = TEX_FORMAT_R8_UNORM;
    TEXTURE_FORMAT DenoiserOutDiffRadianceHitDist   = TEX_FORMAT_RGBA16_FLOAT;
    TEXTURE_FORMAT DenoiserOutSpecRadianceHitDist   = TEX_FORMAT_RGBA16_FLOAT;
    TEXTURE_FORMAT DenoiserOutValidation            = TEX_FORMAT_RGBA8_UNORM;
    TEXTURE_FORMAT DenoiserAvgLayerRadianceHalfRes  = TEX_FORMAT_RGBA16_FLOAT;
};

struct RTXPTRenderTargetDimensions
{
    Uint32 RenderWidth           = 1;
    Uint32 RenderHeight          = 1;
    Uint32 DisplayWidth          = 1;
    Uint32 DisplayHeight         = 1;
    bool   SuperResolutionActive = false;

    bool IsValid() const
    {
        if (RenderWidth == 0 || RenderHeight == 0 || DisplayWidth == 0 || DisplayHeight == 0)
            return false;

        return SuperResolutionActive ?
            RenderWidth <= DisplayWidth && RenderHeight <= DisplayHeight :
            RenderWidth == DisplayWidth && RenderHeight == DisplayHeight;
    }

    bool operator==(const RTXPTRenderTargetDimensions& RHS) const
    {
        return RenderWidth == RHS.RenderWidth &&
            RenderHeight == RHS.RenderHeight &&
            DisplayWidth == RHS.DisplayWidth &&
            DisplayHeight == RHS.DisplayHeight &&
            SuperResolutionActive == RHS.SuperResolutionActive;
    }
};

struct RTXPTDenoisingLayerData
{
    float3 RayOrigin       = float3{0, 0, 0};
    float  LastRayTCurrent = 0.0f;
    float3 RayDir          = float3{0, 0, 1};
    float  SceneLength     = 0.0f;

    uint3  PackedThpAndMVs            = uint3{0, 0, 0};
    Uint32 VertexIndexAndRoughness    = 0;
    uint3  DenoiserPackedBSDFEstimate = uint3{0, 0, 0};
    Uint32 PackedNormal               = 0;

    Uint32 PackedNoisyRadianceAndSpecAvg[2] = {};
    Uint32 FlagsAndVertexIndex              = 0;
    Uint32 PackedCounters                   = 0;
};
static_assert(sizeof(RTXPTDenoisingLayerData) == 80, "RTXPTDenoisingLayerData layout must match PathTracer/DenoisingLayers.hlsli DenoisingLayer");
static_assert(offsetof(RTXPTDenoisingLayerData, LastRayTCurrent) == 12, "RTXPTDenoisingLayerData LastRayTCurrent offset must match DenoisingLayer");
static_assert(offsetof(RTXPTDenoisingLayerData, RayDir) == 16, "RTXPTDenoisingLayerData RayDir offset must match DenoisingLayer");
static_assert(offsetof(RTXPTDenoisingLayerData, SceneLength) == 28, "RTXPTDenoisingLayerData SceneLength offset must match DenoisingLayer");
static_assert(offsetof(RTXPTDenoisingLayerData, PackedThpAndMVs) == 32, "RTXPTDenoisingLayerData PackedThpAndMVs offset must match DenoisingLayer");
static_assert(offsetof(RTXPTDenoisingLayerData, VertexIndexAndRoughness) == 44, "RTXPTDenoisingLayerData VertexIndexAndRoughness offset must match DenoisingLayer");
static_assert(offsetof(RTXPTDenoisingLayerData, DenoiserPackedBSDFEstimate) == 48, "RTXPTDenoisingLayerData DenoiserPackedBSDFEstimate offset must match DenoisingLayer");
static_assert(offsetof(RTXPTDenoisingLayerData, PackedNormal) == 60, "RTXPTDenoisingLayerData PackedNormal offset must match DenoisingLayer");
static_assert(offsetof(RTXPTDenoisingLayerData, PackedNoisyRadianceAndSpecAvg) == 64, "RTXPTDenoisingLayerData PackedNoisyRadianceAndSpecAvg offset must match DenoisingLayer");
static_assert(offsetof(RTXPTDenoisingLayerData, FlagsAndVertexIndex) == 72, "RTXPTDenoisingLayerData FlagsAndVertexIndex offset must match DenoisingLayer");
static_assert(offsetof(RTXPTDenoisingLayerData, PackedCounters) == 76, "RTXPTDenoisingLayerData PackedCounters offset must match DenoisingLayer");

struct RTXPTRenderTargetCreateInfo
{
    RTXPTRenderTargetDimensions Dimensions;
    RTXPTRenderTargetFormats    Formats;
    bool                        CreateComputeOutput              = false;
    bool                        CreateAccumulatedRadiance        = false;
    bool                        CreateRealtimeResources          = false;
    bool                        CreateDenoiserValidation         = false;
    bool                        CreateRayReconstructionResources = false;
};

class RTXPTRenderTargets
{
public:
    void Reset();
    bool Resize(IRenderDevice* pDevice, const RTXPTRenderTargetCreateInfo& CreateInfo);
    bool Resize(IRenderDevice*                     pDevice,
                const RTXPTRenderTargetDimensions& Dimensions,
                const RTXPTRenderTargetFormats&    Formats,
                bool                               CreateComputeOutput,
                bool                               CreateAccumulatedRadiance);
    bool Resize(IRenderDevice*                  pDevice,
                Uint32                          Width,
                Uint32                          Height,
                const RTXPTRenderTargetFormats& Formats,
                bool                            CreateComputeOutput,
                bool                            CreateAccumulatedRadiance);

    bool IsValid() const { return HasPostProcessTargets(); }
    bool HasPostProcessTargets() const;
    bool IsAccumulationActive() const { return m_AccumulatedRadiance != nullptr; }

    ITextureView* GetOutputColorUAV() const;
    ITextureView* GetOutputColorSRV() const;
    ITextureView* GetAccumulatedRadianceUAV() const;
    ITextureView* GetAccumulatedRadianceSRV() const;
    ITextureView* GetProcessedOutputColorUAV() const;
    ITextureView* GetProcessedOutputColorSRV() const;
    ITextureView* GetProcessedOutputColorRTV() const;
    ITextureView* GetLdrColorUAV() const;
    ITextureView* GetLdrColorSRV() const;
    ITextureView* GetLdrColorRTV() const;
    ITexture*     GetLdrColorTexture() const;
    ITextureView* GetComputeColorUAV() const;
    ITextureView* GetComputeColorSRV() const;
    ITextureView* GetPresentationSRV() const;
    ITextureView* GetSuperResolutionInputColorUAV() const;
    ITextureView* GetSuperResolutionInputColorSRV() const;
    ITextureView* GetDepthUAV() const;
    ITextureView* GetDepthSRV() const;
    ITextureView* GetPreviousDepthSRV() const;
    ITextureView* GetPreviousDepthRTV() const;
    ITextureView* GetScreenMotionVectorsUAV() const;
    ITextureView* GetScreenMotionVectorsSRV() const;
    ITextureView* GetTemporalFeedback1UAV() const;
    ITextureView* GetTemporalFeedback1SRV() const;
    ITextureView* GetTemporalFeedback2UAV() const;
    ITextureView* GetTemporalFeedback2SRV() const;
    ITextureView* GetCombinedHistoryClampRelaxUAV() const;
    ITextureView* GetCombinedHistoryClampRelaxSRV() const;
    bool          HasRealtimeRenderTargets() const;
    bool          AreRealtimeRenderTargetsRequested() const { return m_RealtimeResourcesRequested; }
    const char*   GetLastFailureReason() const { return m_LastFailureReason.c_str(); }
    Uint64        GetDenoisingLayersElementCount() const { return m_DenoisingLayersElementCount; }

    bool          HasRayReconstructionRenderTargets() const;
    ITextureView* GetRRDiffuseAlbedoUAV() const;
    ITextureView* GetRRDiffuseAlbedoSRV() const;
    ITextureView* GetRRSpecAlbedoUAV() const;
    ITextureView* GetRRSpecAlbedoSRV() const;
    ITextureView* GetRRNormalsAndRoughnessUAV() const;
    ITextureView* GetRRNormalsAndRoughnessSRV() const;
    ITextureView* GetRRDepthUAV() const;
    ITextureView* GetRRDepthSRV() const;
    ITextureView* GetRRMotionVectorsUAV() const;
    ITextureView* GetRRMotionVectorsSRV() const;
    ITextureView* GetRRSpecMotionVectorsUAV() const;
    ITextureView* GetRRSpecMotionVectorsSRV() const;

    ITextureView* GetStableRadianceUAV() const;
    ITextureView* GetStableRadianceSRV() const;
    ITextureView* GetDenoisingLayersHeaderUAV() const;
    ITextureView* GetDenoisingLayersHeaderSRV() const;
    IBufferView*  GetDenoisingLayersBufferUAV() const;
    IBufferView*  GetDenoisingLayersBufferSRV() const;
    IBuffer*      GetDenoisingLayersBuffer() const;
    ITextureView* GetThroughputUAV() const;
    ITextureView* GetThroughputSRV() const;
    ITextureView* GetSpecularHitTUAV() const;
    ITextureView* GetSpecularHitTSRV() const;
    ITextureView* GetScratchFloat1UAV() const;
    ITextureView* GetScratchFloat1SRV() const;
    ITextureView* GetDenoiserViewspaceZUAV() const;
    ITextureView* GetDenoiserViewspaceZSRV() const;
    ITextureView* GetDenoiserMotionVectorsUAV() const;
    ITextureView* GetDenoiserMotionVectorsSRV() const;
    ITextureView* GetDenoiserNormalRoughnessUAV() const;
    ITextureView* GetDenoiserNormalRoughnessSRV() const;
    ITextureView* GetDenoiserDiffRadianceHitDistUAV() const;
    ITextureView* GetDenoiserDiffRadianceHitDistSRV() const;
    ITextureView* GetDenoiserSpecRadianceHitDistUAV() const;
    ITextureView* GetDenoiserSpecRadianceHitDistSRV() const;
    ITextureView* GetDenoiserDisocclusionThresholdMixUAV() const;
    ITextureView* GetDenoiserDisocclusionThresholdMixSRV() const;
    ITextureView* GetDenoiserOutDiffRadianceHitDistUAV(Uint32 PlaneIndex) const;
    ITextureView* GetDenoiserOutDiffRadianceHitDistSRV(Uint32 PlaneIndex) const;
    ITextureView* GetDenoiserOutSpecRadianceHitDistUAV(Uint32 PlaneIndex) const;
    ITextureView* GetDenoiserOutSpecRadianceHitDistSRV(Uint32 PlaneIndex) const;
    ITextureView* GetDenoiserOutValidationUAV() const;
    ITextureView* GetDenoiserOutValidationSRV() const;
    ITextureView* GetDenoiserAvgLayerRadianceHalfResUAV() const;
    ITextureView* GetDenoiserAvgLayerRadianceHalfResSRV() const;

    ITextureView* GetAccumulationOutputUAV() const;
    ITextureView* GetSuperResolutionColorSRV() const;
    ITextureView* GetSuperResolutionOutputUAV() const;

    Uint32                             GetRenderWidth() const { return m_Dimensions.RenderWidth; }
    Uint32                             GetRenderHeight() const { return m_Dimensions.RenderHeight; }
    Uint32                             GetDisplayWidth() const { return m_Dimensions.DisplayWidth; }
    Uint32                             GetDisplayHeight() const { return m_Dimensions.DisplayHeight; }
    Uint32                             GetWidth() const { return GetRenderWidth(); }
    Uint32                             GetHeight() const { return GetRenderHeight(); }
    bool                               IsSuperResolutionActive() const { return m_Dimensions.SuperResolutionActive; }
    const RTXPTRenderTargetDimensions& GetDimensions() const { return m_Dimensions; }
    TEXTURE_FORMAT                     GetOutputColorFormat() const { return m_Formats.OutputColor; }
    TEXTURE_FORMAT                     GetAccumulatedRadianceFormat() const { return m_Formats.AccumulatedRadiance; }
    TEXTURE_FORMAT                     GetSuperResolutionInputColorFormat() const { return m_Formats.SuperResolutionInputColor; }
    TEXTURE_FORMAT                     GetProcessedOutputColorFormat() const { return m_Formats.ProcessedOutputColor; }
    TEXTURE_FORMAT                     GetLdrColorFormat() const { return m_Formats.LdrColor; }
    TEXTURE_FORMAT                     GetDepthFormat() const { return m_Formats.Depth; }
    TEXTURE_FORMAT                     GetPreviousDepthFormat() const { return m_Formats.PreviousDepth; }
    TEXTURE_FORMAT                     GetScreenMotionVectorsFormat() const { return m_Formats.ScreenMotionVectors; }

private:
    bool CreateTarget(IRenderDevice* pDevice, const TextureDesc& Desc, RefCntAutoPtr<ITexture>& Target);
    bool CreateDenoisingLayersBuffer(IRenderDevice*          pDevice,
                                     Uint64                  ElementCount,
                                     RefCntAutoPtr<IBuffer>& Target);
    bool SupportsBindFlags(IRenderDevice* pDevice, TEXTURE_FORMAT TargetFormat, BIND_FLAGS BindFlags) const;
    bool FailResize(const char* Reason);

    RefCntAutoPtr<ITexture>                                        m_OutputColor;
    RefCntAutoPtr<ITexture>                                        m_AccumulatedRadiance;
    RefCntAutoPtr<ITexture>                                        m_SuperResolutionInputColor;
    RefCntAutoPtr<ITexture>                                        m_ProcessedOutputColor;
    RefCntAutoPtr<ITexture>                                        m_LdrColor;
    RefCntAutoPtr<ITexture>                                        m_ComputeColor;
    RefCntAutoPtr<ITexture>                                        m_Depth;
    RefCntAutoPtr<ITexture>                                        m_PreviousDepth;
    RefCntAutoPtr<ITexture>                                        m_ScreenMotionVectors;
    RefCntAutoPtr<ITexture>                                        m_TemporalFeedback1;
    RefCntAutoPtr<ITexture>                                        m_TemporalFeedback2;
    RefCntAutoPtr<ITexture>                                        m_CombinedHistoryClampRelax;
    RefCntAutoPtr<ITexture>                                        m_StableRadiance;
    RefCntAutoPtr<ITexture>                                        m_DenoisingLayersHeader;
    RefCntAutoPtr<IBuffer>                                         m_DenoisingLayersBuffer;
    RefCntAutoPtr<ITexture>                                        m_Throughput;
    RefCntAutoPtr<ITexture>                                        m_SpecularHitT;
    RefCntAutoPtr<ITexture>                                        m_ScratchFloat1;
    RefCntAutoPtr<ITexture>                                        m_DenoiserViewspaceZ;
    RefCntAutoPtr<ITexture>                                        m_DenoiserMotionVectors;
    RefCntAutoPtr<ITexture>                                        m_DenoiserNormalRoughness;
    RefCntAutoPtr<ITexture>                                        m_DenoiserDiffRadianceHitDist;
    RefCntAutoPtr<ITexture>                                        m_DenoiserSpecRadianceHitDist;
    RefCntAutoPtr<ITexture>                                        m_DenoiserDisocclusionThresholdMix;
    std::array<RefCntAutoPtr<ITexture>, kRTXPTDenoisingLayerCount> m_DenoiserOutDiffRadianceHitDist;
    std::array<RefCntAutoPtr<ITexture>, kRTXPTDenoisingLayerCount> m_DenoiserOutSpecRadianceHitDist;
    RefCntAutoPtr<ITexture>                                        m_DenoiserOutValidation;
    RefCntAutoPtr<ITexture>                                        m_DenoiserAvgLayerRadianceHalfRes;
    // Fixed RR formats match the prepare shader, independently of NRD encoding.
    RefCntAutoPtr<ITexture>     m_RRDiffuseAlbedo;
    RefCntAutoPtr<ITexture>     m_RRSpecAlbedo;
    RefCntAutoPtr<ITexture>     m_RRNormalsAndRoughness;
    RefCntAutoPtr<ITexture>     m_RRDepth;
    RefCntAutoPtr<ITexture>     m_RRMotionVectors;
    RefCntAutoPtr<ITexture>     m_RRSpecMotionVectors;
    bool                        m_RayReconstructionResourcesRequested = false;
    bool                        m_AccumulatedRadianceUnavailable      = false;
    bool                        m_AccumulatedRadianceRequested        = false;
    bool                        m_RealtimeResourcesRequested          = false;
    bool                        m_DenoiserValidationRequested         = false;
    bool                        m_PendingResizeRequestValid           = false;
    bool                        m_PendingRealtimeResources            = false;
    bool                        m_PendingDenoiserValidation           = false;
    Uint64                      m_DenoisingLayersElementCount         = 0;
    std::string                 m_LastFailureReason;
    RTXPTRenderTargetDimensions m_Dimensions = {0, 0, 0, 0, false};
    RTXPTRenderTargetFormats    m_Formats    = {};
};

} // namespace Diligent
