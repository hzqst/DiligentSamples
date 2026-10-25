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
#include "../shaders/PathTracer/Lighting/LightingConfig.h"

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

constexpr Uint32 kProxyRatio                        = RTXPT_LIGHTING_SAMPLING_PROXY_RATIO;
constexpr Uint32 kMaxLightCount                     = RTXPT_LIGHTING_MAX_LIGHTS;
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
    Uint32 LightClusterCount                                             = 0;
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
static_assert(offsetof(RTXPTLightingControlDataCPU, LightClusterCount) == 100,
              "LightingControlData LightClusterCount offset must match LightingTypes.hlsli");
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
    m_ProxyBuildPass.Reset();
    m_EnvironmentLights.Reset();
    m_LightFeedback.Reset();
    m_LightHistory.Reset();
    m_EnvironmentRadianceMap.Release();
    m_EnvironmentBuildPending = false;
    m_ControlBuffer.Release();
    m_LightsBuffer.Release();
    m_LightsExBuffer.Release();
    m_LightBuffersDirty = false;
    m_LightWeights.Release();
    m_LightProxyCounters.Release();
    m_LightSamplingProxies.Release();
    m_LocalSamplingBuffer.Release();
    m_AnalyticLightBuffer    = nullptr;
    m_AnalyticLightExBuffer  = nullptr;
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
    m_LightHistory.ClearHistory();
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

    const bool FeedbackOk   = CreateFeedbackTextures(pDevice, Width, Height);
    const bool LocalOk      = CreateLocalSamplingBuffer(pDevice, Width, Height);
    const bool ProxyBuildOk = FeedbackOk && LocalOk && m_ProxyBuildPass.Initialize(pDevice, pEngineFactory, pStateCache) &&
        m_LightFeedback.Initialize(pDevice, pEngineFactory, pStateCache, Width, Height);
    const bool EnvBuildOk = m_EnvironmentLights.Initialize(pDevice, pEngineFactory, pStateCache) &&
        m_LightHistory.Initialize(pDevice, pEngineFactory, pStateCache);
    m_Stats.Ready = FeedbackOk && LocalOk && ProxyBuildOk && EnvBuildOk;
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
    const Uint32 EnvResolution = Settings.EnvMapParams.Enabled > 0.0f ? Settings.EnvMapImportanceMapResolution : 0u;
    if (!m_EnvironmentLights.CreateResources(pDevice, EnvResolution))
    {
        m_Stats.LastError = "Environment quadtree requires a power-of-two importance map of at least 128 texels";
        return false;
    }
    m_EnvironmentRadianceMap  = Settings.pEnvironmentRadianceMap;
    m_EnvironmentBuildPending = true;

    // Allocate the GPU proxy buffers sized to the current light count; the per-triangle power-proportional
    // proxy contents are filled on the GPU in UpdateEnd (RunProxyBuild), which needs a device context.
    if (!CreateProxyBuffers(pDevice, Lights, Settings))
        return false;
    std::vector<RTXPTLightSourceRange> Sources = Lights.GetAnalyticSources();
    for (auto& Source : Sources) Source.Base += m_Stats.EnvironmentLightCount;
    for (auto Source : Lights.GetTriangleSources())
    {
        Source.Base += m_Stats.EnvironmentLightCount + m_Stats.AnalyticLightCount;
        Sources.push_back(Source);
    }
    Uint32 ClusterBase = m_Stats.EnvironmentLightCount + m_Stats.AnalyticLightCount + m_Stats.TriangleLightCount;
    for (auto Source : Lights.GetTriangleSources())
    {
        if (Source.Count <= RTXPT_LIGHT_CLUSTER_MIN_TRIANGLES) continue;
        Source.Key[0] = 4u;
        // Preserve the original triangle count in the identity: a changed source
        // invalidates history even if it still needs the same number of clusters.
        Source.Key[3] ^= Uint64{Source.Count} << 32;
        Source.Base  = ClusterBase;
        Source.Count = DivRoundUp(Source.Count, RTXPT_LIGHT_CLUSTER_SIZE);
        ClusterBase += Source.Count;
        Sources.push_back(Source);
    }
    m_LightHistory.SetSources(std::move(Sources), m_Stats.TotalLightCount, m_Stats.EnvironmentLightCount);
    if (!m_LightFeedback.SetLightCount(pDevice, m_Stats.TotalLightCount))
        return false;
    if (!UploadControlBuffer(pDevice, Lights, Settings))
        return false;
    m_ProxyBuildPending = true;

    ++m_Stats.UpdateCounter;
    m_Stats.Ready = true;
    m_Stats.LastError.clear();
    return true;
}

