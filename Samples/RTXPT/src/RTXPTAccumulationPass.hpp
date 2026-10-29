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

#include "BasicMath.hpp"
#include "Buffer.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Sampler.h"
#include "ShaderResourceBinding.h"
#include "TextureView.h"

namespace Diligent
{

struct IRenderStateCache;
struct IPipelineStateCache;

struct RTXPTAccumulationPassStats
{
    bool   Ready                = false;
    bool   LastDispatchExecuted = false;
    Uint32 DispatchCount        = 0;
};

struct RTXPTAccumulationDispatch
{
    ITextureView* pInputColorSRV          = nullptr;
    ITextureView* pAccumulatedRadianceUAV = nullptr;
    ITextureView* pProcessedOutputUAV     = nullptr;
    Uint32        InputWidth              = 0;
    Uint32        InputHeight             = 0;
    Uint32        OutputWidth             = 0;
    Uint32        OutputHeight            = 0;
    float2        PixelOffset             = float2{0.0f, 0.0f};
    float         BlendFactor             = 1.0f;
};

class RTXPTAccumulationPass
{
public:
    void Reset();
    bool Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache, IPipelineStateCache* pPSOCache, bool ComputeSupported);
    bool Render(IDeviceContext* pContext, const RTXPTAccumulationDispatch& Dispatch);

    bool                              IsReady() const { return m_Stats.Ready; }
    const RTXPTAccumulationPassStats& GetStats() const { return m_Stats; }

private:
    RefCntAutoPtr<IPipelineState>         m_PSO;
    RefCntAutoPtr<IShaderResourceBinding> m_SRB;
    RefCntAutoPtr<IBuffer>                m_Constants;
    RefCntAutoPtr<ISampler>               m_LinearSampler;
    RTXPTAccumulationPassStats            m_Stats;
};

} // namespace Diligent
