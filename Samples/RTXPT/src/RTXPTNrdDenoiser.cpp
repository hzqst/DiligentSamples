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

#include "RTXPTNrdDenoiser.hpp"

#include "DebugUtilities.hpp"

#include "RTXPTRayTracingPass.hpp"

namespace Diligent
{

namespace
{

void InsertDenoiserPrepareOutputBarriers(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets, ITextureView* pMergeOutputUAV, Uint32 LayerIndex)
{
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, pMergeOutputUAV);
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetCombinedHistoryClampRelaxUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserViewspaceZUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserMotionVectorsUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserNormalRoughnessUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserDiffRadianceHitDistUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserSpecRadianceHitDistUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserDisocclusionThresholdMixUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserHistoryConfidenceUAV(LayerIndex));
}

} // namespace

void RTXPTNrdDenoiser::Reset()
{
    for (RTXPTNrdIntegration& Layer : m_Layers)
        Layer.Reset();
    m_LastFailureReason.clear();
}

bool RTXPTNrdDenoiser::Fail(const char* Reason)
{
    m_LastFailureReason = Reason;
    DEV_ERROR(m_LastFailureReason.c_str());
    return false;
}

bool RTXPTNrdDenoiser::IsReady() const
{
    for (const RTXPTNrdIntegration& Layer : m_Layers)
    {
        if (!Layer.IsReady())
            return false;
    }
    return true;
}

Uint64 RTXPTNrdDenoiser::GetPoolTextureBytes() const
{
    Uint64 Bytes = 0;
    for (const RTXPTNrdIntegration& Layer : m_Layers)
        Bytes += Layer.GetStats().PoolTextureBytes;
    return Bytes;
}

bool RTXPTNrdDenoiser::Prepare(IRenderDevice* pDevice, const RTXPTNrdIntegrationCreateInfo& CreateInfo)
{
    m_LastFailureReason.clear();
    for (RTXPTNrdIntegration& Layer : m_Layers)
    {
        const bool bLayerMatches =
            Layer.IsReady() &&
            Layer.GetMethod() == CreateInfo.Method &&
            Layer.GetWidth() == CreateInfo.Width &&
            Layer.GetHeight() == CreateInfo.Height;
        if (bLayerMatches)
            continue;

        if (!Layer.Initialize(pDevice, CreateInfo))
        {
            // The layer already reported the failure; keep its reason across the reset.
            const std::string Reason = Layer.GetLastFailureReason();
            Reset();
            m_LastFailureReason = Reason;
            return false;
        }
    }

    return true;
}

/* Denoises the active layers from the deepest to the primary one. For each layer:
 * 1. Prepare writes the layer's demodulated radiance, view Z, motion and normal/roughness into the denoiser
 *    inputs. The first (deepest) prepare also initializes the merge output with the stable radiance.
 * 2. That layer's NRD instance denoises the inputs with its own history.
 * 3. The final merge remodulates the denoised radiance and adds it to the merge output.
 * The merge output is linear HDR. Attribs.PostProcess supplies the render targets and the merge output UAV.
 */
bool RTXPTNrdDenoiser::Denoise(IDeviceContext* pContext, RTXPTPostProcessPass& PostProcess, const RTXPTNrdDenoiserFrameAttribs& Attribs)
{
    m_LastFailureReason.clear();

    const RTXPTRenderTargets* pRenderTargets = Attribs.PostProcess.pRenderTargets;
    if (pContext == nullptr || pRenderTargets == nullptr || Attribs.pRealtime == nullptr)
        return Fail("RTXPT NRD denoiser requires a device context, render targets, and realtime settings");
    if (Attribs.LayerCount == 0 || Attribs.LayerCount > kRTXPTDenoisingLayerCount)
        return Fail("RTXPT NRD denoiser layer count is out of range");

    const RTXPTNrdMethod Method            = Attribs.pRealtime->NRDMethod;
    const bool           bEnableValidation = pRenderTargets->GetDenoiserOutValidationUAV() != nullptr;
    const bool           bMergeValidation  = pRenderTargets->GetDenoiserOutValidationSRV() != nullptr;

    bool bInitOutput = true;
    for (Uint32 LayerIndex = Attribs.LayerCount; LayerIndex-- > 0;)
    {
        RTXPTDenoiserPostProcessAttribs PrepareAttribs = Attribs.PostProcess;
        PrepareAttribs.Method                          = Method;
        PrepareAttribs.PlaneIndex                      = LayerIndex;
        PrepareAttribs.InitOutput                      = bInitOutput;
        PrepareAttribs.MiniConstants.params.x          = LayerIndex;
        PrepareAttribs.MiniConstants.params.y          = bInitOutput ? 1u : 0u;
        if (!PostProcess.RunDenoiserPrepare(pContext, PrepareAttribs))
            return Fail("Denoiser prepare dispatch failed");

        bInitOutput = false;
        InsertDenoiserPrepareOutputBarriers(pContext, *pRenderTargets, Attribs.PostProcess.pMergeOutputUAV, LayerIndex);

        RTXPTNrdFrameAttribs NrdAttribs;
        NrdAttribs.pRenderTargets    = pRenderTargets;
        NrdAttribs.pFrameConstants   = Attribs.pFrameConstants;
        NrdAttribs.pRealtime         = Attribs.pRealtime;
        NrdAttribs.PlaneIndex        = LayerIndex;
        NrdAttribs.FrameIndex        = Attribs.FrameIndex;
        NrdAttribs.TimeDeltaSeconds  = Attribs.TimeDeltaSeconds;
        NrdAttribs.ResetHistory      = Attribs.ResetHistory;
        NrdAttribs.EnableValidation  = bEnableValidation;
        NrdAttribs.HistoryConfidence = Attribs.HistoryConfidence;
        if (!m_Layers[LayerIndex].Dispatch(pContext, NrdAttribs))
        {
            // The layer already reported the failure.
            m_LastFailureReason = m_Layers[LayerIndex].GetLastFailureReason();
            return false;
        }

        RTXPTDenoiserPostProcessAttribs MergeAttribs = Attribs.PostProcess;
        MergeAttribs.Method                          = Method;
        MergeAttribs.PlaneIndex                      = LayerIndex;
        MergeAttribs.HasValidation                   = bMergeValidation;
        MergeAttribs.MiniConstants.params.x          = LayerIndex;
        MergeAttribs.MiniConstants.params.y          = bMergeValidation ? 1u : 0u;
        if (!PostProcess.RunDenoiserFinalMerge(pContext, MergeAttribs))
            return Fail("Denoiser final merge dispatch failed");

        RTXPTRayTracingPass::InsertUAVBarrier(pContext, Attribs.PostProcess.pMergeOutputUAV);
    }

    return true;
}

} // namespace Diligent
