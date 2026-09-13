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
#include "Shader.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

#include "RTXPTFrameConstants.hpp"
#include "RTXPTRealtimeSettings.hpp"
#include "RTXPTRenderTargets.hpp"

namespace Diligent
{

struct IRenderStateCache;

enum class RTXPTPostProcessPassId : Uint32
{
    StablePlanesDebugViz,
    RelaxDenoiserPrepareInputs,
    ReblurDenoiserPrepareInputs,
    RelaxDenoiserFinalMerge,
    ReblurDenoiserFinalMerge,
    NoDenoiserFinalMerge,
    DLSSRRDenoiserPrepareInputs,
    Count
};

struct RTXPTDenoiserPostProcessAttribs
{
    ITextureView*             pMergeOutputUAV = nullptr;
    const RTXPTRenderTargets* pRenderTargets  = nullptr;
    SampleMiniConstants       MiniConstants   = {};
    RTXPTNrdMethod            Method          = RTXPTNrdMethod::REBLUR;
    Uint32                    PlaneIndex      = 0;
    bool                      InitOutput      = false;
    bool                      HasValidation   = false;
    // RR prepare only: distinct render-size RGBA16_FLOAT UAVs, owned by the caller.
    ITextureView* pRRDiffuseAlbedoUAV       = nullptr;
    ITextureView* pRRSpecAlbedoUAV          = nullptr;
    ITextureView* pRRNormalsAndRoughnessUAV = nullptr;
};

struct RTXPTPostProcessPassStats
{
    bool   Ready                           = false;
    bool   LastStablePlanesDebugExecuted   = false;
    bool   LastDenoiserPrepareExecuted     = false;
    bool   LastDenoiserFinalMergeExecuted  = false;
    bool   LastNoDenoiserMergeExecuted     = false;
    Uint32 StablePlanesDebugDispatchCount  = 0;
    Uint32 DenoiserPrepareDispatchCount    = 0;
    Uint32 DenoiserFinalMergeDispatchCount = 0;
    Uint32 NoDenoiserMergeDispatchCount    = 0;
};

class RTXPTPostProcessPass
{
public:
    void Reset();
    bool Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache, IBuffer* pFrameConstants, bool ComputeSupported);
    bool RunStablePlanesDebugViz(IDeviceContext* pContext, const RTXPTDenoiserPostProcessAttribs& Attribs);
    bool RunDenoiserPrepare(IDeviceContext* pContext, const RTXPTDenoiserPostProcessAttribs& Attribs);
    bool RunDenoiserFinalMerge(IDeviceContext* pContext, const RTXPTDenoiserPostProcessAttribs& Attribs);
    bool RunNoDenoiserFinalMerge(IDeviceContext* pContext, const RTXPTDenoiserPostProcessAttribs& Attribs);
    bool RunDLSSRRDenoiserPrepareInputs(IDeviceContext* pContext, const RTXPTDenoiserPostProcessAttribs& Attribs);

    bool                             IsReady() const { return m_Stats.Ready; }
    const RTXPTPostProcessPassStats& GetStats() const { return m_Stats; }

private:
    struct PassState
    {
        RefCntAutoPtr<IPipelineState>         PSO;
        RefCntAutoPtr<IShaderResourceBinding> SRB;
    };

    bool CreatePostProcessPSO(IRenderDevice*          pDevice,
                              IRenderStateCache*      pStateCache,
                              const ShaderCreateInfo& BaseShaderCI,
                              RTXPTPostProcessPassId  Pass);
    bool DispatchPass(IDeviceContext*                        pContext,
                      RTXPTPostProcessPassId                 Pass,
                      const RTXPTDenoiserPostProcessAttribs& Attribs);

    std::array<PassState, static_cast<std::size_t>(RTXPTPostProcessPassId::Count)> m_Passes;
    RefCntAutoPtr<IBuffer>                                                         m_FrameConstants;
    RefCntAutoPtr<IBuffer>                                                         m_MiniConstants;
    RTXPTPostProcessPassStats                                                      m_Stats;
};

} // namespace Diligent
