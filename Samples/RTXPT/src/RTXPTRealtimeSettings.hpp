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

#include <algorithm>

#include "BasicTypes.h"

namespace Diligent
{

constexpr Uint32 kRTXPTStablePlaneCount           = 3;
constexpr Uint32 kRTXPTStablePlaneMaxVertexIndex  = 15;
constexpr Uint32 kRTXPTGenericTSTileSize          = 8;
constexpr Uint32 kRTXPTRealtimeSamplesPerPixelMax = 64;

enum class RTXPTRealtimeAAMode : Uint32
{
    Disabled        = 0,
    TAA             = 1,
    SuperResolution = 2,
    DLSSRR          = 3
};

// Quality / performance trade-off for the realtime super resolution upscaler. Mirrors
// Diligent's SUPER_RESOLUTION_OPTIMIZATION_TYPE 1:1; the mapping lives in RTXPTSample.cpp
// so this lean settings header stays free of graphics-interface includes.
enum class RTXPTSuperResolutionQuality : Uint32
{
    MaxQuality      = 0,
    HighQuality     = 1,
    Balanced        = 2,
    HighPerformance = 3,
    MaxPerformance  = 4,
};

constexpr Uint32 kRTXPTSuperResolutionQualityCount = 5;

enum class RTXPTNrdMethod : Uint32
{
    REBLUR = 0,
    RELAX  = 1
};

enum class RTXPTDenoisingGuideDebugView : Uint32
{
    Disabled         = 0,
    Depth            = 1,
    MotionVectors    = 2,
    SpecularHitT     = 3,
    AvgLayerRadiance = 4,
    PrimaryLayer     = 5
};

enum class RTXPTNrdHitDistanceReconstructionMode : Uint32
{
    Off     = 0,
    Area3x3 = 1,
    Area5x5 = 2
};

enum RTXPTRealtimeResetFlags : Uint32
{
    RTXPT_REALTIME_RESET_NONE                   = 0u,
    RTXPT_REALTIME_RESET_ACCUMULATION           = 1u << 0u,
    RTXPT_REALTIME_RESET_REALTIME_CACHES        = 1u << 1u,
    RTXPT_REALTIME_RESET_NRD_HISTORY            = 1u << 2u,
    RTXPT_REALTIME_RESET_TAA_SR_HISTORY         = 1u << 3u,
    RTXPT_REALTIME_RESET_RENDER_TARGET_RECREATE = 1u << 4u,
};

inline RTXPTRealtimeResetFlags operator|(RTXPTRealtimeResetFlags LHS, RTXPTRealtimeResetFlags RHS)
{
    return static_cast<RTXPTRealtimeResetFlags>(static_cast<Uint32>(LHS) | static_cast<Uint32>(RHS));
}

inline RTXPTRealtimeResetFlags operator&(RTXPTRealtimeResetFlags LHS, RTXPTRealtimeResetFlags RHS)
{
    return static_cast<RTXPTRealtimeResetFlags>(static_cast<Uint32>(LHS) & static_cast<Uint32>(RHS));
}

inline RTXPTRealtimeResetFlags& operator|=(RTXPTRealtimeResetFlags& LHS, RTXPTRealtimeResetFlags RHS)
{
    LHS = LHS | RHS;
    return LHS;
}

inline bool HasRealtimeResetFlag(RTXPTRealtimeResetFlags Flags, RTXPTRealtimeResetFlags Test)
{
    return (static_cast<Uint32>(Flags & Test) != 0u);
}

struct RTXPTNrdReblurUiSettings
{
    bool                                  EnableAntiFirefly             = true;
    RTXPTNrdHitDistanceReconstructionMode HitDistanceReconstructionMode = RTXPTNrdHitDistanceReconstructionMode::Area5x5;
    Uint32                                MaxAccumulatedFrameNum        = 50;
    Uint32                                MaxFastAccumulatedFrameNum    = 6;
    Uint32                                HistoryFixFrameNum            = 3;
    float                                 DiffusePrepassBlurRadius      = 15.0f;
    float                                 SpecularPrepassBlurRadius     = 40.0f;
};

struct RTXPTNrdRelaxUiSettings
{
    bool                                  EnableAntiFirefly                  = true;
    RTXPTNrdHitDistanceReconstructionMode HitDistanceReconstructionMode      = RTXPTNrdHitDistanceReconstructionMode::Off;
    float                                 DiffusePrepassBlurRadius           = 0.0f;
    float                                 SpecularPrepassBlurRadius          = 0.0f;
    Uint32                                DiffuseMaxAccumulatedFrameNum      = 25;
    Uint32                                SpecularMaxAccumulatedFrameNum     = 40;
    Uint32                                DiffuseMaxFastAccumulatedFrameNum  = 5;
    Uint32                                SpecularMaxFastAccumulatedFrameNum = 6;
    Uint32                                HistoryFixFrameNum                 = 3;
    Uint32                                AtrousIterationNum                 = 5;
    float                                 LobeAngleFraction                  = 0.7f;
    float                                 SpecularLobeAngleSlack             = 0.2f;
    float                                 DepthThreshold                     = 0.004f;
    float                                 AntilagAccelerationAmount          = 0.55f;
    float                                 AntilagSpatialSigmaScale           = 2.5f;
    float                                 AntilagTemporalSigmaScale          = 0.3f;
    float                                 AntilagResetAmount                 = 0.5f;
};

struct RTXPTRealtimeSettings
{
    bool                RealtimeMode            = true;
    Int32               RealtimeSamplesPerPixel = 1;
    RTXPTRealtimeAAMode RealtimeAA              = RTXPTRealtimeAAMode::Disabled;
    bool                StandaloneDenoiser      = true;

