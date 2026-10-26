// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once
namespace
{
std::atomic<Uint32>      g_AsyncValidationEngineErrorCount{0};
DebugMessageCallbackType g_AsyncValidationForwardCallback = nullptr;

// Engine-side errors (Vulkan validation, failed submissions after device loss) never reach
// m_LastFrameSucceeded, so count them here and forward the message to the regular output.
void DILIGENT_CALL_TYPE CountAsyncValidationEngineMessage(DEBUG_MESSAGE_SEVERITY Severity, const Char* Message, const Char* Function, const Char* File, int Line)
{
    if (Severity >= DEBUG_MESSAGE_SEVERITY_ERROR)
        g_AsyncValidationEngineErrorCount.fetch_add(1u, std::memory_order_relaxed);
    if (g_AsyncValidationForwardCallback != nullptr)
        g_AsyncValidationForwardCallback(Severity, Message, Function, File, Line);
}
} // namespace

void RTXPTSample::InstallAsyncValidationErrorCounter(IEngineFactory* pFactory)
{
    // The engine module has its own callback; forward to this module's default console output.
    g_AsyncValidationForwardCallback = DebugMessageCallback;
    pFactory->SetMessageCallback(CountAsyncValidationEngineMessage);
}

void RTXPTSample::RunAsyncPrefixValidation()
{
    if (!m_ValidateAsyncPrefix) return;
    constexpr Uint32 FramesPerStage = 32, StageCount = 10;
    const Uint32     Frame        = m_AsyncValidationFrame++;
    const Uint32     EngineErrors = g_AsyncValidationEngineErrorCount.load(std::memory_order_relaxed);
    if (Frame && Frame <= FramesPerStage * StageCount && !m_LastFrameSucceeded)
    {
        ++m_AsyncValidationFailures;
        LOG_ERROR_MESSAGE("AsyncPrefix scene validation failed at frame ", Frame - 1, ": ", m_RealtimePathTraceStatus);
    }
    else if (Frame && Frame <= FramesPerStage * StageCount && EngineErrors != m_AsyncValidationEngineErrors)
    {
        ++m_AsyncValidationFailures;
        LOG_ERROR_MESSAGE("AsyncPrefix scene validation failed at frame ", Frame - 1, ": engine reported ",
                          EngineErrors - m_AsyncValidationEngineErrors, " error(s)");
    }
    m_AsyncValidationEngineErrors = EngineErrors;
    if (Frame == FramesPerStage * StageCount)
        LOG_INFO_MESSAGE("AsyncPrefix scene validation finished: failures=", m_AsyncValidationFailures,
                         ", engine errors=", EngineErrors, ", async frames=", m_AsyncFrames);
    if (Frame >= FramesPerStage * StageCount) return;
    const Uint32 Stage = Frame / FramesPerStage;
    if (Stage == 2) m_Camera.SetPos(m_Camera.GetPos() + float3{0.0025f, 0, 0});
    if (Frame % FramesPerStage) return;
    LOG_INFO_MESSAGE("AsyncPrefix validation stage ", Stage, " starts at frame ", Frame);
    DrainAsyncPrefix();
    switch (Stage)
    {
        case 0:
            m_EnableAsyncPrefix                 = true;
            m_RealtimeUI.RealtimeMode           = true;
            m_RealtimeUI.StandaloneDenoiser     = false;
            m_RealtimeUI.RealtimeAA             = RTXPTRealtimeAAMode::DLSSRR;
            m_RayReconstructionSettings.Quality = RAY_RECONSTRUCTION_QUALITY_NATIVE;
            break;
        case 1: m_EnableAsyncPrefix = false; break;
        case 2: m_EnableAsyncPrefix = true; break;
        case 3:
        {
#if PLATFORM_WIN32
            HWND Window = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
            RECT Rect   = {0, 0, 641, 361};
            if (!Window || !AdjustWindowRectEx(&Rect, static_cast<DWORD>(GetWindowLongPtr(Window, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtr(Window, GWL_EXSTYLE))) ||
                !SetWindowPos(Window, nullptr, 0, 0, Rect.right - Rect.left, Rect.bottom - Rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
            {
                ++m_AsyncValidationFailures;
                LOG_ERROR_MESSAGE("AsyncPrefix validation resize failed");
            }
#endif
            break;
        }
        case 4:
            if (!SetCurrentScene(m_CurrentSceneName, true)) ++m_AsyncValidationFailures;
            break;
        case 5: m_RealtimeUI.RealtimeMode = false; break;
        case 6: m_RealtimeUI.RealtimeMode = true; break;
        case 7: m_EnableSceneAnimations = true; break; // Dynamic geometry takes the serial route.
        case 8:
            m_EnableSceneAnimations                 = false;
            m_RealtimeUI.RealtimeSubSamplingMode    = 3;
            m_RealtimeUI.RealtimeSubSamplingPattern = 1;
            break;
        case 9: m_Camera.SetPos(m_Camera.GetPos() + float3{0.5f, 0, 0}); break;
    }
    RequestRealtimeCachesReset("AsyncPrefix validation stage changed");
}
