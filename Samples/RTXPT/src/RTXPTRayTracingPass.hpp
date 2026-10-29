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
#include "BufferView.h"
#include "DeviceContext.h"
#include "DeviceObject.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderBindingTable.h"
#include "ShaderResourceBinding.h"
#include "TextureView.h"
#include "TopLevelAS.h"

namespace Diligent
{

struct IRenderStateCache;
struct IPipelineStateCache;
struct SampleMiniConstants;

enum class RTXPTPathTraceVariant : Uint32
{
    Reference            = 0,
    BuildDenoisingLayers = 1,
    FillDenoisingLayers  = 2,
    SharcTrain,
    SharcFill,
    Count
};

struct RTXPTRayTracingDispatch
{
    IBufferView*               pSubSamplingPixelList     = nullptr;
    IBuffer*                   pSharcConstants           = nullptr;
    IBufferView*               pSharcHash                = nullptr;
    IBufferView*               pSharcAccumulation        = nullptr;
    IBufferView*               pSharcResolved            = nullptr;
    IBufferView*               pSharcPathState           = nullptr;
    ITextureView*              pOutputColorUAV           = nullptr;
    ITextureView*              pDepthUAV                 = nullptr;
    ITextureView*              pMotionVectorsUAV         = nullptr;
    ITextureView*              pThroughputUAV            = nullptr;
    ITextureView*              pSpecularHitTUAV          = nullptr;
    ITextureView*              pStableRadianceUAV        = nullptr;
    ITextureView*              pDenoisingLayersHeaderUAV = nullptr;
    IBufferView*               pDenoisingLayersBufferUAV = nullptr;
    const SampleMiniConstants* pMiniConstants            = nullptr;
    Uint32                     Width                     = 0;
    Uint32                     Height                    = 0;
};

struct RTXPTRayTracingVariantStats
{
    bool   Ready             = false;
    bool   LastTraceExecuted = false;
    Uint32 TraceCount        = 0;
};

struct RTXPTRayTracingPassStats
{
    bool   Ready                    = false;
    bool   LastTraceExecuted        = false;
    bool   MaterialBridgeBound      = false;
    bool   SubInstanceBound         = false;
    bool   LightBridgeBound         = false;
    bool   LightsBakerBridgeBound   = false;
    bool   EnvironmentBridgeBound   = false;
    bool   EmissiveLightBridgeBound = false;
    bool   VertexBufferBound        = false;
    bool   SkinnedVertexBufferBound = false;
    bool   IndexBufferBound         = false;
    bool   MaterialTexturesBound    = false;
    bool   AnyHitEnabled            = false;
    bool   InlineVisibilityEnabled  = false;
    Uint32 MaterialTextureCount     = 0;
    Uint32 TraceCount               = 0;
    Uint32 NEECandidateSamples      = 0;
    Uint32 NEEFullSamples           = 0;
};

class RTXPTRayTracingPass
{
public:
    void Reset();
    void UnbindSharcBuffers();

    bool Initialize(IRenderDevice*        pDevice,
                    IDeviceContext*       pContext,
                    IEngineFactory*       pEngineFactory,
                    IRenderStateCache*    pStateCache,
                    IPipelineStateCache*  pPSOCache,
                    IBuffer*              pFrameConstants,
                    IBuffer*              pMaterialBuffer,
                    IBuffer*              pSubInstanceBuffer,
                    IBuffer*              pLightBuffer,
                    IBuffer*              pLightExBuffer,
                    IBuffer*              pLightingControlBuffer,
                    IBuffer*              pLightProxyCounters,
                    IBuffer*              pLightSamplingProxies,
                    IBuffer*              pLocalSamplingBuffer,
                    ITextureView*         pEnvironmentLightLookup,
                    ITextureView*         pFeedbackTotalWeightUAV,
                    ITextureView*         pFeedbackCandidatesUAV,
                    ITextureView*         pEnvironmentMapSRV,
                    ITextureView*         pEnvironmentImportanceMapSRV,
                    ITextureView*         pEnvironmentRadianceMapSRV,
                    ISampler*             pEnvironmentSampler,
                    ISampler*             pEnvironmentImportanceSampler,
                    IBuffer*              pVertexBuffer,
                    IBuffer*              pSkinnedVertexBuffer,
                    IBuffer*              pIndexBuffer,
                    VALUE_TYPE            IndexValueType,
                    ITopLevelAS*          pTLAS,
                    IDeviceObject* const* pMaterialTextures,
                    Uint32                MaterialTextureCount,
                    bool                  EnableMaterialTextures,
                    bool                  EnableAnyHit,
                    bool                  EnableLDSamplerForBSDF,
                    bool                  RayTracingSupported,
                    bool                  StandaloneRTShadersSupported,
                    bool                  EnableInlineVisibility,
                    Uint32                NEEMISType,
                    Uint32                NEECandidateSamples,
                    Uint32                NEEFullSamples,
                    bool                  UseFp16Types,
                    bool                  EnableHitObject,
                    bool                  EnableThreadReorder,
                    bool                  EnableSharc = false);

    bool Dispatch(IDeviceContext*                pContext,
                  RTXPTPathTraceVariant          Variant,
                  const RTXPTRayTracingDispatch& Dispatch);

    bool Trace(IDeviceContext* pContext,
               ITextureView*   pOutputUAV,
               ITextureView*   pDepthUAV,
               ITextureView*   pScreenMotionVectorsUAV,
               Uint32          Width,
               Uint32          Height);

    static void InsertUAVBarrier(IDeviceContext* pContext, ITextureView* pTextureUAV);
    static void InsertUAVBarrier(IDeviceContext* pContext, IBuffer* pBuffer);
    static void InsertUAVBarrier(IDeviceContext* pContext, IBufferView* pBufferUAV);

    bool                               IsReady(RTXPTPathTraceVariant Variant) const;
    bool                               IsReady() const { return m_Stats.Ready; }
    const RTXPTRayTracingPassStats&    GetStats() const { return m_Stats; }
    IBuffer*                           GetMiniConstantsBuffer() const { return m_MiniConstantsCB; }
    const RTXPTRayTracingVariantStats& GetVariantStats(RTXPTPathTraceVariant Variant) const;

private:
    struct VariantState
    {
        RefCntAutoPtr<IPipelineState>         PSO;
        RefCntAutoPtr<IShaderResourceBinding> SRB;
        RefCntAutoPtr<IShaderBindingTable>    SBT;
        RTXPTRayTracingVariantStats           Stats;
    };

    std::array<VariantState, static_cast<size_t>(RTXPTPathTraceVariant::Count)> m_Variants;
    RefCntAutoPtr<IBuffer>                                                      m_MiniConstantsCB;
    RefCntAutoPtr<ITopLevelAS>                                                  m_TLAS;
    RefCntAutoPtr<IBufferView>                                                  m_IndexBufferView;
    RTXPTRayTracingPassStats                                                    m_Stats;
};

} // namespace Diligent
