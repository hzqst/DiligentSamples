// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <string>
#include "BasicMath.hpp"
#include "RefCntAutoPtr.hpp"
#include "SuperResolutionFactory.h"

namespace Diligent
{
enum class RTXPTErrorCode : Uint32;

struct RTXPTViewSize
{
    Uint32 Width  = 0;
    Uint32 Height = 0;
};

//Each quality is queried exactly; an unsupported one is never substituted. Native runs temporal AA at display size.
enum class RTXPTUpscaleQuality
{
    Native,
    Quality,
    Balanced,
    Performance,
};

enum class RTXPTSuperResolutionProvider
{
    Auto,
    DLSS,
    DirectSR,
    FSR,
};

const char* GetRTXPTSuperResolutionProviderString(RTXPTSuperResolutionProvider Provider);

//The feature of one view, independent of the renderer supplying its color, depth and motion.
struct RTXPTViewSuperResolutionInfo
{
    std::string                  Variant;
    RTXPTUpscaleQuality          Quality        = RTXPTUpscaleQuality::Quality;
    RTXPTSuperResolutionProvider Provider       = RTXPTSuperResolutionProvider::Auto;
    SUPER_RESOLUTION_PROVIDER    ActualProvider = SUPER_RESOLUTION_PROVIDER_UNKNOWN;
    RTXPTViewSize                RenderSize;
    RTXPTViewSize                DisplaySize;
    //The jitter and reset of the last committed frame. Only a committed frame advances the jitter sequence.
    float2 Jitter    = float2{0, 0};
    bool   LastReset = false;
    Uint32 Executes  = 0;
    Uint32 Resets    = 0;
    Uint32 Failures  = 0;
};

struct RTXPTViewUpscalerCreateInfo
{
    ISuperResolutionFactory*     Factory = nullptr;
    RTXPTViewSize                DisplaySize;
    RTXPTUpscaleQuality          Quality     = RTXPTUpscaleQuality::Quality;
    RTXPTSuperResolutionProvider Provider    = RTXPTSuperResolutionProvider::Auto;
    TEXTURE_FORMAT               DepthFormat = TEX_FORMAT_R32_FLOAT;
};

//Owns the factory, feature, query and successful-frame jitter sequence of a view. Producers supply scene-linear HDR
//and aligned inputs; the display transform follows Execute. CommitFrame accepts the whole producer/display frame.
//InvalidateFrame leaves the sequence in place and forces a reset because provider history cannot be rolled back.
class RTXPTViewUpscaler
{
public:
    RTXPTViewUpscaler()                         = default;
    RTXPTViewUpscaler(const RTXPTViewUpscaler&) = delete;
    RTXPTViewUpscaler& operator=(const RTXPTViewUpscaler&) = delete;
    RTXPTViewUpscaler(RTXPTViewUpscaler&&)                 = default;
    RTXPTViewUpscaler& operator=(RTXPTViewUpscaler&&) = default;
    RTXPTErrorCode     Initialize(const RTXPTViewUpscalerCreateInfo& CreateInfo);
    //Retries object creation with the already selected variant and exact query, without reevaluating Auto.
    RTXPTErrorCode                      RetryCreate();
    bool                                HasSelection() const { return m_HasSelection; }
    RTXPTErrorCode                      Execute(const ExecuteSuperResolutionAttribs& Attribs, bool InvalidSurfaceHistory = false);
    void                                CommitFrame();
    void                                InvalidateFrame();
    void                                RecordInputFailure();
    bool                                IsReady() const { return m_Feature != nullptr; }
    bool                                IsTemporal() const { return m_Type == SUPER_RESOLUTION_TYPE_TEMPORAL; }
    bool                                SupportsIgnoreHistoryMask() const { return (m_CapFlags & SUPER_RESOLUTION_TEMPORAL_CAP_FLAG_IGNORE_HISTORY_MASK) != 0; }
    TEXTURE_VIEW_TYPE                   GetOutputViewType() const { return IsTemporal() ? TEXTURE_VIEW_UNORDERED_ACCESS : TEXTURE_VIEW_RENDER_TARGET; }
    float2                              GetJitter() const;
    const SuperResolutionDesc&          GetDesc() const { return m_Desc; }
    const RTXPTViewSuperResolutionInfo& GetInfo() const { return m_Info; }

