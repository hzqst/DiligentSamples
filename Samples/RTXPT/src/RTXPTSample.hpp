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
#include <string>
#include <vector>

#include "Buffer.h"
#include "FirstPersonCamera.hpp"
#include "PBR_Renderer.hpp"
#include "RefCntAutoPtr.hpp"
#include "RTXPTAccelerationStructures.hpp"
#include "RTXPTBlitPass.hpp"
#include "RTXPTEmissiveTrianglePass.hpp"
#include "RTXPTEnvMapBaker.hpp"
#include "RTXPTFrameConstants.hpp"
#include "RTXPTRealtimeSettings.hpp"
#include "RTXPTLights.hpp"
#include "RTXPTLightsBaker.hpp"
#include "RTXPTMaterials.hpp"
#include "RTXPTRayTracingPass.hpp"
#include "RTXPTDenoisingGuidesBaker.hpp"
#include "RTXPTNrdIntegration.hpp"
#include "RTXPTRenderTargets.hpp"
#include "RTXPTToneMappingPass.hpp"
#include "RTXPTPostProcessPipeline.hpp"
#include "SampleBase.hpp"
#include "RTXPTScene.hpp"
#include "RTXPTSkinnedGeometry.hpp"

namespace Diligent
{

struct IRenderStateCache;

struct RTXPTFeatureCaps
{
    bool RayTracing                  = false;
    bool StandaloneRayTracingShaders = false;
    bool RayQuery                    = false;
    bool BindlessResources           = false;
    bool ComputeShaders              = false;
    bool DXILCompiler                = false;
    bool SPIRVCompiler               = false;
};

// Reference-mode UI state, mirroring the reference subset of RTXPT-fork's SampleUIData
// (D:/RTXPT-fork/Rtxpt/SampleUI.h).
struct RTXPTReferenceUIState
{
    bool                       EnableRussianRoulette           = true; // RR: always on; start bounce == Min bounces (RR start).
    bool                       ReferenceFireflyFilterEnabled   = true; // Phase R1 (G1): adaptive firefly filter.
    float                      ReferenceFireflyFilterThreshold = 5.0f; // Phase R1 (G1).
    int                        DiffuseBounceCount              = 2;    // Phase R5 (G9): separate diffuse-bounce limit.
    bool                       EnableToneMapping               = true; // Phase 6: configurable tone-map pass.
    RTXPTToneMappingParameters ToneMapping;                            // Phase 6/P3: post-process tone-mapping controls.
    bool                       EnableBloom                        = true;
    float                      BloomRadius                        = 8.0f;
    float                      BloomIntensity                     = 0.004f;
    int                        NEEType                            = 2; // Phase R3 (G5): 0=Uniform, 1=Power+, 2=NEE-AT.
    int                        NEECandidateSamples                = 5; // Phase R3 (G5): RIS candidate count.
    int                        NEEFullSamples                     = 1; // Phase R3 (G5): visibility-tested full samples.
    int                        NEEMISType                         = 1; // Phase R3 (G5): 0=Full, 1=ApproxInRealtime, 2=Approximate (deferred).
    float                      NEEAT_GlobalTemporalFeedbackWeight = 0.75f;
    float                      NEEAT_LocalToGlobalSampleRatio     = 0.65f;
    float                      NEEAT_DistantVsLocalImportance     = 1.0f;
    int                        NestedDielectricsQuality           = 1;        // Nested dielectrics quality: 0=Off, 1=Fast, 2=Quality.
    bool                       EnableLDSamplerForBSDF             = true;     // Phase R5 (G9): low-discrepancy (Sobol/Owen) sampler.
    bool                       EnvironmentMapEnabled              = false;    // Phase R4 (G7): HDR env-map loading (procedural sky is always active).
    float                      CameraAperture                     = 0.0f;     // Phase R7 (G11): thin-lens aperture radius.
    float                      CameraFocalDistance                = 10000.0f; // Phase R7 (G11): thin-lens focal distance.
};

class RTXPTSample final : public SampleBase
{
public:
    ~RTXPTSample() override;

