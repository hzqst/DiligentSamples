/* Copyright 2026 Diligent Graphics LLC
 * Licensed under the Apache License, Version 2.0.
 */
#include "RTXPTRayReconstructionPass.hpp"

#include <vector>
#include "DebugUtilities.hpp"
#if RTXPT_HAS_RAY_RECONSTRUCTION
#    include "RayReconstructionFactoryLoader.h"
#endif

namespace Diligent
{

void RTXPTRayReconstructionPass::ReleaseReconstruction()
{
    if (m_Reconstruction && m_Context)
    {
        // Native NGX owns resources referenced by submitted and pending commands.
        m_Context->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
        m_Context->Flush();
        m_Context->WaitForIdle();
    }
    m_Reconstruction.Release();
    m_ResetHistory      = true;
    m_Stats.LastExecute = false;
}

void RTXPTRayReconstructionPass::Reset()
{
    ReleaseReconstruction();
    m_Context.Release();
    m_Factory.Release();
    m_Stats  = {};
    m_Desc   = {};
    m_Failed = false;
}

void RTXPTRayReconstructionPass::Initialize(IRenderDevice* pDevice)
{
    Reset();
#if RTXPT_HAS_RAY_RECONSTRUCTION
    LoadAndCreateRayReconstructionFactory(pDevice, &m_Factory);
    Uint32 Count = 0;
    if (m_Factory)
        m_Factory->EnumerateVariants(Count, nullptr);
    std::vector<RayReconstructionInfo> Variants(Count);
    if (Count != 0)
        m_Factory->EnumerateVariants(Count, Variants.data());
    for (Uint32 Index = 0; Index < Count; ++Index)
        m_Stats.Available |= Variants[Index].VariantId == RayReconstructionVariant_DLSS;
    if (!m_Stats.Available)
    {
        Fail(RTXPTRayReconstructionStatus::Unavailable,
             "DLSS-RR unavailable: check device/driver support, DLSSD runtime deployment and Vulkan RR extensions.");
        return;
    }
    m_Stats.Status  = RTXPTRayReconstructionStatus::Available;
    m_Stats.Message = "DLSS-RR available";
    LOG_INFO_MESSAGE(m_Stats.Message);
#else
    (void)pDevice;
    m_Stats.Message = "DLSS-RR disabled in this build";
    LOG_INFO_MESSAGE(m_Stats.Message);
#endif
}

void RTXPTRayReconstructionPass::Deactivate()
{
    ReleaseReconstruction();
    m_Failed = false;
    if (m_Stats.Available)
    {
        m_Stats.Status  = RTXPTRayReconstructionStatus::Available;
        m_Stats.Message = "DLSS-RR available";
    }
}

bool RTXPTRayReconstructionPass::Fail(RTXPTRayReconstructionStatus Status, const char* Message)
{
    m_Stats.Status      = Status;
    m_Stats.LastExecute = false;
    m_Stats.Message     = Message;
    m_ResetHistory      = true;
    LOG_WARNING_MESSAGE(Message);
    return false;
}

void RTXPTRayReconstructionPass::NotifyInputPreparationFailed()
{
    Fail(RTXPTRayReconstructionStatus::ExecuteFailed, "DLSS-RR input preparation failed; output discarded.");
}

bool RTXPTRayReconstructionPass::PrepareFrame(IDeviceContext*                       pContext,
                                              const RTXPTRayReconstructionSettings& Settings,
                                              Uint32                                Width,
                                              Uint32                                Height,
                                              Uint32                                FrameIndex)
{
    m_Stats.LastExecute = false;
    if (!m_Stats.Available || pContext == nullptr || Width == 0 || Height == 0)
        return false;
    m_Context          = pContext;
    const bool Changed = m_Desc.OutputWidth != Width || m_Desc.OutputHeight != Height ||
        m_Desc.Quality != Settings.Quality || m_Desc.DLSSPreset != Settings.Preset;
    if (Changed)
    {
        ReleaseReconstruction();
        m_Failed = false;
    }
    if (m_Failed)
        return false;
    if (!m_Reconstruction)
    {
        RayReconstructionSourceSettingsAttribs Query;
        Query.OutputWidth  = Width;
        Query.OutputHeight = Height;
        Query.Quality      = Settings.Quality;
        RayReconstructionSourceSettings Source;
        m_Desc.OutputWidth  = Width;
        m_Desc.OutputHeight = Height;
        m_Desc.Quality      = Settings.Quality;
        m_Desc.DLSSPreset   = Settings.Preset;
        RayReconstructionDesc Desc;
        if (!m_Factory->GetSourceSettings(Query, Source) || !RTXPTRayReconstructionInputs::MakeDesc(Query, Source, Desc))
        {
            m_Failed = true;
            return Fail(RTXPTRayReconstructionStatus::SourceQueryFailed, "DLSS-RR source-size query failed; change quality or retry.");
        }
        Desc.DLSSPreset = Settings.Preset;
        m_Desc          = Desc;
        m_Factory->CreateRayReconstruction(m_Desc, &m_Reconstruction);
        if (!m_Reconstruction)
        {
            m_Failed = true;
            return Fail(RTXPTRayReconstructionStatus::CreationFailed, "DLSS-RR creation failed; see NGX log, change configuration or retry.");
        }
        ++m_Stats.CreateCount;
        LOG_INFO_MESSAGE("DLSS-RR created: ", Desc.InputWidth, "x", Desc.InputHeight, " -> ", Width, "x", Height,
                         ", preset=", static_cast<Uint32>(Settings.Preset));
    }
    m_Reconstruction->GetJitterOffset(FrameIndex, m_Jitter.x, m_Jitter.y);
    m_Stats.Status  = RTXPTRayReconstructionStatus::Ready;
    m_Stats.Message = "DLSS-RR ready";
    return true;
}

bool RTXPTRayReconstructionPass::Execute(IDeviceContext* pContext, const RTXPTRenderTargets& Targets, const SampleConstants& Frame, bool ResetHistory)
{
    m_Stats.LastExecute = false;
    if (!m_Reconstruction || m_Failed)
        return false;
    ExecuteRayReconstructionAttribs Attribs;
    if (!RTXPTRayReconstructionInputs::MakeExecuteAttribs(pContext, Targets, m_Desc, Frame,
                                                          ResetHistory || m_ResetHistory, Attribs))
        return Fail(RTXPTRayReconstructionStatus::ExecuteFailed, "DLSS-RR frame input contract failed; output discarded.");
    pContext->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
    if (!m_Reconstruction->Execute(Attribs))
    {
        m_Failed = true;
        return Fail(RTXPTRayReconstructionStatus::ExecuteFailed, "DLSS-RR Execute failed; output discarded. Change configuration or retry.");
    }
    m_Stats.LastExecute = true;
    ++m_Stats.ExecuteCount;
    if (Attribs.ResetHistory)
        ++m_Stats.ResetCount;
    m_ResetHistory = false;
    return true;
}

} // namespace Diligent
