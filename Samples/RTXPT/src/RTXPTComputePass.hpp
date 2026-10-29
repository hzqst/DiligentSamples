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

#include "Buffer.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "TextureView.h"

namespace Diligent
{

struct IRenderStateCache;

struct RTXPTComputePassStats
{
    bool   Ready                = false;
    bool   LastDispatchExecuted = false;
    Uint32 DispatchCount        = 0;
};

struct RTXPTComputePassCreateInfo
{
    IEngineFactory*    pEngineFactory   = nullptr;
    IRenderStateCache* pStateCache      = nullptr;
    const char*        PassName         = nullptr;
    const char*        ShaderFilePath   = nullptr;
    IBuffer*           pFrameConstants  = nullptr;
    bool               ComputeSupported = false;
};

class RTXPTComputePass
{
public:
    void Reset();

    bool Initialize(IRenderDevice* pDevice, const RTXPTComputePassCreateInfo& CreateInfo);

    bool Dispatch(IDeviceContext* pContext, ITextureView* pInputSRV, ITextureView* pOutputUAV, Uint32 Width, Uint32 Height);

    bool                         IsReady() const { return m_Stats.Ready; }
    const RTXPTComputePassStats& GetStats() const { return m_Stats; }
    const std::string&           GetName() const { return m_Name; }

private:
    RefCntAutoPtr<IPipelineState>         m_PSO;
    RefCntAutoPtr<IShaderResourceBinding> m_SRB;
    RTXPTComputePassStats                 m_Stats;
    std::string                           m_Name;
};

} // namespace Diligent
