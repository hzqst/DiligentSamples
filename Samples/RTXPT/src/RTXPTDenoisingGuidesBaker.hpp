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

#include "Buffer.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "RTXPTRealtimeSettings.hpp"
#include "RTXPTRenderTargets.hpp"
#include "ShaderResourceBinding.h"

namespace Diligent
{

struct IRenderStateCache;

enum class RTXPTGuideHistoryMode
{
    Accumulate,
    CurrentFrame,
};

struct RTXPTDenoisingGuidesBakerStats
{
    bool   Ready                         = false;
    bool   LastBakeExecuted              = false;
    bool   LastDenoiseSpecHitTExecuted   = false;
    bool   LastAvgLayerRadianceExecuted  = false;
    bool   LastDebugVizExecuted          = false;
    Uint32 DenoiseSpecHitTDispatchCount  = 0;
    Uint32 AvgLayerRadianceDispatchCount = 0;
    Uint32 DebugVizDispatchCount         = 0;
};

class RTXPTDenoisingGuidesBaker
{
public:
    enum class PassId : Uint32
    {
        DenoiseSpecHitT = 0,
        ComputeAvgLayerRadiance,
        ComputeSpecularMotion,
        DebugViz,
        Count
    };

    void Reset();

    bool Initialize(IRenderDevice*     pDevice,
                    IEngineFactory*    pEngineFactory,
                    IRenderStateCache* pStateCache,
                    IBuffer*           pFrameConstants,
                    bool               ComputeSupported);

    bool Bake(IDeviceContext*              pContext,
              const RTXPTRenderTargets&    RenderTargets,
              RTXPTDenoisingGuideDebugView DebugView,
              RTXPTGuideHistoryMode        HistoryMode = RTXPTGuideHistoryMode::Accumulate);

    bool                                  IsReady() const { return m_Stats.Ready; }
    const RTXPTDenoisingGuidesBakerStats& GetStats() const { return m_Stats; }

private:
    struct PassState
    {
        RefCntAutoPtr<IPipelineState>         PSO;
        RefCntAutoPtr<IShaderResourceBinding> SRB;
    };

    bool CreatePass(IRenderDevice*                   pDevice,
                    IRenderStateCache*               pStateCache,
                    IShaderSourceInputStreamFactory* pShaderSourceFactory,
                    PassId                           Pass);
    bool DispatchPass(IDeviceContext*              pContext,
                      const RTXPTRenderTargets&    RenderTargets,
                      PassId                       Pass,
                      RTXPTDenoisingGuideDebugView DebugView,
                      Uint32                       Ping,
                      RTXPTGuideHistoryMode        HistoryMode);

    std::array<PassState, static_cast<size_t>(PassId::Count)> m_Passes;
    RefCntAutoPtr<IBuffer>                                    m_FrameConstants;
    RefCntAutoPtr<IBuffer>                                    m_Constants;
    RTXPTDenoisingGuidesBakerStats                            m_Stats;
};

} // namespace Diligent
