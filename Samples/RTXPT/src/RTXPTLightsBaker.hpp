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

#include <string>
#include <vector>

#include "Buffer.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "GLTFLoader.hpp"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"
#include "TextureView.h"

#include "RTXPTLights.hpp"
#include "RTXPTLightsBakerPass.hpp"
#include "RTXPTLightProxyBuildPass.hpp"
#include "RTXPTEnvironmentLights.hpp"
#include "RTXPTLightFeedback.hpp"
#include "RTXPTSceneGraph.hpp"

namespace Diligent
{

struct IRenderStateCache;

struct RTXPTFloat3x4
{
    float4 Row0 = {};
    float4 Row1 = {};
    float4 Row2 = {};
};

struct LightsBakerEnvMapParamsCPU
{
    RTXPTFloat3x4 Transform       = {};
    RTXPTFloat3x4 InvTransform    = {};
    float3        ColorMultiplier = float3{1, 1, 1};
    float         Enabled         = 0.0f;
};

struct RTXPTLightsBakerSettings
{
    Uint32                     ImportanceSamplingType        = 1; // 0=Uniform, 1=Power+, 2=NEE-AT.
    float3                     CameraPosition                = float3{0, 0, 0};
    float3                     CameraDirection               = float3{0, 0, -1};
    float                      AverageContentsDistance       = 10.0f;
    Uint32                     MouseCursorX                  = 0;
    Uint32                     MouseCursorY                  = 0;
    float4x4                   ViewProjMatrix                = float4x4::Identity();
    float                      GlobalTemporalFeedbackWeight  = 0.75f;
    float                      LocalToGlobalSampleRatio      = 0.65f;
    bool                       UseApproximateMIS             = false;
    bool                       ResetFeedback                 = false;
    float2                     ViewportSize                  = float2{0, 0};
    float2                     PrevViewportSize              = float2{0, 0};
    Uint32                     EnvMapImportanceMapResolution = 0;
    Uint32                     EnvMapImportanceMapMipCount   = 0;
    LightsBakerEnvMapParamsCPU EnvMapParams                  = {};
    ITextureView*              pEnvironmentRadianceMap       = nullptr;
    float                      DistantVsLocalImportanceScale = 1.0f;
    Int64                      FrameIndex                    = -1;
};

struct RTXPTLightsBakerStats
{
    bool        Ready                   = false;
    bool        FeedbackReady           = false;
    Uint32      TotalLightCount         = 0;
    Uint32      AnalyticLightCount      = 0;
    Uint32      TriangleLightCount      = 0;
    Uint32      EnvironmentLightCount   = 0;
    Uint32      SamplingProxyCount      = 0;
    Uint32      LocalSamplingTileCountX = 0;
    Uint32      LocalSamplingTileCountY = 0;
    Uint32      UpdateCounter           = 0;
    float       ProxyTotalWeight        = 0.0f;
    std::string LastError;
};

class RTXPTLightsBaker
{
public:
    void Reset();
    void SceneReloaded();
    void RequestFeedbackReset();
    // Re-run the GPU per-triangle proxy build on the next UpdateEnd (e.g. after a dynamic/skinned update
    // rebuilt the emissive-triangle buffer in place, so the power weights/proxies reflect current geometry).
    void RequestProxyRebuild();

    bool CreateResources(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache, Uint32 Width, Uint32 Height, bool ComputeSupported);
    bool UpdateBegin(IRenderDevice* pDevice, const RTXPTLights& Lights, const RTXPTLightsBakerSettings& Settings);
    // Process feedback after BUILD exported current depth and pixel-space motion.
    bool UpdateEnd(IDeviceContext* pContext, ITextureView* pDepthSRV, ITextureView* pMotionVectorsSRV);

    bool InfoGUI(float Indent);
    bool DebugGUI(float Indent);