    //Enforces the queried range before creating a feature; native providers do not necessarily reject an invalid size.
    static bool MakeDesc(const SuperResolutionSourceSettingsAttribs& Query,
                         const SuperResolutionSourceSettings&        Source,
                         TEXTURE_FORMAT                              DepthFormat,
                         SuperResolutionDesc&                        Desc)
    {
        Desc = {};
        if (!Query.OutputWidth || !Query.OutputHeight || Query.OptimizationType >= SUPER_RESOLUTION_OPTIMIZATION_TYPE_COUNT ||
            Query.Flags != SUPER_RESOLUTION_FLAG_NONE || Query.OutputFormat != TEX_FORMAT_RGBA16_FLOAT ||
            !Source.OptimalInputWidth || !Source.OptimalInputHeight ||
            Source.OptimalInputWidth > Query.OutputWidth || Source.OptimalInputHeight > Query.OutputHeight ||
            Source.OptimalInputWidth < Source.MinInputWidth || Source.OptimalInputWidth > Source.MaxInputWidth ||
            Source.OptimalInputHeight < Source.MinInputHeight || Source.OptimalInputHeight > Source.MaxInputHeight)
            return false;
        Desc.Name             = "RTXPT view super resolution";
        Desc.VariantId        = Query.VariantId;
        Desc.InputWidth       = Source.OptimalInputWidth;
        Desc.InputHeight      = Source.OptimalInputHeight;
        Desc.OutputWidth      = Query.OutputWidth;
        Desc.OutputHeight     = Query.OutputHeight;
        Desc.ColorFormat      = TEX_FORMAT_RGBA16_FLOAT;
        Desc.OutputFormat     = TEX_FORMAT_RGBA16_FLOAT;
        Desc.DepthFormat      = DepthFormat;
        Desc.MotionFormat     = TEX_FORMAT_RG16_FLOAT;
        Desc.ExposureFormat   = TEX_FORMAT_R16_FLOAT;
        Desc.OptimizationType = Query.OptimizationType;
        return true;
    }

private:
    RTXPTErrorCode                         InitializeFeature(const RTXPTViewUpscalerCreateInfo& CreateInfo);
    SuperResolutionDesc                    m_Desc;
    bool                                   m_HasSelection = false;
    RefCntAutoPtr<ISuperResolutionFactory> m_Factory;
    RefCntAutoPtr<ISuperResolution>        m_Feature;
    RTXPTViewSuperResolutionInfo           m_Info;
    Uint32                                 m_FrameIndex = 0;
    float2                                 m_PendingJitter{0, 0};
    bool                                   m_PendingReset = false;
    bool                                   m_Pending      = false;
    bool                                   m_RequireReset = true;
    SUPER_RESOLUTION_TYPE                  m_Type         = SUPER_RESOLUTION_TYPE_SPATIAL;
    SUPER_RESOLUTION_TEMPORAL_CAP_FLAGS    m_CapFlags     = SUPER_RESOLUTION_TEMPORAL_CAP_FLAG_NONE;
};

//A producer that completes its display frame accepts the pending execution; every earlier return invalidates it.
class RTXPTViewUpscalerFrame
{
public:
    explicit RTXPTViewUpscalerFrame(RTXPTViewUpscaler* Upscaler) :
        m_Upscaler{Upscaler} {}
    ~RTXPTViewUpscalerFrame()
    {
        if (m_Upscaler)
            m_Upscaler->InvalidateFrame();
    }
    void Succeed(bool Commit = true)
    {
        if (m_Upscaler && Commit)
            m_Upscaler->CommitFrame();
        m_Upscaler = nullptr;
    }

private:
    RTXPTViewUpscaler* m_Upscaler;
};
} // namespace Diligent
