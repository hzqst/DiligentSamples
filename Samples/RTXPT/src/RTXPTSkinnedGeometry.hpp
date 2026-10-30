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
#include "ShaderResourceBinding.h"
#include "RTXPTSceneRuntime.hpp"

namespace Diligent
{

struct IRenderStateCache;
struct IPipelineStateCache;

// The buffers of skinning dispatches: source vertices (RTXPTGeometryVertex), skin influences (RTXPTSkinVertex), the
// joint palette (float4x4) and the skinned vertices written.
struct RTXPTSkinningBindings
{
    IBuffer* pSourceVertices  = nullptr;
    IBuffer* pSkinData        = nullptr;
    IBuffer* pJointMatrices   = nullptr;
    IBuffer* pSkinnedVertices = nullptr;
};

// One skinned vertex range. Element offsets into the bound buffers; NormalSkinBase equals SkinBase when normals follow
// the position influences.
struct RTXPTSkinningDispatch
{
    Uint32 SourceVertexBase = 0;
    Uint32 DestVertexBase   = 0;
    Uint32 JointBase        = 0;
    Uint32 VertexCount      = 0;
    Uint32 SkinBase         = 0;
    Uint32 NormalSkinBase   = 0;
};

// Skins vertex ranges on the GPU with SkinnedVertexBuild.csh.
class RTXPTSkinningPass
{
public:
    void Reset();
    bool Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache, IPipelineStateCache* pPSOCache);
    // Binds the buffers of the following dispatches; repeating the bound objects keeps the resource binding.
    bool Bind(const RTXPTSkinningBindings& Bindings);
    bool Dispatch(IDeviceContext* pContext, const RTXPTSkinningDispatch& Range);

    bool   IsReady() const { return m_PSO != nullptr; }
    Uint64 GetDispatchCount() const { return m_DispatchCount; }

private:
    struct Constants
    {
        RTXPTSkinningDispatch Range;
        Uint32                Padding[2] = {};
    };
    static_assert(sizeof(Constants) == 32, "Constants must match cbSkinningConstants in SkinnedVertexBuild.csh");

    RefCntAutoPtr<IPipelineState>         m_PSO;
    RefCntAutoPtr<IShaderResourceBinding> m_SRB;
    RefCntAutoPtr<IBuffer>                m_Constants;
    RTXPTSkinningBindings                 m_Bound;
    Uint64                                m_DispatchCount = 0;
};

} // namespace Diligent