    virtual void        Initialize(const SampleInitInfo& InitInfo) override final;
    virtual void        Render() override final;
    virtual void        Update(double CurrTime, double ElapsedTime, bool DoUpdateUI) override final;
    virtual void        WindowResize(Uint32 Width, Uint32 Height) override final;
    virtual const Char* GetSampleName() const override final { return "RTXPT"; }
    virtual void        ModifyEngineInitInfo(const ModifyEngineInitInfoAttribs& Attribs) override final;

    // Initial window size (client area = swap chain resolution); the -w/-h command line overrides it.
    virtual DesiredApplicationSettings GetDesiredApplicationSettings(bool IsInitialization) override final
    {
        DesiredApplicationSettings Settings;
        if (IsInitialization)
        {
            Settings.SetWindowWidth(1920);
            Settings.SetWindowHeight(1080);
        }
        return Settings;
    }
    CommandLineStatus ProcessCommandLine(int argc, const char* const* argv) override;

protected:
    virtual void UpdateUI() override final;

private:
    void CreateFrameResources();
    void EnumerateAvailableScenes();
    void EnumerateEnvironmentMaps();
    bool SetCurrentScene(const std::string& SceneName, bool ForceReload = false);
    void ApplySceneEnvironmentSettings();
    void ResetSceneDependentResources();
    bool RebuildSceneDependentResources();
    void InitializeCamera();
    bool ApplySceneCamera(Uint32 CameraIndex);
    void UpdateCameraProjection(Uint32 Width, Uint32 Height);
    void UpdateFrameConstants(double CurrTime);
    void UpdateRenderTargetDimensions(float TimeDeltaSeconds);
    bool UpdateLightsBaker(bool ResetFeedback);
    bool UpdateEnvMapBaker(bool ForceRebuild);
    void CreateSceneIndependentPasses();
    void CreateSceneDependentPasses();
    bool BuildEmissiveTriangles();
    bool EnsureRenderTargets();
    void ClearFallback(const float4& ClearColor);
    void RequestAccumulationReset(const char* Reason, bool ResetTemporalHistory = true);
    void RequestRealtimeReset(RTXPTRealtimeResetFlags Flags, const char* Reason);
    void RequestRealtimeCachesReset(const char* Reason);
    void BeginRealtimeFrameResetScope();
    void InvalidatePreviousFrameConstants();
    bool RunReferencePathTraceAndPostProcess();
    bool Denoise();
    bool EnsureNrdIntegrations();
    void ResetNrdIntegrations();
    bool RunRealtimePostProcess();
    bool RunRealtimeNoDenoiserFinalMerge();
    bool PresentRealtimeFinalOutput();
    bool RunRealtimePathTraceOnly();
    bool PathTrace();
    void RunRayReconstructionValidation(double CurrTime);
    bool BakeDenoisingGuides();
    bool PresentRealtimeGuideDebug();
    bool DispatchPathTracePrePass(const RTXPTRayTracingDispatch& BaseDispatch);
    bool DispatchPathTraceLoop(bool UseStablePlanes, const RTXPTRayTracingDispatch& BaseDispatch);
    void RecordRealtimePathTraceStatus(const char* Status);

