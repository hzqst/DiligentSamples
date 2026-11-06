// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#include "RTXPTViewUpscaler.hpp"
#include "RTXPTSceneRuntime.hpp"
#include <algorithm>
#include <utility>
#include <vector>

namespace Diligent
{
namespace
{
SUPER_RESOLUTION_OPTIMIZATION_TYPE ToUpscalerOptimizationType(RTXPTUpscaleQuality Quality)
{
    switch (Quality)
    {
        case RTXPTUpscaleQuality::Native: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_NATIVE;
        case RTXPTUpscaleQuality::Quality: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_QUALITY;
        case RTXPTUpscaleQuality::Balanced: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_BALANCED;
        case RTXPTUpscaleQuality::Performance: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_PERFORMANCE;
        default: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_COUNT;
    }
}
} // namespace

const char* GetRTXPTSuperResolutionProviderString(RTXPTSuperResolutionProvider Provider)
{
    switch (Provider)
    {
        case RTXPTSuperResolutionProvider::Auto: return "Auto";
        case RTXPTSuperResolutionProvider::DLSS: return "DLSS";
        case RTXPTSuperResolutionProvider::DirectSR: return "DirectSR";
        case RTXPTSuperResolutionProvider::FSR: return "FSR";
        default: return "unknown";
    }
}

RTXPTErrorCode RTXPTViewUpscaler::Initialize(const RTXPTViewUpscalerCreateInfo& CreateInfo)
{
    const auto Code = InitializeFeature(CreateInfo);
    if (Code != RTXPTErrorCode::Ok)
        ++m_Info.Failures;
    return Code;
}

RTXPTErrorCode RTXPTViewUpscaler::InitializeFeature(const RTXPTViewUpscalerCreateInfo& CreateInfo)
{
    m_Feature.Release();
    m_Factory          = CreateInfo.Factory;
    m_Info             = {};
    m_Info.Provider    = CreateInfo.Provider;
    m_Info.Quality     = CreateInfo.Quality;
    m_Info.DisplaySize = CreateInfo.DisplaySize;
    m_FrameIndex       = 0;
    m_Pending          = false;
    m_RequireReset     = true;
    m_Type             = SUPER_RESOLUTION_TYPE_SPATIAL;
    m_CapFlags         = SUPER_RESOLUTION_TEMPORAL_CAP_FLAG_NONE;
    m_HasSelection     = false;
    m_Desc             = {};
    if (!m_Factory)
        return RTXPTErrorCode::SuperResolutionFactoryUnavailable;
    const auto Optimization = ToUpscalerOptimizationType(CreateInfo.Quality);
    if (Optimization == SUPER_RESOLUTION_OPTIMIZATION_TYPE_COUNT)
        return RTXPTErrorCode::SuperResolutionQueryFailed;
    if (CreateInfo.Provider < RTXPTSuperResolutionProvider::Auto || CreateInfo.Provider > RTXPTSuperResolutionProvider::FSR)
        return RTXPTErrorCode::InvalidArgument;
    Uint32 Count = 0;
    m_Factory->EnumerateVariants(Count, nullptr);
    std::vector<SuperResolutionInfo> Variants(Count);
    if (Count)
        m_Factory->EnumerateVariants(Count, Variants.data());
    Variants.resize(Count);
    const std::pair<RTXPTSuperResolutionProvider, SUPER_RESOLUTION_PROVIDER> Providers[] = {
        {RTXPTSuperResolutionProvider::DLSS, SUPER_RESOLUTION_PROVIDER_DLSS},
        {RTXPTSuperResolutionProvider::DirectSR, SUPER_RESOLUTION_PROVIDER_DIRECTSR},
        {RTXPTSuperResolutionProvider::FSR, SUPER_RESOLUTION_PROVIDER_FSR},
    };
    for (const auto& [Requested, Provider] : Providers)
    {
        if (CreateInfo.Provider != RTXPTSuperResolutionProvider::Auto && CreateInfo.Provider != Requested)
            continue;
        //DirectSR can enumerate several variants. The first temporal one is the initial policy, even if its query fails.
        const auto Found = std::find_if(Variants.begin(), Variants.end(), [Provider](const auto& Variant) {
            return Variant.Provider == Provider && Variant.Type == (Provider == SUPER_RESOLUTION_PROVIDER_FSR ? SUPER_RESOLUTION_TYPE_SPATIAL : SUPER_RESOLUTION_TYPE_TEMPORAL);
        });
        if (Found == Variants.end())
            continue;
        const auto&                          Variant = *Found;
        SuperResolutionSourceSettingsAttribs Query;
        Query.VariantId        = Variant.VariantId;
        Query.OutputWidth      = CreateInfo.DisplaySize.Width;
        Query.OutputHeight     = CreateInfo.DisplaySize.Height;
        Query.OutputFormat     = TEX_FORMAT_RGBA16_FLOAT;
        Query.OptimizationType = Optimization;
        SuperResolutionSourceSettings Source;
        SuperResolutionDesc           Desc;
        if (m_Factory->GetSourceSettings(Query, Source) != SUPER_RESOLUTION_SOURCE_SETTINGS_STATUS_SUCCESS ||
            !MakeDesc(Query, Source, CreateInfo.DepthFormat, Desc))
        {
            if (CreateInfo.Provider == RTXPTSuperResolutionProvider::Auto)
                continue;
            return RTXPTErrorCode::SuperResolutionQueryFailed;
        }
        m_Type     = Variant.Type;
        m_CapFlags = IsTemporal() ? Variant.TemporalCapFlags : SUPER_RESOLUTION_TEMPORAL_CAP_FLAG_NONE;
        if (!IsTemporal())
        {
            Desc.DepthFormat    = TEX_FORMAT_UNKNOWN;
            Desc.MotionFormat   = TEX_FORMAT_UNKNOWN;
            Desc.ExposureFormat = TEX_FORMAT_UNKNOWN;
        }
        m_Info.Variant        = Variant.Name;
        m_Info.ActualProvider = Provider;
        m_Info.RenderSize     = {Desc.InputWidth, Desc.InputHeight};
        m_Info.DisplaySize    = CreateInfo.DisplaySize;
        m_Desc                = Desc;
        m_HasSelection        = true;
        m_Factory->CreateSuperResolution(m_Desc, &m_Feature);
        if (!m_Feature)
            return RTXPTErrorCode::SuperResolutionCreateFailed;
        return RTXPTErrorCode::Ok;
    }
    switch (CreateInfo.Provider)
    {
        case RTXPTSuperResolutionProvider::Auto: return RTXPTErrorCode::SuperResolutionQueryFailed;
        case RTXPTSuperResolutionProvider::DLSS: return RTXPTErrorCode::SuperResolutionUnavailable;
        case RTXPTSuperResolutionProvider::DirectSR: return RTXPTErrorCode::DirectSRUnavailable;
        case RTXPTSuperResolutionProvider::FSR: return RTXPTErrorCode::FSRUnavailable;
        default: return RTXPTErrorCode::InvalidArgument;
    }
}

RTXPTErrorCode RTXPTViewUpscaler::RetryCreate()
{
    if (m_Feature)
        return RTXPTErrorCode::Ok;
    if (!m_HasSelection)
        return RTXPTErrorCode::SuperResolutionQueryFailed;
    m_Factory->CreateSuperResolution(m_Desc, &m_Feature);
    if (!m_Feature)
    {
        RecordInputFailure();
        return RTXPTErrorCode::SuperResolutionCreateFailed;
    }
    return RTXPTErrorCode::Ok;
}

float2 RTXPTViewUpscaler::GetJitter() const
{
    float2 Jitter{0, 0};
    if (m_Feature && IsTemporal())
        m_Feature->GetJitterOffset(m_FrameIndex, Jitter.x, Jitter.y);
    return Jitter;
}

RTXPTErrorCode RTXPTViewUpscaler::Execute(const ExecuteSuperResolutionAttribs& Attribs, bool InvalidSurfaceHistory)
{
    if (!m_Feature)
        return RTXPTErrorCode::SuperResolutionUnavailable;
    auto Execute = Attribs;
    if (!IsTemporal())
    {
        Execute.JitterX = Execute.JitterY    = 0;
        Execute.pDepthTextureSRV             = nullptr;
        Execute.pMotionVectorsSRV            = nullptr;
        Execute.pIgnoreHistoryMaskTextureSRV = nullptr;
    }
    else if (!(m_CapFlags & SUPER_RESOLUTION_TEMPORAL_CAP_FLAG_IGNORE_HISTORY_MASK))
        Execute.pIgnoreHistoryMaskTextureSRV = nullptr;
    if (m_Feature->GetDesc().ExposureFormat == TEX_FORMAT_UNKNOWN)
        Execute.pExposureTextureSRV = nullptr;
    Execute.ResetHistory = Execute.ResetHistory || m_RequireReset;
    if (IsTemporal() && !SupportsIgnoreHistoryMask() && InvalidSurfaceHistory)
        Execute.ResetHistory = True;
    if (!m_Feature->Execute(Execute))
    {
        ++m_Info.Failures;
        InvalidateFrame();
        //DLSS creates the native feature on its first Execute.
        return !m_Info.Executes && m_Info.ActualProvider == SUPER_RESOLUTION_PROVIDER_DLSS ? RTXPTErrorCode::SuperResolutionCreateFailed : RTXPTErrorCode::SuperResolutionExecuteFailed;
    }
    ++m_Info.Executes;
    m_Info.Resets += Execute.ResetHistory ? 1u : 0u;
    m_PendingJitter = {Execute.JitterX, Execute.JitterY};
    m_PendingReset  = Execute.ResetHistory;
    m_Pending       = true;
    return RTXPTErrorCode::Ok;
}

void RTXPTViewUpscaler::CommitFrame()
{
    if (!m_Pending)
        return;
    m_Info.Jitter    = m_PendingJitter;
    m_Info.LastReset = m_PendingReset;
    ++m_FrameIndex;
    m_Pending      = false;
    m_RequireReset = false;
}

void RTXPTViewUpscaler::InvalidateFrame()
{
    m_Pending      = false;
    m_RequireReset = true;
}

void RTXPTViewUpscaler::RecordInputFailure()
{
    ++m_Info.Failures;
    InvalidateFrame();
}
} // namespace Diligent