    const RTXPTLightsBakerStats& GetStats() const { return m_Stats; }
    IBuffer*                     GetControlBuffer() const { return m_ControlBuffer; }
    IBuffer*                     GetLightProxyCounters() const { return m_LightProxyCounters; }
    IBuffer*                     GetLightSamplingProxies() const { return m_LightSamplingProxies; }
    IBuffer*                     GetLocalSamplingBuffer() const { return m_LocalSamplingBuffer; }
    IBuffer*                     GetEnvironmentLights() const { return m_EnvironmentLights.GetLights(); }
    ITextureView*                GetEnvironmentLightLookup() const { return m_EnvironmentLights.GetLookupSRV(); }
    ITextureView*                GetFeedbackTotalWeightUAV() const { return m_FeedbackTotalWeightUAV; }
    ITextureView*                GetFeedbackCandidatesUAV() const { return m_FeedbackCandidatesUAV; }
    ITextureView*                GetFeedbackTotalWeightSRV() const { return m_FeedbackTotalWeightSRV; }
    ITextureView*                GetFeedbackCandidatesSRV() const { return m_FeedbackCandidatesSRV; }

private:
    // Allocates the per-light GPU buffers (weights, proxy counters, proxy table) sized to the current light
    // count, and captures the analytic + emissive-triangle source buffers. The proxy contents are then filled
    // on the GPU by RunProxyBuild() (RTXPTLightProxyBuildPass) per-triangle and power-proportionally.
    bool CreateProxyBuffers(IRenderDevice* pDevice, const RTXPTLights& Lights, const RTXPTLightsBakerSettings& Settings);
    bool RunProxyBuild(IDeviceContext* pContext);
    bool UploadControlBuffer(IRenderDevice* pDevice, const RTXPTLights& Lights, const RTXPTLightsBakerSettings& Settings);
    bool CreateFeedbackTextures(IRenderDevice* pDevice, Uint32 Width, Uint32 Height);
    bool CreateLocalSamplingBuffer(IRenderDevice* pDevice, Uint32 Width, Uint32 Height);

    RefCntAutoPtr<IBuffer>      m_ControlBuffer;
    RefCntAutoPtr<IBuffer>      m_LightWeights;
    RefCntAutoPtr<IBuffer>      m_LightProxyCounters;
    RefCntAutoPtr<IBuffer>      m_LightSamplingProxies;
    RefCntAutoPtr<IBuffer>      m_LocalSamplingBuffer;
    IBuffer*                    m_AnalyticLightBuffer    = nullptr; // borrowed from RTXPTLights (analytic-light weights)
    IBuffer*                    m_EmissiveTriangleBuffer = nullptr; // borrowed from RTXPTLights (emissive-triangle power)
    RefCntAutoPtr<ITexture>     m_FeedbackTotalWeight;
    RefCntAutoPtr<ITexture>     m_FeedbackCandidates;
    RefCntAutoPtr<ITextureView> m_FeedbackTotalWeightSRV;
    RefCntAutoPtr<ITextureView> m_FeedbackTotalWeightUAV;
    RefCntAutoPtr<ITextureView> m_FeedbackCandidatesSRV;
    RefCntAutoPtr<ITextureView> m_FeedbackCandidatesUAV;

    RTXPTLightProxyBuildPass m_ProxyBuildPass;
    RTXPTEnvironmentLights      m_EnvironmentLights;
    RTXPTLightFeedback          m_LightFeedback;
    RefCntAutoPtr<ITextureView> m_EnvironmentRadianceMap;
    bool                        m_EnvironmentBuildPending = false;

    Uint32                m_ProxyBudget          = 0;     // capacity of m_LightSamplingProxies
    Uint32                m_ProxyImportanceType  = 1;     // 0 = uniform, otherwise power-proportional
    bool                  m_ProxyBuildPending    = false; // run the GPU proxy build on the next UpdateEnd
    Uint32                m_AllocatedWidth       = 0;
    Uint32                m_AllocatedHeight      = 0;
    bool                  m_ResetFeedbackPending = false;
    bool                  m_LocalSamplingEnabled = false;
    RTXPTLightsBakerStats m_Stats;
};

} // namespace Diligent
