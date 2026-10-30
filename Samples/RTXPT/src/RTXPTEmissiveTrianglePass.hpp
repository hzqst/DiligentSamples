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

#include <vector>

#include "Buffer.h"
#include "BufferView.h"
#include "DeviceContext.h"
#include "DeviceObject.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"

namespace Diligent
{

struct IRenderStateCache;
struct IPipelineStateCache;

struct RTXPTEmissiveTrianglePassStats
{
    bool   Ready                = false;
    bool   LastDispatchExecuted = false;
    Uint32 DispatchCount        = 0;
};

// How the pass compiles its pipeline. The scene resources are bound later with Bind.
struct RTXPTEmissiveTrianglePassCreateInfo
{
    IEngineFactory*      pEngineFactory = nullptr;
    IRenderStateCache*   pStateCache    = nullptr;
    IPipelineStateCache* pPSOCache      = nullptr;
    VALUE_TYPE           IndexValueType = VT_UNDEFINED;
    // The size of the bindless material-texture table the pipeline is compiled for.
    Uint32 MaterialTextureCount   = 0;
    bool   EnableMaterialTextures = false;
    bool   ComputeSupported       = false;
};

// The scene resources the pass reads and the emissive triangle buffer it writes. Changing any object creates a new
// resource binding, so dispatches recorded with the previous one keep their resources.
struct RTXPTEmissiveTriangleBindings
{
    IBuffer*              pMaterialBuffer             = nullptr;
    IBuffer*              pSubInstanceBuffer          = nullptr;
    IBuffer*              pSubInstanceTransformBuffer = nullptr;
    IBuffer*              pVertexBuffer               = nullptr;
    IBuffer*              pSkinnedVertexBuffer        = nullptr;
    IBuffer*              pIndexBuffer                = nullptr;
    IBuffer*              pEmissiveTriangleBuffer     = nullptr;
    IDeviceObject* const* pMaterialTextures           = nullptr;
    Uint32                MaterialTextureCount        = 0;
};

class RTXPTEmissiveTrianglePass
{
public:
    void Reset();

    bool Initialize(IRenderDevice* pDevice, const RTXPTEmissiveTrianglePassCreateInfo& CreateInfo);

    // Binds the resources of the following dispatches; repeating the bound objects keeps the resource binding.
    bool Bind(const RTXPTEmissiveTriangleBindings& Bindings);

    bool Dispatch(IDeviceContext* pContext, Uint32 SubInstanceCount);

    bool                                  IsReady() const { return m_Stats.Ready; }
    const RTXPTEmissiveTrianglePassStats& GetStats() const { return m_Stats; }

private:
    RefCntAutoPtr<IPipelineState>         m_PSO;
    RefCntAutoPtr<IShaderResourceBinding> m_SRB;
    RefCntAutoPtr<IBufferView>            m_IndexBufferView;
    RefCntAutoPtr<IBuffer>                m_IndexBuffer;
    RefCntAutoPtr<IBuffer>                m_Constants;
    RTXPTEmissiveTriangleBindings         m_Bound;
    std::vector<IDeviceObject*>           m_BoundTextures;
    VALUE_TYPE                            m_IndexValueType = VT_UNDEFINED;
    bool                                  m_UseTextures    = false;
    RTXPTEmissiveTrianglePassStats        m_Stats;
};

} // namespace Diligent
