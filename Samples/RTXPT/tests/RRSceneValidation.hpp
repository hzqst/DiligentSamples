/* Copyright 2026 Diligent Graphics LLC
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

// Included by RTXPTSample.cpp after the class definition. This opt-in sequence
// exercises the production scene, frame scheduler and post-processing passes.
void RTXPTSample::RunRayReconstructionValidation(double CurrTime)
{
    if (!m_ValidateRayReconstruction)
        return;
    constexpr Uint32 FramesPerStage = 32;
    constexpr Uint32 StageCount     = 14;
    const Uint32     Frame          = m_RayReconstructionValidationFrame++;
    auto&            RR             = m_PostProcessPipeline.GetRayReconstructionPass();
    if (Frame != 0 && Frame <= StageCount * FramesPerStage)
    {
        if (!m_LastFrameSucceeded || (m_RealtimeUI.RealtimeMode && m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR && !RR.GetStats().LastExecute))
        {
            ++m_RayReconstructionValidationFailures;
            LOG_ERROR_MESSAGE("RR scene validation failed at frame ", Frame - 1, ": ", m_RealtimePathTraceStatus);
        }
    }
    if (Frame != 0 && Frame <= StageCount * FramesPerStage && Frame % FramesPerStage == 0)
    {
        // Smooth motion and environment table changes must retain light feedback,
        // even when image-denoiser history is invalidated independently.
        if ((Frame == 2 * FramesPerStage || Frame >= 12 * FramesPerStage) &&
            m_LightsBaker.GetFeedbackHistoryFrameCount() <= FramesPerStage)
        {
            ++m_RayReconstructionValidationFailures;
            LOG_ERROR_MESSAGE("Light feedback history was reset during camera/environment changes at frame ", Frame);
        }
        LOG_INFO_MESSAGE("RR validation stage ", Frame / FramesPerStage - 1, " wall time: ",
                         (CurrTime - m_RayReconstructionValidationStageStart) * 1000.0 / FramesPerStage,
                         " ms/frame (includes stage setup)");
    }
    if (Frame == StageCount * FramesPerStage)
    {
        LOG_INFO_MESSAGE("RR scene validation finished: failures=", m_RayReconstructionValidationFailures,
                         ", RR executions=", RR.GetStats().ExecuteCount, ", creates=", RR.GetStats().CreateCount,
                         ", history resets=", RR.GetStats().ResetCount);
        std::fflush(stdout);
        return;
    }
    if (Frame >= StageCount * FramesPerStage)
        return;
    const Uint32 Stage = Frame / FramesPerStage;
    if (Stage == 1)
    {
        constexpr float CameraStep = 0.0025f; // Keep the convergence-test camera inside the room.
        m_Camera.SetPos(m_Camera.GetPos() + float3{CameraStep, 0.0f, 0.0f});
    }
    if (Frame % FramesPerStage != 0)
        return;
    LOG_INFO_MESSAGE("RR scene validation stage ", Stage, " starts at frame ", Frame);
    m_RayReconstructionValidationStageStart = CurrTime;
    std::fflush(stdout);
    switch (Stage)
    {
        case 0:
            m_RealtimeUI.RealtimeMode           = true;
            m_RealtimeUI.RealtimeAA             = RTXPTRealtimeAAMode::DLSSRR;
            m_RayReconstructionSettings.Quality = RAY_RECONSTRUCTION_QUALITY_QUALITY;
            break;
        case 1: // Smooth camera motion retains the temporal history.
            return;
        case 2: // Explicit camera/scene history invalidation.
            RequestRealtimeCachesReset("RR validation history reset");
            break;
        case 3:
            m_RayReconstructionSettings.Quality = RAY_RECONSTRUCTION_QUALITY_NATIVE;
            break;
        case 4:
        {
#if PLATFORM_WIN32
            // Drive WM_SIZE through the real window/framework path so ImGui,
            // swap chain and sample agree on the new render surface dimensions.
            HWND Window = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
            RECT Rect   = {0, 0, 768, 432};
            if (Window == nullptr ||
                !AdjustWindowRectEx(&Rect, static_cast<DWORD>(GetWindowLongPtr(Window, GWL_STYLE)), FALSE,
                                    static_cast<DWORD>(GetWindowLongPtr(Window, GWL_EXSTYLE))) ||
                !SetWindowPos(Window, nullptr, 0, 0, Rect.right - Rect.left, Rect.bottom - Rect.top,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
            {
                ++m_RayReconstructionValidationFailures;
                LOG_ERROR_MESSAGE("RR validation could not resize the native window");
            }
#else
            LOG_WARNING_MESSAGE("RR validation automatic window resize is Windows-only; resize manually on this platform");
#endif
            break;
        }
        case 5:
            m_RealtimeUI.RealtimeMode = false;
            break;
        case 6:
        case 7:
        case 8:
            if (m_pDevice->GetDeviceInfo().IsVulkanDevice())
            {
                // Existing NRD RGB10A2 UAV allocation blocks these realtime modes.
                LOG_WARNING_MESSAGE("RR validation: Vulkan NRD/TAA/SR mode regression skipped (existing RGB10A2 UAV support limitation)");
                m_RealtimeUI.RealtimeMode = false;
                break;
            }
            m_RealtimeUI.RealtimeMode       = true;
            m_RealtimeUI.RealtimeAA         = Stage == 7 ? RTXPTRealtimeAAMode::SuperResolution : RTXPTRealtimeAAMode::TAA;
            m_RealtimeUI.StandaloneDenoiser = Stage == 8;
            break;
        case 9:
            m_RealtimeUI.RealtimeMode           = true;
            m_RealtimeUI.RealtimeAA             = RTXPTRealtimeAAMode::DLSSRR;
            m_RayReconstructionSettings.Quality = RAY_RECONSTRUCTION_QUALITY_QUALITY;
            break;
        case 10:
            if (m_pDevice->GetDeviceInfo().Type == RENDER_DEVICE_TYPE_D3D12)
                m_RayReconstructionSettings.Preset = RAY_RECONSTRUCTION_DLSS_PRESET_E;
            RequestAccumulationReset("RR validation lighting change");
            m_LightIntensityScale *= 0.8f;
            break;
        case 11:
            m_EnvIntensity *= 2.0f;
            m_EnvMapBakerSettingsDirty = true;
            return;
        case 12:
            m_ReferenceUI.EnvironmentMapEnabled = false;
            m_EnvMapBakerSettingsDirty          = true;
            return;
        case 13:
            m_ReferenceUI.EnvironmentMapEnabled = true;
            m_EnvMapBakerSettingsDirty          = true;
            return;
    }
    RequestRealtimeCachesReset("RR scene validation stage changed");
}
