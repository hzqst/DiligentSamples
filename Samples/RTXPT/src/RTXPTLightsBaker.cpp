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

#include "RTXPTLightsBaker.hpp"

#include "DebugUtilities.hpp"
#include "GraphicsTypesX.hpp"
#include "RenderStateCache.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

namespace Diligent
{

namespace
{

constexpr Uint32 kProxyRatio                        = 12u;
constexpr Uint32 kLocalProxyCount                   = 128u;
constexpr Uint32 kTileSize                          = 8u;
constexpr float  kDistantVsLocalImportanceBaseScale = 0.0002f;
constexpr Uint32 kLightingControlHeaderSizeBytes    = 112u;
constexpr Uint32 kLightsBakerConstantsSizeBytes     = 464u;
constexpr Uint32 kLightsBakerEnvMapParamsBaseWord   = 88u;
constexpr Uint32 kLightsBakerEnvMapColorBaseWord    = kLightsBakerEnvMapParamsBaseWord + 24u;

static_assert(kLightsBakerConstantsSizeBytes % sizeof(Uint32) == 0, "LightsBakerConstants size must be uint-addressable");
static_assert(kLightsBakerEnvMapColorBaseWord + 4u <= kLightsBakerConstantsSizeBytes / sizeof(Uint32),
              "EnvMapParams must fit inside LightsBakerConstants padding");

struct RTXPTLightingControlDataCPU
{
    Uint32 TotalLightCount                                               = 0;
    Uint32 EnvmapQuadNodeCount                                           = 0;
    Uint32 AnalyticLightCount                                            = 0;
    Uint32 TriangleLightCount                                            = 0;
    Uint32 SamplingProxyCount                                            = 0;
    Uint32 HistoricTotalLightCount                                       = 0;
    Uint32 LastFrameTemporalFeedbackAvailable                            = 0;
    Uint32 LastFrameLocalSamplesAvailable                                = 0;
    Uint32 ProxyBuildTaskCount                                           = 0;
    Uint32 WeightsSumUINT                                                = 0;
    Uint32 ImportanceSamplingType                                        = 1;
    Uint32 _padding0                                                     = 0;
    Uint32 TemporalFeedbackRequired                                      = 0;
    Uint32 TotalMaxFeedbackCount                                         = 0;
    float  GlobalFeedbackUseWeight                                       = 0.75f;
    float  LocalToGlobalSampleRatio                                      = 0.65f;
    Uint32 TileBufferHeight                                              = 0;
    float  ScreenSpaceVsWorldSpaceThreshold                              = 0.3f;
    Uint32 LocalSamplingResolution[2]                                    = {};
    Uint32 LocalSamplingTileJitter[2]                                    = {};
    Uint32 LocalSamplingTileJitterPrev[2]                                = {};
    Uint32 ValidFeedbackCount                                            = 0;
    Uint32 _padding1                                                     = 0;
    Uint32 _padding2                                                     = 0;
    Uint32 _padding3                                                     = 0;
    Uint32 BakerPadding[kLightsBakerConstantsSizeBytes / sizeof(Uint32)] = {};
};
static_assert(sizeof(RTXPTLightingControlDataCPU) == kLightingControlHeaderSizeBytes + kLightsBakerConstantsSizeBytes,
              "LightingControlData CPU mirror must match LightingTypes.hlsli");
static_assert(offsetof(RTXPTLightingControlDataCPU, AnalyticLightCount) == 8,
              "LightingControlData AnalyticLightCount offset must match LightingTypes.hlsli");
static_assert(offsetof(RTXPTLightingControlDataCPU, SamplingProxyCount) == 16,
              "LightingControlData SamplingProxyCount offset must match LightingTypes.hlsli");
static_assert(offsetof(RTXPTLightingControlDataCPU, ProxyBuildTaskCount) == 32,
              "LightingControlData ProxyBuildTaskCount offset must match LightingTypes.hlsli");
static_assert(offsetof(RTXPTLightingControlDataCPU, TemporalFeedbackRequired) == 48,
              "LightingControlData TemporalFeedbackRequired offset must match LightingTypes.hlsli");
static_assert(offsetof(RTXPTLightingControlDataCPU, TileBufferHeight) == 64,
              "LightingControlData TileBufferHeight offset must match LightingTypes.hlsli");
static_assert(offsetof(RTXPTLightingControlDataCPU, LocalSamplingResolution) == 72,
              "LightingControlData LocalSamplingResolution offset must match LightingTypes.hlsli");
static_assert(offsetof(RTXPTLightingControlDataCPU, LocalSamplingTileJitter) == 80,
              "LightingControlData LocalSamplingTileJitter offset must match LightingTypes.hlsli");
static_assert(offsetof(RTXPTLightingControlDataCPU, ValidFeedbackCount) == 96,
              "LightingControlData ValidFeedbackCount offset must match LightingTypes.hlsli");
static_assert(offsetof(RTXPTLightingControlDataCPU, BakerPadding) == kLightingControlHeaderSizeBytes,
              "LightingControlData BakerConstants offset must match LightingTypes.hlsli");
static_assert(sizeof(RTXPTLightingControlDataCPU) - offsetof(RTXPTLightingControlDataCPU, BakerPadding) == kLightsBakerConstantsSizeBytes,
              "LightingControlData BakerConstants payload size must match LightingTypes.hlsli");

Uint32 DivRoundUp(Uint32 Value, Uint32 Divisor)
{
    return (Value + Divisor - 1u) / Divisor;
}

Uint32 FloatAsUint(float Value)
{
    Uint32 Bits = 0;
    static_assert(sizeof(Bits) == sizeof(Value), "float and uint bit widths must match");
    std::memcpy(&Bits, &Value, sizeof(Bits));
    return Bits;
}

} // namespace

void RTXPTLightsBaker::Reset()
{
    m_FillLocalSamplingPass.Reset();
    m_ClearFeedbackPass.Reset();
    m_ProxyBuildPass.Reset();
    m_ControlBuffer.Release();
    m_LightWeights.Release();
    m_LightProxyCounters.Release();
    m_LightSamplingProxies.Release();
    m_LocalSamplingBuffer.Release();
    m_AnalyticLightBuffer    = nullptr;
    m_EmissiveTriangleBuffer = nullptr;
    m_FeedbackTotalWeight.Release();
    m_FeedbackCandidates.Release();
    m_FeedbackTotalWeightSRV.Release();
    m_FeedbackTotalWeightUAV.Release();
    m_FeedbackCandidatesSRV.Release();
    m_FeedbackCandidatesUAV.Release();
    m_ProxyBudget          = 0;
    m_ProxyImportanceType  = 1;
    m_ProxyBuildPending    = false;
    m_AllocatedWidth       = 0;
    m_AllocatedHeight      = 0;
    m_ResetFeedbackPending = false;
    m_LocalSamplingEnabled = false;
    m_Stats                = {};
}

void RTXPTLightsBaker::SceneReloaded()
{
    m_ProxyBuildPending = false;
    RequestFeedbackReset();
    m_LocalSamplingEnabled     = false;
    m_Stats.TotalLightCount    = 0;
    m_Stats.AnalyticLightCount = 0;
    m_Stats.TriangleLightCount = 0;
    m_Stats.SamplingProxyCount = 0;
    m_Stats.ProxyTotalWeight   = 0.0f;
}

void RTXPTLightsBaker::RequestFeedbackReset()
{
    m_ResetFeedbackPending = true;
}

bool RTXPTLightsBaker::CreateResources(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache, Uint32 Width, Uint32 Height, bool ComputeSupported)
{
    if (pDevice == nullptr)
    {
        m_Stats.LastError = "RTXPT LightsBaker requires a render device";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }
    if (pEngineFactory == nullptr)
    {
        m_Stats.LastError = "RTXPT LightsBaker requires an engine factory";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }
    if (!ComputeSupported)
    {
        m_Stats.LastError = "RTXPT LightsBaker requires compute shader support";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    const bool FeedbackOk          = CreateFeedbackTextures(pDevice, Width, Height);
    const bool LocalOk             = CreateLocalSamplingBuffer(pDevice, Width, Height);
    const bool ClearFeedbackPassOk = FeedbackOk &&
        m_ClearFeedbackPass.Initialize(pDevice, pEngineFactory, pStateCache, "RTXPT LightsBaker clear feedback", "ClearFeedbackCS");
    const bool FillPassOk = FeedbackOk && LocalOk && ClearFeedbackPassOk &&
        m_FillLocalSamplingPass.Initialize(pDevice, pEngineFactory, pStateCache, "RTXPT LightsBaker fill local sampling", "FillLocalSamplingCS");
    const bool ProxyBuildOk = FillPassOk && m_ProxyBuildPass.Initialize(pDevice, pEngineFactory, pStateCache);
    m_Stats.Ready           = FeedbackOk && LocalOk && ClearFeedbackPassOk && FillPassOk && ProxyBuildOk;
    return m_Stats.Ready;
}

bool RTXPTLightsBaker::UpdateBegin(IRenderDevice* pDevice, const RTXPTLights& Lights, const RTXPTLightsBakerSettings& Settings)
{
    m_Stats.Ready = false;
    m_ControlBuffer.Release();
    m_LightWeights.Release();
    m_LightProxyCounters.Release();
    m_LightSamplingProxies.Release();
    m_LocalSamplingEnabled = false;

    if (Settings.ResetFeedback)
        RequestFeedbackReset();

    // Allocate the GPU proxy buffers sized to the current light count; the per-triangle power-proportional
    // proxy contents are filled on the GPU in UpdateEnd (RunProxyBuild), which needs a device context.
    if (!CreateProxyBuffers(pDevice, Lights, Settings))
        return false;
    if (!UploadControlBuffer(pDevice, Lights, Settings))
        return false;
    m_ProxyBuildPending = true;

    ++m_Stats.UpdateCounter;
    m_Stats.Ready = true;
    m_Stats.LastError.clear();
    return true;
}

bool RTXPTLightsBaker::UpdateEnd(IDeviceContext* pContext,
                                 ITextureView*   pDepthSRV,
                                 ITextureView*   pMotionVectorsSRV)
{
    if (!m_Stats.Ready)
        return false;

    // Current Diligent LightsBaker only clears/processes feedback resources.
    // Depth and motion-vector views are accepted here to preserve the RTXPT-fork
    // PathTrace call contract for future NEE-AT feedback passes.
    (void)pDepthSRV;
    (void)pMotionVectorsSRV;

    // Per-triangle power-proportional proxy build (GPU). Runs after the emissive-triangle build pass (so the
    // emissive radiance is current) and before the local-sampling fill (which reads the proxy table).
    if (m_ProxyBuildPending)
    {
        if (!RunProxyBuild(pContext))
        {
            m_Stats.LastError = "RTXPT LightsBaker proxy build failed";
            return false;
        }
        m_ProxyBuildPending = false;
    }

    if (m_ResetFeedbackPending)
    {
        const Uint32 ClearGroupsX = (m_AllocatedWidth + 7u) / 8u;
        const Uint32 ClearGroupsY = (m_AllocatedHeight + 7u) / 8u;
        const bool   ClearFeedbackOk =
            m_ClearFeedbackPass.Bind(m_ControlBuffer, m_LightProxyCounters, m_LightSamplingProxies, m_LocalSamplingBuffer,
                                     nullptr, nullptr, m_FeedbackTotalWeightUAV, m_FeedbackCandidatesUAV) &&
            m_ClearFeedbackPass.Dispatch(pContext, ClearGroupsX, ClearGroupsY);
        if (!ClearFeedbackOk)
        {
            m_Stats.LastError = "RTXPT LightsBaker feedback clear failed";
            return false;
        }
        m_ResetFeedbackPending = false;
    }

    if (!m_LocalSamplingEnabled)
    {
        m_Stats.LastError.clear();
        return true;
    }

    const Uint32 GroupsX = m_Stats.LocalSamplingTileCountX;
    const Uint32 GroupsY = m_Stats.LocalSamplingTileCountY;
    if (GroupsX == 0u || GroupsY == 0u)
    {
        m_Stats.LastError = "RTXPT LightsBaker local sampling has no dispatchable tile groups";
        return false;
    }

    // Fill owns every entry of each tile, including the empty-proxy case.
    const bool FillOk = m_FillLocalSamplingPass.Bind(m_ControlBuffer, m_LightProxyCounters, m_LightSamplingProxies, m_LocalSamplingBuffer,
                                                     m_FeedbackTotalWeightSRV, m_FeedbackCandidatesSRV, nullptr, nullptr) &&
        m_FillLocalSamplingPass.Dispatch(pContext, GroupsX, GroupsY);
    if (!FillOk)
    {
        m_Stats.LastError = "RTXPT LightsBaker local sampling fill failed";
        return false;
    }

    m_Stats.LastError.clear();
    return true;
}

bool RTXPTLightsBaker::InfoGUI(float Indent)
{
    ImGui::Indent(Indent);
    ImGui::Text("Ready: %s", m_Stats.Ready ? "yes" : "no");
    ImGui::Text("Lights: %u", m_Stats.TotalLightCount);
    ImGui::Text("Proxies: %u", m_Stats.SamplingProxyCount);
    ImGui::Text("Feedback: %s", m_Stats.FeedbackReady ? "ready" : "missing");
    ImGui::Text("Local tiles: %u x %u", m_Stats.LocalSamplingTileCountX, m_Stats.LocalSamplingTileCountY);
    if (!m_Stats.LastError.empty())
        ImGui::TextWrapped("LightsBaker error: %s", m_Stats.LastError.c_str());
    ImGui::Unindent(Indent);
    return false;
}

bool RTXPTLightsBaker::DebugGUI(float Indent)
{
    ImGui::Indent(Indent);
    ImGui::Text("Update counter: %u", m_Stats.UpdateCounter);
    ImGui::Text("Proxy total weight: %.3f", m_Stats.ProxyTotalWeight);
    ImGui::Text("Allocated feedback: %u x %u", m_AllocatedWidth, m_AllocatedHeight);
    ImGui::Unindent(Indent);
    return false;
}

bool RTXPTLightsBaker::CreateFeedbackTextures(IRenderDevice* pDevice, Uint32 Width, Uint32 Height)
{
    const Uint32 SafeWidth  = std::max(Width, 1u);
    const Uint32 SafeHeight = std::max(Height, 1u);
    const Uint64 PixelCount = Uint64{SafeWidth} * Uint64{SafeHeight};

    m_FeedbackTotalWeight.Release();
    m_FeedbackCandidates.Release();
    m_FeedbackTotalWeightSRV.Release();
    m_FeedbackTotalWeightUAV.Release();
    m_FeedbackCandidatesSRV.Release();
    m_FeedbackCandidatesUAV.Release();

    std::vector<float>  ZeroWeights(static_cast<size_t>(PixelCount), 0.0f);
    std::vector<Uint32> InvalidCandidates(static_cast<size_t>(PixelCount), std::numeric_limits<Uint32>::max());

    TextureSubResData WeightSubres{ZeroWeights.data(), Uint64{SafeWidth} * sizeof(float)};
    TextureData       WeightData{&WeightSubres, 1};

    TextureDesc WeightDesc;
    WeightDesc.Name      = "RTXPT LightsBaker feedback total weight";
    WeightDesc.Type      = RESOURCE_DIM_TEX_2D;
    WeightDesc.Width     = SafeWidth;
    WeightDesc.Height    = SafeHeight;
    WeightDesc.Format    = TEX_FORMAT_R32_FLOAT;
    WeightDesc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    pDevice->CreateTexture(WeightDesc, &WeightData, &m_FeedbackTotalWeight);

    TextureSubResData CandidateSubres{InvalidCandidates.data(), Uint64{SafeWidth} * sizeof(Uint32)};
    TextureData       CandidateData{&CandidateSubres, 1};

    TextureDesc CandidateDesc = WeightDesc;
    CandidateDesc.Name        = "RTXPT LightsBaker feedback candidates";
    CandidateDesc.Format      = TEX_FORMAT_R32_UINT;
    pDevice->CreateTexture(CandidateDesc, &CandidateData, &m_FeedbackCandidates);

    if (!m_FeedbackTotalWeight || !m_FeedbackCandidates)
    {
        m_Stats.LastError = "Failed to create RTXPT LightsBaker feedback textures";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    m_FeedbackTotalWeightSRV = m_FeedbackTotalWeight->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    m_FeedbackTotalWeightUAV = m_FeedbackTotalWeight->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    m_FeedbackCandidatesSRV  = m_FeedbackCandidates->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    m_FeedbackCandidatesUAV  = m_FeedbackCandidates->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS);
    m_Stats.FeedbackReady    = m_FeedbackTotalWeightSRV && m_FeedbackTotalWeightUAV &&
        m_FeedbackCandidatesSRV && m_FeedbackCandidatesUAV;
    if (!m_Stats.FeedbackReady)
    {
        m_Stats.LastError = "Failed to create RTXPT LightsBaker feedback texture views";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    m_AllocatedWidth  = SafeWidth;
    m_AllocatedHeight = SafeHeight;
    return true;
}

bool RTXPTLightsBaker::CreateLocalSamplingBuffer(IRenderDevice* pDevice, Uint32 Width, Uint32 Height)
{
    m_LocalSamplingBuffer.Release();

    const Uint32 TileCountX   = DivRoundUp(std::max(Width, 1u), kTileSize);
    const Uint32 TileCountY   = DivRoundUp(std::max(Height, 1u), kTileSize);
    const Uint64 ElementCount = Uint64{TileCountX} * Uint64{TileCountY} * kLocalProxyCount;

    BufferDesc Desc;
    Desc.Name              = "RTXPT LightsBaker local sampling buffer";
    Desc.Usage             = USAGE_DEFAULT;
    Desc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    Desc.Mode              = BUFFER_MODE_STRUCTURED;
    Desc.ElementByteStride = sizeof(Uint32);
    Desc.Size              = ElementCount * sizeof(Uint32);
    pDevice->CreateBuffer(Desc, nullptr, &m_LocalSamplingBuffer);

    if (!m_LocalSamplingBuffer)
    {
        m_Stats.LastError = "Failed to create RTXPT LightsBaker local sampling buffer";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    m_Stats.LocalSamplingTileCountX = TileCountX;
    m_Stats.LocalSamplingTileCountY = TileCountY;
    return true;
}

bool RTXPTLightsBaker::CreateProxyBuffers(IRenderDevice* pDevice, const RTXPTLights& Lights, const RTXPTLightsBakerSettings& Settings)
{
    if (pDevice == nullptr)
    {
        m_Stats.LastError = "RTXPT LightsBaker proxy buffer creation requires a render device";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    const Uint32 AnalyticCount   = static_cast<Uint32>(Lights.GetAnalyticLights().size());
    const Uint32 TriangleCount   = Lights.GetEmissiveTriangleCount();
    const Uint32 TotalLightCount = AnalyticCount + TriangleCount;

    m_Stats.TotalLightCount    = TotalLightCount;
    m_Stats.AnalyticLightCount = AnalyticCount;
    m_Stats.TriangleLightCount = TriangleCount;
    m_Stats.ProxyTotalWeight   = 0.0f; // computed on the GPU now (control.WeightsSumUINT)
    m_ProxyImportanceType      = Settings.ImportanceSamplingType;

    // Proxy budget mirrors the previous CPU build (kProxyRatio proxies per light); the GPU build fills up to
    // this many entries and writes the exact count into control.SamplingProxyCount. ProxyBudget is the actual
    // allocated capacity, so the path tracer's selection pdf denominator stays consistent.
    // Intentional deviation from RTXPT-fork: we floor at max(TotalLightCount, 1) rather than upstream's
    // max(TotalLightCount, RTXPT_LIGHTING_MAX_LIGHTS/10). Unbiased either way; see RTXPT_FORK_MAPPING.md
    // ("proxy budget floor deviation") for the rationale (avoids a fixed ~2.4 MB minimum proxy buffer).
    m_ProxyBudget              = kProxyRatio * std::max(TotalLightCount, 1u);
    m_Stats.SamplingProxyCount = m_ProxyBudget; // CPU estimate (UI / local-sampling gating); GPU writes the real count

    // Capture the source light buffers for the GPU weight computation (owned by RTXPTLights for the scene's lifetime).
    m_AnalyticLightBuffer    = Lights.GetLightBuffer();
    m_EmissiveTriangleBuffer = Lights.GetEmissiveTriangleBuffer();

    auto CreateRWStructured = [&](const char* Name, Uint32 ElementCount, Uint32 Stride, RefCntAutoPtr<IBuffer>& Out) {
        BufferDesc Desc;
        Desc.Name              = Name;
        Desc.Usage             = USAGE_DEFAULT;
        Desc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        Desc.Mode              = BUFFER_MODE_STRUCTURED;
        Desc.ElementByteStride = Stride;
        Desc.Size              = Uint64{std::max(ElementCount, 1u)} * Stride;
        Out.Release();
        pDevice->CreateBuffer(Desc, nullptr, &Out);
        return Out != nullptr;
    };

    const bool BuffersOk =
        CreateRWStructured("RTXPT LightsBaker light weights", TotalLightCount, sizeof(float), m_LightWeights) &&
        CreateRWStructured("RTXPT LightsBaker proxy counters", TotalLightCount, sizeof(Uint32), m_LightProxyCounters) &&
        CreateRWStructured("RTXPT LightsBaker sampling proxies", m_ProxyBudget, sizeof(Uint32), m_LightSamplingProxies);
    if (!BuffersOk)
    {
        m_Stats.LastError = "Failed to create RTXPT LightsBaker proxy buffers";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }
    return true;
}

bool RTXPTLightsBaker::RunProxyBuild(IDeviceContext* pContext)
{
    if (m_Stats.TotalLightCount == 0u)
        return true; // no lights: control.SamplingProxyCount stays 0 and NEE light sampling is skipped

    if (!m_ProxyBuildPass.IsReady() || m_ControlBuffer == nullptr || m_AnalyticLightBuffer == nullptr ||
        m_EmissiveTriangleBuffer == nullptr || m_LightWeights == nullptr || m_LightProxyCounters == nullptr ||
        m_LightSamplingProxies == nullptr)
        return false;

    return m_ProxyBuildPass.Build(pContext,
                                  m_ControlBuffer,
                                  m_AnalyticLightBuffer,
                                  m_EmissiveTriangleBuffer,
                                  m_LightWeights,
                                  m_LightProxyCounters,
                                  m_LightSamplingProxies,
                                  m_Stats.TotalLightCount,
                                  m_Stats.AnalyticLightCount,
                                  m_ProxyBudget,
                                  m_ProxyImportanceType);
}

void RTXPTLightsBaker::RequestProxyRebuild()
{
    // The proxy buffers already exist (allocated in UpdateBegin); a dynamic/skinned update only rewrites the
    // emissive-triangle buffer contents in place. Flag the GPU build to re-run on the next UpdateEnd so the
    // per-triangle power weights, proxy counters, and proxy table are recomputed from the current geometry.
    m_ProxyBuildPending = true;
}

bool RTXPTLightsBaker::UploadControlBuffer(IRenderDevice* pDevice, const RTXPTLights&, const RTXPTLightsBakerSettings& Settings)
{
    if (pDevice == nullptr)
    {
        m_Stats.LastError = "RTXPT LightsBaker control upload requires a render device";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    RTXPTLightingControlDataCPU Control;
    Control.TotalLightCount    = m_Stats.TotalLightCount;
    Control.AnalyticLightCount = m_Stats.AnalyticLightCount;
    Control.TriangleLightCount = m_Stats.TriangleLightCount;
    // SamplingProxyCount, WeightsSumUINT and ProxyBuildTaskCount are produced by the GPU proxy build
    // (RTXPTLightProxyBuildPass) into this DEFAULT/UAV buffer; initialize them to zero.
    Control.SamplingProxyCount      = 0u;
    Control.HistoricTotalLightCount = m_Stats.TotalLightCount;
    Control.ProxyBuildTaskCount     = 0u;
    Control.WeightsSumUINT          = 0u;
    const bool  NEEATEnabled        = Settings.ImportanceSamplingType == 2u;
    const float FeedbackUseWeight   = NEEATEnabled ? std::clamp(Settings.GlobalTemporalFeedbackWeight, 0.0f, 0.95f) : 0.0f;
    const float LocalToGlobalRatio  = NEEATEnabled ? std::clamp(Settings.LocalToGlobalSampleRatio, 0.0f, 0.95f) : 0.0f;
    m_LocalSamplingEnabled =
        NEEATEnabled &&
        m_Stats.SamplingProxyCount > 0u &&
        m_Stats.LocalSamplingTileCountX > 0u &&
        m_Stats.LocalSamplingTileCountY > 0u;

    Control.ImportanceSamplingType           = Settings.ImportanceSamplingType;
    Control.TemporalFeedbackRequired         = NEEATEnabled ? 1u : 0u;
    Control.GlobalFeedbackUseWeight          = FeedbackUseWeight;
    Control.LocalToGlobalSampleRatio         = LocalToGlobalRatio;
    Control.TileBufferHeight                 = m_Stats.LocalSamplingTileCountY;
    Control.ScreenSpaceVsWorldSpaceThreshold = 0.3f;
    Control.LocalSamplingResolution[0]       = m_Stats.LocalSamplingTileCountX;
    Control.LocalSamplingResolution[1]       = m_Stats.LocalSamplingTileCountY;
    Control.BakerPadding[0]                  = FloatAsUint(Settings.DistantVsLocalImportanceScale * kDistantVsLocalImportanceBaseScale);
    Control.BakerPadding[1]                  = Settings.EnvMapImportanceMapMipCount;
    Control.BakerPadding[2]                  = Settings.EnvMapImportanceMapResolution;
    Control.BakerPadding[4]                  = m_AllocatedWidth;
    Control.BakerPadding[5]                  = m_AllocatedHeight;
    Control.BakerPadding[6]                  = m_AllocatedWidth;
    Control.BakerPadding[7]                  = m_AllocatedHeight;
    Control.BakerPadding[8]                  = Settings.MouseCursorX;
    Control.BakerPadding[9]                  = Settings.MouseCursorY;
    Control.BakerPadding[10]                 = FloatAsUint(Settings.ViewportSize.x > 0.0f ? Settings.PrevViewportSize.x / Settings.ViewportSize.x : 1.0f);
    Control.BakerPadding[11]                 = FloatAsUint(Settings.ViewportSize.y > 0.0f ? Settings.PrevViewportSize.y / Settings.ViewportSize.y : 1.0f);
    Control.BakerPadding[14]                 = m_Stats.UpdateCounter + 1u;
    Control.BakerPadding[20]                 = FloatAsUint(Settings.CameraPosition.x);
    Control.BakerPadding[21]                 = FloatAsUint(Settings.CameraPosition.y);
    Control.BakerPadding[22]                 = FloatAsUint(Settings.CameraPosition.z);
    Control.BakerPadding[23]                 = FloatAsUint(Settings.AverageContentsDistance);
    const auto WriteFloat4                   = [&Control](Uint32 BaseIndex, const float4& Value) {
        Control.BakerPadding[BaseIndex + 0] = FloatAsUint(Value.x);
        Control.BakerPadding[BaseIndex + 1] = FloatAsUint(Value.y);
        Control.BakerPadding[BaseIndex + 2] = FloatAsUint(Value.z);
        Control.BakerPadding[BaseIndex + 3] = FloatAsUint(Value.w);
    };
    // EnvMapParams follows the scalar controls, frustum planes, and frustum corners in LightsBakerConstants.
    WriteFloat4(kLightsBakerEnvMapParamsBaseWord + 0u, Settings.EnvMapParams.Transform.Row0);
    WriteFloat4(kLightsBakerEnvMapParamsBaseWord + 4u, Settings.EnvMapParams.Transform.Row1);
    WriteFloat4(kLightsBakerEnvMapParamsBaseWord + 8u, Settings.EnvMapParams.Transform.Row2);
    WriteFloat4(kLightsBakerEnvMapParamsBaseWord + 12u, Settings.EnvMapParams.InvTransform.Row0);
    WriteFloat4(kLightsBakerEnvMapParamsBaseWord + 16u, Settings.EnvMapParams.InvTransform.Row1);
    WriteFloat4(kLightsBakerEnvMapParamsBaseWord + 20u, Settings.EnvMapParams.InvTransform.Row2);
    Control.BakerPadding[kLightsBakerEnvMapColorBaseWord + 0u] = FloatAsUint(Settings.EnvMapParams.ColorMultiplier.x);
    Control.BakerPadding[kLightsBakerEnvMapColorBaseWord + 1u] = FloatAsUint(Settings.EnvMapParams.ColorMultiplier.y);
    Control.BakerPadding[kLightsBakerEnvMapColorBaseWord + 2u] = FloatAsUint(Settings.EnvMapParams.ColorMultiplier.z);
    Control.BakerPadding[kLightsBakerEnvMapColorBaseWord + 3u] = FloatAsUint(Settings.EnvMapParams.Enabled);

    BufferDesc Desc;
    Desc.Name              = "RTXPT LightsBaker control buffer";
    Desc.Usage             = USAGE_DEFAULT; // GPU proxy build writes the computed scalars via UAV atomics
    Desc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    Desc.Mode              = BUFFER_MODE_STRUCTURED;
    Desc.ElementByteStride = sizeof(RTXPTLightingControlDataCPU);
    Desc.Size              = sizeof(RTXPTLightingControlDataCPU);
    BufferData Data{&Control, Desc.Size};
    pDevice->CreateBuffer(Desc, &Data, &m_ControlBuffer);

    if (!m_ControlBuffer)
    {
        m_Stats.LastError = "Failed to upload RTXPT LightsBaker control buffer";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }
    return true;
}

} // namespace Diligent
