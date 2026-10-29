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

#include "Buffer.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Sampler.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

#include "RTXPTFrameConstants.hpp"
#include "RTXPTNrdConfig.hpp"
#include "RTXPTRenderTargets.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Diligent
{

struct IRenderStateCache;

struct RTXPTNrdFrameAttribs
{
    const RTXPTRenderTargets*    pRenderTargets   = nullptr;
    const SampleConstants*       pFrameConstants  = nullptr;
    const RTXPTRealtimeSettings* pRealtime        = nullptr;
    Uint32                       PlaneIndex       = 0;
    Uint32                       FrameIndex       = 0;
    float                        TimeDeltaSeconds = -1.0f;
    bool                         ResetHistory     = false;
    bool                         EnableValidation = false;
};

struct RTXPTNrdIntegrationCreateInfo
{
    IEngineFactory*    pEngineFactory   = nullptr;
    IRenderStateCache* pStateCache      = nullptr;
    RTXPTNrdMethod     Method           = RTXPTNrdMethod::REBLUR;
    Uint32             Width            = 0;
    Uint32             Height           = 0;
    bool               ComputeSupported = false;
};

struct RTXPTNrdIntegrationStats
{
    bool           Ready                = false;
    bool           LastDispatchExecuted = false;
    Uint32         DispatchCount        = 0;
    Uint32         LastPlaneIndex       = 0;
    Uint32         LastDispatches       = 0;
    Uint32         Width                = 0;
    Uint32         Height               = 0;
    RTXPTNrdMethod Method               = RTXPTNrdMethod::REBLUR;
    std::string    LastFailureReason;
};

class RTXPTNrdIntegration
{
public:
    ~RTXPTNrdIntegration();

    void Reset();

    bool Initialize(IRenderDevice* pDevice, const RTXPTNrdIntegrationCreateInfo& CreateInfo);

    bool Dispatch(IDeviceContext* pContext, const RTXPTNrdFrameAttribs& Attribs);

    bool                            IsReady() const { return m_Stats.Ready; }
    RTXPTNrdMethod                  GetMethod() const { return m_Stats.Method; }
    Uint32                          GetWidth() const { return m_Stats.Width; }
    Uint32                          GetHeight() const { return m_Stats.Height; }
    const char*                     GetLastFailureReason() const { return m_Stats.LastFailureReason.c_str(); }
    const RTXPTNrdIntegrationStats& GetStats() const { return m_Stats; }

private:
    bool Fail(const char* Reason);

#if RTXPT_HAS_NRD
    struct PipelineState
    {
        RefCntAutoPtr<IPipelineState>               PSO;
        RefCntAutoPtr<IShaderResourceBinding>       SRB;
        std::vector<std::string>                    ConstantBufferNames;
        std::vector<std::pair<Uint32, std::string>> SamplerNames;
        std::vector<std::pair<Uint32, std::string>> TextureSRVNames;
        std::vector<std::pair<Uint32, std::string>> TextureUAVNames;
    };

    bool CreateInstance(RTXPTNrdMethod Method);
    bool CreateConstantBuffer(IRenderDevice* pDevice);
    bool CreateSamplers(IRenderDevice* pDevice);
    bool CreatePipelines(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache);
    bool CreatePoolTextures(IRenderDevice* pDevice, Uint32 Width, Uint32 Height);
    bool BindDispatchResources(IDeviceContext*             pContext,
                               const nrd::DispatchDesc&    DispatchDesc,
                               const nrd::PipelineDesc&    PipelineDesc,
                               PipelineState&              Pipeline,
                               const RTXPTNrdFrameAttribs& Attribs);
    void PopulateCommonSettings(nrd::CommonSettings& Settings, const RTXPTNrdFrameAttribs& Attribs) const;

    nrd::Instance*                       m_Instance   = nullptr;
    nrd::Identifier                      m_Identifier = 0;
    RefCntAutoPtr<IRenderDevice>         m_Device;
    RefCntAutoPtr<IBuffer>               m_ConstantBuffer;
    std::vector<PipelineState>           m_Pipelines;
    std::vector<RefCntAutoPtr<ISampler>> m_Samplers;
    std::vector<RefCntAutoPtr<ITexture>> m_PermanentTextures;
    std::vector<RefCntAutoPtr<ITexture>> m_TransientTextures;
#endif

    RTXPTNrdIntegrationStats m_Stats;
};

} // namespace Diligent