    // Super resolution provider/quality selection (active when RealtimeAA == SuperResolution).
    // SuperResolutionVariantIdx indexes the full provider list reported by the pass; the
    // realtime path only consumes temporal variants and falls back to the first temporal one.
    Int32                       SuperResolutionVariantIdx = 0;
    RTXPTSuperResolutionQuality SuperResolutionQuality    = RTXPTSuperResolutionQuality::Balanced;
    float                       SuperResolutionSharpness  = 0.0f;

    bool  RealtimeFireflyFilterEnabled   = true;
    float RealtimeFireflyFilterThreshold = 0.10f;
    float TexLODBias                     = -1.0f;

    Int32 StablePlanesActiveCount                      = static_cast<Int32>(kRTXPTStablePlaneCount);
    Int32 StablePlanesMaxVertexDepth                   = 9;
    bool  AllowPrimarySurfaceReplacement               = true;
    float StablePlanesSplitStopThreshold               = 0.95f;
    bool  StablePlanesSuppressPrimaryIndirectSpecular  = true;
    float StablePlanesSuppressPrimaryIndirectSpecularK = 0.6f;
    float StablePlanesAntiAliasingFallthrough          = 0.6f;

    float DenoiserRadianceClampK = 8.0f;

    RTXPTDenoisingGuideDebugView DenoisingGuideDebugView = RTXPTDenoisingGuideDebugView::Disabled;

    RTXPTNrdMethod           NRDMethod                               = RTXPTNrdMethod::REBLUR;
    float                    NRDDisocclusionThreshold                = 0.03f;
    bool                     NRDUseAlternateDisocclusionThresholdMix = true;
    float                    NRDDisocclusionThresholdAlternate       = 0.2f;
    RTXPTNrdReblurUiSettings ReblurSettings;
    RTXPTNrdRelaxUiSettings  RelaxSettings;

    float DLSSRRBrightnessClampK = 4096.0f;
    float DLSSRRMicroJitter      = 0.1f;

    bool ActualUseStandaloneDenoiser() const
    {
        return RealtimeMode &&
            static_cast<Uint32>(RealtimeAA) < static_cast<Uint32>(RTXPTRealtimeAAMode::DLSSRR) &&
            StandaloneDenoiser;
    }