bool RTXPTLightsBaker::PrepareLightBuffers(IDeviceContext* pContext)
{
    if (!m_Stats.Ready || !pContext) return false;
    if (m_EnvironmentBuildPending)
    {
        if (!m_EnvironmentLights.Build(pContext, m_ControlBuffer, m_EnvironmentRadianceMap))
        {
            m_Stats.LastError = "Environment light quadtree build failed";
            return false;
        }
        m_EnvironmentBuildPending = false;
    }

    if (m_LightBuffersDirty)
    {
        const auto Copy = [&](IBuffer* Source, IBuffer* Destination, Uint32 Offset, Uint32 Count, Uint32 Stride) {
            if (Count != 0u)
                pContext->CopyBuffer(Source, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Destination,
                                     Uint64{Offset} * Stride, Uint64{Count} * Stride, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        };
        const Uint32 AnalyticBase = m_Stats.EnvironmentLightCount;
        const Uint32 TriangleBase = AnalyticBase + m_Stats.AnalyticLightCount;
        Copy(m_EnvironmentLights.GetLights(), m_LightsBuffer, 0, m_Stats.EnvironmentLightCount, sizeof(PolymorphicLightInfo));
        Copy(m_AnalyticLightBuffer, m_LightsBuffer, AnalyticBase, m_Stats.AnalyticLightCount, sizeof(PolymorphicLightInfo));
        Copy(m_AnalyticLightExBuffer, m_LightsExBuffer, AnalyticBase, m_Stats.AnalyticLightCount, sizeof(PolymorphicLightInfoEx));
        Copy(m_EmissiveTriangleBuffer, m_LightsBuffer, TriangleBase, m_Stats.TriangleLightCount, sizeof(PolymorphicLightInfo));
        m_LightBuffersDirty = false;
    }
    return true;
}

bool RTXPTLightsBaker::UpdateEnd(IDeviceContext* pContext,
                                 ITextureView*   pDepthSRV,
                                 ITextureView*   pMotionVectorsSRV)
{
    if (!PrepareLightBuffers(pContext)) return false;

    if (!m_LightHistory.Prepare(pContext, m_ControlBuffer, m_EnvironmentLights.GetLights(), m_EnvironmentLights.GetLookupSRV()))
    {
        m_Stats.LastError = "RTXPT light history mapping failed";
        return false;
    }
    // Per-triangle power-proportional proxy build (GPU). Runs after the emissive-triangle build pass (so the
    // emissive radiance is current) and before the local-sampling fill (which reads the proxy table).
    if (!m_LightFeedback.Prepare(pContext, m_ControlBuffer, m_LightSamplingProxies, m_LocalSamplingBuffer,
                                 m_FeedbackTotalWeight, m_FeedbackCandidates, pDepthSRV, pMotionVectorsSRV, m_ResetFeedbackPending, m_LightHistory.GetPastToCurrent()))
    {
        m_Stats.LastError = "RTXPT temporal light feedback preparation failed";
        return false;
    }
    if (m_ProxyBuildPending || m_LocalSamplingEnabled)
    {
        if (!RunProxyBuild(pContext))
        {
            m_Stats.LastError = "RTXPT LightsBaker proxy build failed";
            return false;
        }
        m_ProxyBuildPending = false;
    }

    if (!m_LightFeedback.Finish(pContext, m_LocalSamplingBuffer))
    {
        m_Stats.LastError = "RTXPT temporal light feedback processing failed";
        return false;
    }
    m_ResetFeedbackPending = false;
    if (!m_LightHistory.Finish(pContext, m_EnvironmentLights.GetLights(), m_EnvironmentLights.GetLookupSRV()))
    {
        m_Stats.LastError = "RTXPT light history snapshot failed";
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

    const Uint32 TileCountX   = DivRoundUp(std::max(Width, 1u) + kTileSize - 1u, kTileSize);
    const Uint32 TileCountY   = DivRoundUp(std::max(Height, 1u) + kTileSize - 1u, kTileSize);
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

    const Uint64 AnalyticCount64      = Lights.GetAnalyticLights().size();
    const Uint32 TriangleCount        = Lights.GetEmissiveTriangleCount();
    const Uint64 PhysicalLightCount64 = Uint64{m_EnvironmentLights.GetLightCount()} + AnalyticCount64 + TriangleCount;
    Uint64       ClusterCount         = 0;
    for (const auto& Source : Lights.GetTriangleSources())
        if (Source.Count > RTXPT_LIGHT_CLUSTER_MIN_TRIANGLES)
            ClusterCount += (Uint64{Source.Count} + RTXPT_LIGHT_CLUSTER_SIZE - 1u) / RTXPT_LIGHT_CLUSTER_SIZE;
    if (PhysicalLightCount64 + ClusterCount > kMaxLightCount)
    {
        m_Stats.LastError = "RTXPT unified light table exceeds RTXPT_LIGHTING_MAX_LIGHTS";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }
    const Uint32 AnalyticCount      = static_cast<Uint32>(AnalyticCount64);
    const Uint32 PhysicalLightCount = static_cast<Uint32>(PhysicalLightCount64);
    const Uint32 TotalLightCount    = static_cast<Uint32>(PhysicalLightCount64 + ClusterCount);

    m_Stats.TotalLightCount       = TotalLightCount;
    m_Stats.AnalyticLightCount    = AnalyticCount;
    m_Stats.TriangleLightCount    = TriangleCount;
    m_Stats.EnvironmentLightCount = m_EnvironmentLights.GetLightCount();
    m_Stats.ProxyTotalWeight      = 0.0f; // computed on the GPU now (control.WeightsSumUINT)
    m_ProxyImportanceType         = Settings.ImportanceSamplingType;

    constexpr Uint32 MaxProxies = RTXPT_LIGHTING_MAX_SAMPLING_PROXIES;
    m_ProxyBudget               = std::max(1u, std::min(kProxyRatio * PhysicalLightCount, MaxProxies - MaxProxies / 256u));
    m_Stats.SamplingProxyCount  = m_ProxyBudget; // CPU estimate (UI / local-sampling gating); GPU writes the real count

    // Capture the source light buffers for the GPU weight computation (owned by RTXPTLights for the scene's lifetime).
    m_AnalyticLightBuffer    = Lights.GetLightBuffer();
    m_AnalyticLightExBuffer  = Lights.GetLightExBuffer();
    m_EmissiveTriangleBuffer = Lights.GetEmissiveTriangleBuffer();
    m_LightBuffersDirty      = true;

    auto CreateRWStructured = [&](const char* Name, Uint32 ElementCount, Uint32 Stride, RefCntAutoPtr<IBuffer>& Out, const void* Initial = nullptr) {
        BufferDesc Desc;
        Desc.Name              = Name;
        Desc.Usage             = USAGE_DEFAULT;
        Desc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        Desc.Mode              = BUFFER_MODE_STRUCTURED;
        Desc.ElementByteStride = Stride;
        Desc.Size              = Uint64{std::max(ElementCount, 1u)} * Stride;
        Out.Release();
        BufferData Data{Initial, Desc.Size};
        pDevice->CreateBuffer(Desc, Initial ? &Data : nullptr, &Out);
        return Out != nullptr;
    };

    std::vector<PolymorphicLightInfo> InitialLights(std::max(1u, TotalLightCount));
    Uint32                            ClusterIndex   = PhysicalLightCount;
    const Uint32                      TriangleOffset = m_EnvironmentLights.GetLightCount() + AnalyticCount;
    for (const auto& Source : Lights.GetTriangleSources())
    {
        if (Source.Count <= RTXPT_LIGHT_CLUSTER_MIN_TRIANGLES) continue;
        for (Uint32 From = 0; From < Source.Count; From += RTXPT_LIGHT_CLUSTER_SIZE)
        {
            auto& Cluster             = InitialLights[ClusterIndex++];
            Cluster.ColorTypeAndFlags = kPolymorphicLightTypeCluster << kPolymorphicLightTypeShift;
            Cluster.Direction1        = (TriangleOffset + Source.Base + From) |
                ((std::min(Uint32{RTXPT_LIGHT_CLUSTER_SIZE}, Source.Count - From) - 1u) << 23);
            // Keep the untrimmed range so each frame can rebuild LOD from current geometry.
            Cluster.Direction2 = Cluster.Direction1;
        }
    }
    const bool BuffersOk =
        CreateRWStructured("RTXPT packed lights", TotalLightCount, sizeof(PolymorphicLightInfo), m_LightsBuffer, InitialLights.data()) &&
        CreateRWStructured("RTXPT LightsBaker light weights", TotalLightCount, sizeof(float), m_LightWeights) &&
        CreateRWStructured("RTXPT LightsBaker proxy counters", TotalLightCount, sizeof(Uint32), m_LightProxyCounters) &&
        CreateRWStructured("RTXPT LightsBaker sampling proxies", m_ProxyBudget, sizeof(Uint32), m_LightSamplingProxies);
    if (!BuffersOk)
    {
        m_Stats.LastError = "Failed to create RTXPT LightsBaker proxy buffers";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    // Extensions are a separate stream. Unshaped lights never fetch this buffer.
    std::vector<PolymorphicLightInfoEx> Extensions(std::max(TotalLightCount, 1u));
    for (auto& Extension : Extensions)
        Extension.ClusterIndexAndCount = ~0u;
    BufferDesc ExDesc;
    ExDesc.Name              = "RTXPT packed light extensions";
    ExDesc.Usage             = USAGE_DEFAULT;
    ExDesc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    ExDesc.Mode              = BUFFER_MODE_STRUCTURED;
    ExDesc.ElementByteStride = sizeof(PolymorphicLightInfoEx);
    ExDesc.Size              = Extensions.size() * sizeof(PolymorphicLightInfoEx);
    BufferData ExData{Extensions.data(), ExDesc.Size};
    m_LightsExBuffer.Release();
    pDevice->CreateBuffer(ExDesc, &ExData, &m_LightsExBuffer);
    if (!m_LightsExBuffer) return false;
    return true;
}

bool RTXPTLightsBaker::RunProxyBuild(IDeviceContext* pContext)
{
    if (m_Stats.TotalLightCount == 0u)
        return true; // no lights: control.SamplingProxyCount stays 0 and NEE light sampling is skipped

    if (!m_ProxyBuildPass.IsReady() || m_ControlBuffer == nullptr || m_LightsBuffer == nullptr ||
        m_LightsExBuffer == nullptr || m_LightWeights == nullptr || m_LightProxyCounters == nullptr ||
        m_LightSamplingProxies == nullptr)
        return false;

    return m_ProxyBuildPass.Build(pContext,
                                  m_ControlBuffer,
                                  m_LightsBuffer,
                                  m_LightsExBuffer,
                                  m_LightFeedback.GetCounts(),
                                  m_LightWeights,
                                  m_LightProxyCounters,
                                  m_LightSamplingProxies,
                                  m_Stats.TotalLightCount,
                                  m_Stats.AnalyticLightCount,
                                  m_ProxyBudget,
                                  m_ProxyImportanceType,
                                  m_LightHistory.GetCurrentToPast(),
                                  m_FrustumPlanes.data(),
                                  m_Stats.TotalLightCount - m_Stats.EnvironmentLightCount - m_Stats.AnalyticLightCount - m_Stats.TriangleLightCount,
                                  &m_CameraPosition);
}

void RTXPTLightsBaker::RequestProxyRebuild()
{
    m_LightBuffersDirty = true;
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
    Control.TotalLightCount     = m_Stats.TotalLightCount;
    Control.EnvmapQuadNodeCount = m_Stats.EnvironmentLightCount;
    Control.AnalyticLightCount  = m_Stats.AnalyticLightCount;
    Control.TriangleLightCount  = m_Stats.TriangleLightCount;
    Control.LightClusterCount   = m_Stats.TotalLightCount - m_Stats.EnvironmentLightCount - m_Stats.AnalyticLightCount - m_Stats.TriangleLightCount;
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
    Control.BakerPadding[16]                 = FloatAsUint(Settings.ImportanceBoostIntensityDelta);
    Control.BakerPadding[17]                 = FloatAsUint(Settings.ImportanceBoostFrustumMul);
    Control.BakerPadding[18]                 = FloatAsUint(Settings.ImportanceBoostFrustumFadeDistance);
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

void RTXPTLightsBaker::UpdateFrame(const float4x4& ViewProj, const float3& CameraPosition)
{
    m_CameraPosition = CameraPosition;
    if (!m_Stats.Ready || !m_ControlBuffer) return;
    // Diligent uses forward Z (near = clip.z), whereas upstream uses reversed Z
    // (near = clip.w - clip.z). The four side planes use the same row-vector math.
    const float4 X{ViewProj._11, ViewProj._21, ViewProj._31, ViewProj._41};
    const float4 Y{ViewProj._12, ViewProj._22, ViewProj._32, ViewProj._42};
    const float4 Z{ViewProj._13, ViewProj._23, ViewProj._33, ViewProj._43};
    const float4 W{ViewProj._14, ViewProj._24, ViewProj._34, ViewProj._44};
    m_FrustumPlanes = {W + X, W - X, W - Y, W + Y, Z};
    for (auto& Plane : m_FrustumPlanes)
    {
        const float Length = std::sqrt(Plane.x * Plane.x + Plane.y * Plane.y + Plane.z * Plane.z);
        Plane              = Length > 0.0f ? Plane / Length : float4{};
        Plane.w            = -Plane.w;
    }
    // Frustum importance and intensity history must follow every rendered frame.
    m_ProxyBuildPending = true;
}

} // namespace Diligent
