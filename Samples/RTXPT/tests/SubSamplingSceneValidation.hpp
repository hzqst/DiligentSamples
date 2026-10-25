// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once
// Opt-in production sequence: ten stages each for NRD and native RR.
void RTXPTSample::RunSubSamplingValidation()
{
    if (!m_ValidateSubSampling) return;
    constexpr Uint32 FramesPerStage = 32, StageCount = 20;
    const Uint32     Frame = m_SubSamplingValidationFrame++;
    if (Frame && Frame <= FramesPerStage * StageCount && !m_LastFrameSucceeded)
    {
        ++m_SubSamplingValidationFailures;
        LOG_ERROR_MESSAGE("SubSampling scene validation failed at frame ", Frame - 1, ": ", m_RealtimePathTraceStatus);
    }
    if (Frame == FramesPerStage * StageCount)
        LOG_INFO_MESSAGE("SubSampling scene validation finished: failures=", m_SubSamplingValidationFailures);
    if (Frame >= FramesPerStage * StageCount) return;
    if (!Frame) m_SubSamplingValidationInitialMode = m_RealtimeUI.RealtimeSubSamplingMode;
    const Uint32 Stage = Frame / FramesPerStage;
    if (Stage % 10 == 1)
        m_Camera.SetPos(m_Camera.GetPos() + float3{0.0025f, 0, 0});
    if (Frame % FramesPerStage) return;
    LOG_INFO_MESSAGE("SubSampling validation stage ", Stage, " starts at frame ", Frame);
    switch (Stage % 10)
    {
        case 0:
            m_RealtimeUI.RealtimeMode            = true;
            m_RealtimeUI.RealtimeSamplesPerPixel = 1;
            m_RealtimeUI.RealtimeSubSamplingMode = m_SubSamplingValidationInitialMode;
            m_RealtimeUI.StandaloneDenoiser      = Stage < 10;
            m_RealtimeUI.RealtimeAA              = Stage < 10 ? RTXPTRealtimeAAMode::Disabled : RTXPTRealtimeAAMode::DLSSRR;
            m_RayReconstructionSettings.Quality  = RAY_RECONSTRUCTION_QUALITY_NATIVE;
            break;
        case 1: return;                                                         // Continuous motion retains history.
        case 2: m_Camera.SetPos(m_Camera.GetPos() + float3{0.5f, 0, 0}); break; // Reveal surfaces, invalidate history.
        case 3: m_RealtimeUI.RealtimeSubSamplingMode = 1; break;
        case 4: m_RealtimeUI.RealtimeSubSamplingMode = 2; break;
        case 5: m_RealtimeUI.RealtimeSubSamplingMode = 3; break;
        case 6: m_RealtimeUI.RealtimeSubSamplingPattern = 1; break;
        case 7:
            m_RealtimeUI.RealtimeSubSamplingPattern   = 2;
            m_RealtimeUI.RealtimeSubSamplingBlockSize = 16;
            break;
        case 8:
        {
            m_RealtimeUI.RealtimeSubSamplingPattern   = 3;
            m_RealtimeUI.RealtimeSubSamplingBlockSize = 4;
#if PLATFORM_WIN32
            HWND Window = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
            RECT Rect   = {0, 0, Stage < 10 ? 641 : 639, Stage < 10 ? 361 : 359};
            if (!Window || !AdjustWindowRectEx(&Rect, static_cast<DWORD>(GetWindowLongPtr(Window, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtr(Window, GWL_EXSTYLE))) ||
                !SetWindowPos(Window, nullptr, 0, 0, Rect.right - Rect.left, Rect.bottom - Rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
            {
                ++m_SubSamplingValidationFailures;
                LOG_ERROR_MESSAGE("SubSampling validation resize failed");
            }
#endif
            break;
        }
        case 9: m_RealtimeUI.RealtimeSubSamplingMode = 0; break;
    }
    RequestRealtimeCachesReset("SubSampling validation stage changed");
}
