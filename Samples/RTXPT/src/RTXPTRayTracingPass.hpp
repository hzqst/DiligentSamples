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
#include <vector>

#include "Buffer.h"
#include "BufferView.h"
#include "DeviceContext.h"
#include "DeviceObject.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Sampler.h"
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

// How the pass compiles its pipelines. The scene, lighting and environment resources are bound later with Bind.
struct RTXPTRayTracingPassCreateInfo
{
    IEngineFactory*      pEngineFactory  = nullptr;
    IRenderStateCache*   pStateCache     = nullptr;
    IPipelineStateCache* pPSOCache       = nullptr;
    IBuffer*             pFrameConstants = nullptr;
    VALUE_TYPE           IndexValueType  = VT_UNDEFINED;
    // The size of the bindless material-texture table the pipelines are compiled for. Bind requires a table of
    // exactly this size.
    Uint32 MaterialTextureCount         = 0;
    bool   EnableMaterialTextures       = false;
    bool   EnableAnyHit                 = false;
    bool   EnableLDSamplerForBSDF       = false;
    bool   RayTracingSupported          = false;
    bool   StandaloneRTShadersSupported = false;
    bool   EnableInlineVisibility       = false;
    Uint32 NEEMISType                   = 0;
    Uint32 NEECandidateSamples          = 0;
    Uint32 NEEFullSamples               = 0;
    bool   UseFp16Types                 = false;
    bool   EnableHitObject              = false;
    bool   EnableThreadReorder          = false;
    bool   EnableSharc                  = false;
};

// The scene, lighting and environment resources of the pass. Changing any object creates new resource bindings, so
// work recorded with the previous ones keeps its resources. TLASBuildIndex identifies the last build of pTLAS: the
// hit groups follow its instances, so the shader binding tables are refreshed whenever it changes.
struct RTXPTRayTracingBindings
{
    ITopLevelAS*          pTLAS                         = nullptr;
    Uint64                TLASBuildIndex                = 0;
    IBuffer*              pMaterialBuffer               = nullptr;
    IBuffer*              pSubInstanceBuffer            = nullptr;
    IBuffer*              pVertexBuffer                 = nullptr;
    IBuffer*              pSkinnedVertexBuffer          = nullptr;
    IBuffer*              pIndexBuffer                  = nullptr;
    IDeviceObject* const* pMaterialTextures             = nullptr;
    Uint32                MaterialTextureCount          = 0;
    IBuffer*              pLightBuffer                  = nullptr;
    IBuffer*              pLightExBuffer                = nullptr;
    IBuffer*              pLightingControlBuffer        = nullptr;
    IBuffer*              pLightProxyCounters           = nullptr;
    IBuffer*              pLightSamplingProxies         = nullptr;
    IBuffer*              pLocalSamplingBuffer          = nullptr;
    ITextureView*         pEnvironmentLightLookup       = nullptr;
    ITextureView*         pFeedbackTotalWeightUAV       = nullptr;
    ITextureView*         pFeedbackCandidatesUAV        = nullptr;
    ITextureView*         pEnvironmentMapSRV            = nullptr;
    ITextureView*         pEnvironmentImportanceMapSRV  = nullptr;
    ITextureView*         pEnvironmentRadianceMapSRV    = nullptr;
    ISampler*             pEnvironmentSampler           = nullptr;
    ISampler*             pEnvironmentImportanceSampler = nullptr;
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
    // Resource binding sets created by Bind, and shader binding table refreshes for new TLAS builds.
    Uint32 BindingCount    = 0;
    Uint32 SBTRefreshCount = 0;
};

class RTXPTRayTracingPass
{
public:
    void Reset();
    void UnbindSharcBuffers();

    // Compiles the pipelines. Dispatch needs a successful Bind first.
    bool Initialize(IRenderDevice* pDevice, IDeviceContext* pContext, const RTXPTRayTracingPassCreateInfo& CreateInfo);

    // Binds the scene, lighting and environment resources for the following dispatches. Repeating the bound objects
    // keeps the resource bindings; a new TLASBuildIndex only refreshes the shader binding tables.
    bool Bind(IDeviceContext* pContext, const RTXPTRayTracingBindings& Bindings);

    bool Dispatch(IDeviceContext*                pContext,
                  RTXPTPathTraceVariant          Variant,
                  const RTXPTRayTracingDispatch& Dispatch);

    bool Trace(IDeviceContext* pContext, const RTXPTRayTracingDispatch& DispatchInfo);

    static void InsertUAVBarrier(IDeviceContext* pContext, ITextureView* pTextureUAV);
    static void InsertUAVBarrier(IDeviceContext* pContext, IBuffer* pBuffer);
    static void InsertUAVBarrier(IDeviceContext* pContext, IBufferView* pBufferUAV);

    bool                               IsReady(RTXPTPathTraceVariant Variant) const;
    bool                               IsReady() const { return m_Stats.Ready; }
    const RTXPTRayTracingPassStats&    GetStats() const { return m_Stats; }
    IBuffer*                           GetMiniConstantsBuffer() const { return m_MiniConstantsCB; }
    const RTXPTRayTracingVariantStats& GetVariantStats(RTXPTPathTraceVariant Variant) const;

private:
    bool CreateBindings(const RTXPTRayTracingBindings& Bindings);

    struct VariantState
    {
        RefCntAutoPtr<IPipelineState>         PSO;
        RefCntAutoPtr<IShaderResourceBinding> SRB;
        RefCntAutoPtr<IShaderBindingTable>    SBT;
        RTXPTRayTracingVariantStats           Stats;
    };

    std::array<VariantState, static_cast<size_t>(RTXPTPathTraceVariant::Count)> m_Variants;
    RefCntAutoPtr<IBuffer>                                                      m_MiniConstantsCB;
    RefCntAutoPtr<IBuffer>                                                      m_FrameConstants;
    RefCntAutoPtr<IBufferView>                                                  m_IndexBufferView;
    RefCntAutoPtr<IBuffer>                                                      m_IndexBuffer;
    RTXPTRayTracingBindings                                                     m_Bound;
    std::vector<IDeviceObject*>                                                 m_BoundTextures;
    bool                                                                        m_HasBindings         = false;
    VALUE_TYPE                                                                  m_IndexValueType      = VT_UNDEFINED;
    bool                                                                        m_UseTextures         = false;
    bool                                                                        m_UseAnyHitTextures   = false;
    bool                                                                        m_UseHitObject        = false;
    bool                                                                        m_UseInlineVisibility = false;
    RTXPTRayTracingPassStats                                                    m_Stats;
};

} // namespace Diligent