    RefCntAutoPtr<IRenderStateCache>                        m_pStateCache;
    RTXPTFeatureCaps                                        m_FeatureCaps;
    std::string                                             m_AssetsRoot;
    std::vector<std::string>                                m_AvailableScenes;
    std::string                                             m_CurrentSceneName;
    std::string                                             m_InitialSceneName;
    RTXPTScene                                              m_Scene;
    RTXPTMaterials                                          m_Materials;
    RTXPTLights                                             m_Lights;
    RTXPTLightsBaker                                        m_LightsBaker;
    RTXPTEnvMapBaker                                        m_EnvMapBaker;
    std::vector<RTXPTEnvMapSource>                          m_EnvMapSources;
    RTXPTEnvMapSettings                                     m_EnvMapSettings;
    RTXPTAccelerationStructures                             m_AccelerationStructures;
    RTXPTSkinnedSceneGeometry                               m_SkinnedGeometry;
    RTXPTRenderTargets                                      m_RenderTargets;
    RTXPTRayTracingPass                                     m_RayTracingPass;
    RTXPTDenoisingGuidesBaker                               m_DenoisingGuidesBaker;
    std::array<RTXPTNrdIntegration, kRTXPTStablePlaneCount> m_NrdIntegrations;
    bool                                                    m_LastRealtimePathTraceExecuted = false;
    bool                                                    m_LastRealtimeFinalMergeReady   = false;
    std::string                                             m_RealtimePathTraceStatus;
    RTXPTEmissiveTrianglePass                               m_EmissiveTrianglePass;
    RTXPTBlitPass                                           m_BlitPass;
    RTXPTPostProcessPipeline                                m_PostProcessPipeline;
    FirstPersonCamera                                       m_Camera;
    RefCntAutoPtr<IBuffer>                                  m_FrameConstantsCB;
    SampleConstants                                         m_LastFrameConstants;
    RTXPTReferenceUIState                                   m_ReferenceUI;
    RTXPTRealtimeSettings                                   m_RealtimeUI;
    RTXPTRealtimeResetFlags                                 m_RealtimeResetPending = RTXPT_REALTIME_RESET_REALTIME_CACHES |
        RTXPT_REALTIME_RESET_NRD_HISTORY |
        RTXPT_REALTIME_RESET_TAA_SR_HISTORY;
    RTXPTRealtimeResetFlags        m_CurrentFrameRealtimeReset   = RTXPT_REALTIME_RESET_NONE;
    RTXPTRenderTargetDimensions    m_CurrentTargetDimensions     = {};
    RTXPTSuperResolutionFrameDesc  m_CurrentSuperResolutionFrame = {};
    RTXPTRayReconstructionSettings m_RayReconstructionSettings;
    bool                           m_RayReconstructionFrameReady           = false;
    bool                           m_ValidateRayReconstruction             = false;
    bool                           m_EnableVulkanRayReconstruction         = true;
    bool                           m_LastFrameSucceeded                    = false;
    double                         m_RayReconstructionValidationStageStart = 0;
    Uint32                         m_RayReconstructionValidationFrame      = 0;
    Uint32                         m_RayReconstructionValidationFailures   = 0;
    float2                         m_CurrentRealtimeCameraJitter           = float2{0.0f, 0.0f};
    float                          m_LastElapsedTimeSeconds                = 0.0f;
    float4x4                       m_LastCameraView                        = float4x4::Identity();
    float4x4                       m_LastCameraProj                        = float4x4::Identity();
    PathTracerCameraData           m_PreviousFrameCamera                   = {};
    PathTracerViewData             m_PreviousFrameView                     = {};
    bool                           m_HasPreviousFrameConstants             = false;
    Uint32                         m_RealtimeSampleIndex                   = 0;
    Uint32                         m_LastSampleBaseIndex                   = 0;
    float                          m_CameraVerticalFov                     = PI_F / 4.0f;
    float                          m_CameraNearPlane                       = 1.0f;
    float                          m_CameraFarPlane                        = 10000.0f;
    Uint32                         m_FrameIndex                            = 0;
    Uint32                         m_AccumulationFrame                     = 0;
    Uint32                         m_MaxBounces                            = 20;
    Uint32                         m_MinBounces                            = 3;
    Uint32                         m_MaxNEEBounces                         = 16;
    bool                           m_EnableNEE                             = true;
    bool                           m_EnableEnvNEE                          = true;
    bool                           m_EnableEmissiveNEE                     = true;
    bool                           m_HasDynamicGeometry                    = false;
    bool                           m_EmissiveTrianglesDirty                = true;
    float                          m_EnvIntensity                          = 1.0f;
    float                          m_LightIntensityScale                   = 1.0f;
    int                            m_SelectedSceneCamera                   = -1;
    int                            m_SelectedEnvMapSource                  = 0;
    bool                           m_EnableSceneAnimations                 = false;
    bool                           m_ResetAccumulationPending              = true;
    bool                           m_AccumulationActive                    = false;
    bool                           m_HasLastCameraMatrices                 = false;
    bool                           m_LightsBakerSettingsDirty              = false;
    bool                           m_RayTracingPassSettingsDirty           = false;
    bool                           m_EnvMapBakerDirty                      = true;
    bool                           m_EnvMapBakerSettingsDirty              = true;
};

} // namespace Diligent