    Uint32 ActualSamplesPerPixel() const
    {
        return RealtimeMode ? static_cast<Uint32>(std::max(RealtimeSamplesPerPixel, 1)) : 1u;
    }
};

inline void SanitizeRealtimeSettings(RTXPTRealtimeSettings& Settings)
{
    Settings.RealtimeSamplesPerPixel =
        std::clamp(Settings.RealtimeSamplesPerPixel, Int32{1}, static_cast<Int32>(kRTXPTRealtimeSamplesPerPixelMax));

    const Uint32 AAMode = std::clamp(static_cast<Uint32>(Settings.RealtimeAA),
                                     static_cast<Uint32>(RTXPTRealtimeAAMode::Disabled),
                                     static_cast<Uint32>(RTXPTRealtimeAAMode::DLSSRR));
    Settings.RealtimeAA = static_cast<RTXPTRealtimeAAMode>(AAMode);

    // The upper bound of the variant index depends on the live provider count, which is
    // enforced at runtime by the super resolution pass; here we only reject negatives.
    Settings.SuperResolutionVariantIdx  = std::max(Settings.SuperResolutionVariantIdx, Int32{0});
    const Uint32 SuperResolutionQuality = std::clamp(static_cast<Uint32>(Settings.SuperResolutionQuality),
                                                     0u,
                                                     kRTXPTSuperResolutionQualityCount - 1u);
    Settings.SuperResolutionQuality     = static_cast<RTXPTSuperResolutionQuality>(SuperResolutionQuality);
    Settings.SuperResolutionSharpness   = std::clamp(Settings.SuperResolutionSharpness, 0.0f, 1.0f);

    const Uint32 GuideDebugView      = std::clamp(static_cast<Uint32>(Settings.DenoisingGuideDebugView),
                                             static_cast<Uint32>(RTXPTDenoisingGuideDebugView::Disabled),
                                             static_cast<Uint32>(RTXPTDenoisingGuideDebugView::PrimaryLayer));
    Settings.DenoisingGuideDebugView = static_cast<RTXPTDenoisingGuideDebugView>(GuideDebugView);

    Settings.RealtimeFireflyFilterThreshold = std::clamp(Settings.RealtimeFireflyFilterThreshold, 0.00001f, 1000.0f);
    Settings.StablePlanesActiveCount =
        std::clamp(Settings.StablePlanesActiveCount, Int32{1}, static_cast<Int32>(kRTXPTStablePlaneCount));
    Settings.StablePlanesMaxVertexDepth =
        std::clamp(Settings.StablePlanesMaxVertexDepth, Int32{2}, static_cast<Int32>(kRTXPTStablePlaneMaxVertexIndex));
    Settings.StablePlanesSplitStopThreshold = std::clamp(Settings.StablePlanesSplitStopThreshold, 0.0f, 2.0f);
    Settings.StablePlanesSuppressPrimaryIndirectSpecularK =
        std::clamp(Settings.StablePlanesSuppressPrimaryIndirectSpecularK, 0.0f, 1.0f);
    Settings.StablePlanesAntiAliasingFallthrough =
        std::clamp(Settings.StablePlanesAntiAliasingFallthrough, 0.0f, 1.0f);

    Settings.DenoiserRadianceClampK            = std::max(Settings.DenoiserRadianceClampK, 0.0f);
    Settings.NRDDisocclusionThreshold          = std::max(Settings.NRDDisocclusionThreshold, 0.0f);
    Settings.NRDDisocclusionThresholdAlternate = std::max(Settings.NRDDisocclusionThresholdAlternate, 0.0f);
    Settings.DLSSRRBrightnessClampK            = std::max(Settings.DLSSRRBrightnessClampK, 0.0f);
    Settings.DLSSRRMicroJitter                 = std::clamp(Settings.DLSSRRMicroJitter, 0.0f, 1.0f);

    Settings.ReblurSettings.MaxAccumulatedFrameNum     = std::min(Settings.ReblurSettings.MaxAccumulatedFrameNum, 500u);
    Settings.ReblurSettings.MaxFastAccumulatedFrameNum = std::min(Settings.ReblurSettings.MaxFastAccumulatedFrameNum, 500u);
    Settings.ReblurSettings.HistoryFixFrameNum         = std::min(Settings.ReblurSettings.HistoryFixFrameNum, 500u);
    Settings.ReblurSettings.DiffusePrepassBlurRadius   = std::clamp(Settings.ReblurSettings.DiffusePrepassBlurRadius, 0.0f, 100.0f);
    Settings.ReblurSettings.SpecularPrepassBlurRadius  = std::clamp(Settings.ReblurSettings.SpecularPrepassBlurRadius, 0.0f, 100.0f);

    Settings.RelaxSettings.DiffusePrepassBlurRadius  = std::clamp(Settings.RelaxSettings.DiffusePrepassBlurRadius, 0.0f, 100.0f);
    Settings.RelaxSettings.SpecularPrepassBlurRadius = std::clamp(Settings.RelaxSettings.SpecularPrepassBlurRadius, 0.0f, 100.0f);
    Settings.RelaxSettings.AtrousIterationNum        = std::clamp(Settings.RelaxSettings.AtrousIterationNum, 2u, 8u);
    Settings.RelaxSettings.LobeAngleFraction         = std::clamp(Settings.RelaxSettings.LobeAngleFraction, 0.0f, 1.0f);
    Settings.RelaxSettings.SpecularLobeAngleSlack    = std::clamp(Settings.RelaxSettings.SpecularLobeAngleSlack, 0.0f, 1.0f);
    Settings.RelaxSettings.DepthThreshold            = std::clamp(Settings.RelaxSettings.DepthThreshold, 0.0f, 0.1f);
    Settings.RelaxSettings.AntilagAccelerationAmount = std::clamp(Settings.RelaxSettings.AntilagAccelerationAmount, 0.0f, 1.0f);
    Settings.RelaxSettings.AntilagSpatialSigmaScale  = std::clamp(Settings.RelaxSettings.AntilagSpatialSigmaScale, 0.0f, 5.0f);
    Settings.RelaxSettings.AntilagTemporalSigmaScale = std::clamp(Settings.RelaxSettings.AntilagTemporalSigmaScale, 0.0f, 5.0f);
    Settings.RelaxSettings.AntilagResetAmount        = std::clamp(Settings.RelaxSettings.AntilagResetAmount, 0.0f, 1.0f);
}

} // namespace Diligent
