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

#include "DeviceContext.h"
#include "RenderDevice.h"

#include "RTXPTNrdIntegration.hpp"
#include "RTXPTPostProcessPass.hpp"

namespace Diligent
{

struct RTXPTNrdDenoiserFrameAttribs
{
    // Render targets and merge output shared by every layer; the denoiser sets the per-layer fields.
    RTXPTDenoiserPostProcessAttribs PostProcess;
    const SampleConstants*          pFrameConstants  = nullptr;
    const RTXPTRealtimeSettings*    pRealtime        = nullptr;
    Uint32                          LayerCount       = kRTXPTDenoisingLayerCount;
    Uint32                          FrameIndex       = 0;
    float                           TimeDeltaSeconds = -1.0f;
    bool                            ResetHistory     = false;
};

// Per-layer standalone NRD shared by the Sample and embedded realtime views: one NRD instance per
// denoising layer, all sharing the render targets' denoiser inputs.
class RTXPTNrdDenoiser
{
public:
    void Reset();

    // Keeps the layer instances that already match CreateInfo and recreates the others. A failure
    // releases every layer, so the next successful call starts all histories from scratch.
    bool Prepare(IRenderDevice* pDevice, const RTXPTNrdIntegrationCreateInfo& CreateInfo);

    bool Denoise(IDeviceContext* pContext, RTXPTPostProcessPass& PostProcess, const RTXPTNrdDenoiserFrameAttribs& Attribs);

    bool                       IsReady() const;
    const RTXPTNrdIntegration& GetLayer(Uint32 LayerIndex) const { return m_Layers[LayerIndex]; }
    const char*                GetLastFailureReason() const { return m_LastFailureReason.c_str(); }

private:
    bool Fail(const char* Reason);

    std::array<RTXPTNrdIntegration, kRTXPTDenoisingLayerCount> m_Layers;
    std::string                                                m_LastFailureReason;
};

} // namespace Diligent
