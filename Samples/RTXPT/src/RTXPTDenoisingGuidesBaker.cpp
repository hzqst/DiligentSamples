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

#include "RTXPTDenoisingGuidesBaker.hpp"

#include "DebugUtilities.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"

#include "AsyncPrefix.hpp"

namespace Diligent
{

namespace
{

struct DenoisingGuidesBakerConstants
{
    uint2  RenderResolution  = uint2{0, 0};
    uint2  DisplayResolution = uint2{0, 0};
    Int32  DebugView         = 0;
    Uint32 Ping              = 0;
    Uint32 _Padding1         = 0;
    Uint32 _Padding2         = 0;
    uint4  _Padding3         = {};
    uint4  _Padding4         = {};
};
static_assert(sizeof(DenoisingGuidesBakerConstants) == 64, "DenoisingGuidesBakerConstants must match PathTracer/DenoisingGuidesBaker.hlsl");

constexpr Uint32 kThreadGroupSize = 8u;

const char* GetPassName(RTXPTDenoisingGuidesBaker::PassId Pass)
{
    switch (Pass)
    {
        case RTXPTDenoisingGuidesBaker::PassId::DenoiseSpecHitT: return "RTXPT DenoiseSpecHitT";
        case RTXPTDenoisingGuidesBaker::PassId::ComputeAvgLayerRadiance: return "RTXPT ComputeAvgLayerRadiance";
        case RTXPTDenoisingGuidesBaker::PassId::DebugViz: return "RTXPT DenoisingGuides DebugViz";
        default: return "RTXPT DenoisingGuides unknown pass";
    }
}

const char* GetEntryPoint(RTXPTDenoisingGuidesBaker::PassId Pass)
{
    switch (Pass)
    {
        case RTXPTDenoisingGuidesBaker::PassId::DenoiseSpecHitT: return "DenoiseSpecHitT";
        case RTXPTDenoisingGuidesBaker::PassId::ComputeAvgLayerRadiance: return "ComputeAvgLayerRadiance";
        case RTXPTDenoisingGuidesBaker::PassId::DebugViz: return "DebugViz";
        default: return "";
    }
}

void InsertUAVBarrier(IDeviceContext* pContext, ITextureView* pView)
{
    if (pContext == nullptr || pView == nullptr || pView->GetTexture() == nullptr)
        return;

    StateTransitionDesc Barrier{pView->GetTexture(),
                                RESOURCE_STATE_UNORDERED_ACCESS,
                                RESOURCE_STATE_UNORDERED_ACCESS,
                                STATE_TRANSITION_FLAG_UPDATE_STATE};
    pContext->TransitionResourceState(Barrier);
}

void InsertUAVBarrier(IDeviceContext* pContext, IBufferView* pView)
{
    if (pContext == nullptr || pView == nullptr || pView->GetBuffer() == nullptr)
        return;

    StateTransitionDesc Barrier{pView->GetBuffer(),
                                RESOURCE_STATE_UNORDERED_ACCESS,
                                RESOURCE_STATE_UNORDERED_ACCESS,
                                STATE_TRANSITION_FLAG_UPDATE_STATE};
    pContext->TransitionResourceState(Barrier);
}

} // namespace

void RTXPTDenoisingGuidesBaker::Reset()
{
    for (PassState& Pass : m_Passes)
    {
        Pass.PSO.Release();
        Pass.SRB.Release();
    }
    m_FrameConstants.Release();
    m_Constants.Release();
    m_Stats = {};
}

bool RTXPTDenoisingGuidesBaker::Initialize(IRenderDevice*     pDevice,
                                           IEngineFactory*    pEngineFactory,
                                           IRenderStateCache* pStateCache,
                                           IBuffer*           pFrameConstants,
                                           bool               ComputeSupported)
{
    Reset();

    if (!ComputeSupported)
    {
        DEV_ERROR("RTXPT denoising guides baker requires compute shader support");
        return false;
    }
    if (pDevice == nullptr || pEngineFactory == nullptr || pFrameConstants == nullptr)
    {
        DEV_ERROR("RTXPT denoising guides baker requires device, engine factory, and frame constants");
        return false;
    }

    m_FrameConstants = pFrameConstants;

    BufferDesc ConstantsDesc;
    ConstantsDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    ConstantsDesc.Name                 = "RTXPT denoising guides baker constants";
    ConstantsDesc.Size                 = sizeof(DenoisingGuidesBakerConstants);
    ConstantsDesc.BindFlags            = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage                = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags       = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_Constants);
    VERIFY(m_Constants, "Failed to create RTXPT denoising guides baker constants");
    if (!m_Constants)
        return false;

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    pEngineFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders\\PathTracer", &pShaderSourceFactory);
    if (!pShaderSourceFactory)
        return false;

    for (Uint32 Index = 0; Index < static_cast<Uint32>(PassId::Count); ++Index)
    {
        if (!CreatePass(pDevice, pStateCache, pShaderSourceFactory, static_cast<PassId>(Index)))
        {
            Reset();
            return false;
        }
    }

    m_Stats.Ready = true;
    return true;
}

bool RTXPTDenoisingGuidesBaker::CreatePass(IRenderDevice*                   pDevice,
                                           IRenderStateCache*               pStateCache,
                                           IShaderSourceInputStreamFactory* pShaderSourceFactory,
                                           PassId                           Pass)
{
    PassState& State = m_Passes[static_cast<size_t>(Pass)];

    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.Desc.Name                  = GetPassName(Pass);
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = "PathTracer/DenoisingGuidesBaker.hlsl";
    ShaderCI.EntryPoint                 = GetEntryPoint(Pass);
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    RefCntAutoPtr<IShader> pCS;
    pStateCache->CreateShader(ShaderCI, &pCS);
    VERIFY(pCS, "Failed to create RTXPT denoising guides shader");
    if (!pCS)
        return false;

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name                 = GetPassName(Pass);
    PSOCreateInfo.PSODesc.PipelineType         = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.PSODesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    PSOCreateInfo.pCS                          = pCS;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout
        .AddVariable(SHADER_TYPE_COMPUTE, "g_Const", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "g_DenoisingGuidesBakerConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_Depth", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_MotionVectors", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_SpecularHitT", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_ScratchFloat1", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_StableRadiance", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoisingLayersHeader", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoisingLayersBuffer", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoiserAvgLayerRadianceHalfRes", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DebugOutput", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    pStateCache->CreateComputePipelineState(PSOCreateInfo, &State.PSO);
    VERIFY(State.PSO, "Failed to create RTXPT denoising guides PSO");
    if (!State.PSO)
        return false;

    auto SetStatic = [&State](const char* Name, IDeviceObject* pObject) {
        IShaderResourceVariable* pVar = State.PSO->GetStaticVariableByName(SHADER_TYPE_COMPUTE, Name);
        if (pVar == nullptr)
            return true;
        if (pObject == nullptr)
            return false;
        pVar->Set(pObject, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
        return true;
    };

    if (!SetStatic("g_Const", m_FrameConstants) ||
        !SetStatic("g_DenoisingGuidesBakerConstants", m_Constants))
    {
        DEV_ERROR("Failed to bind RTXPT denoising guides static resources");
        return false;
    }

    State.PSO->CreateShaderResourceBinding(&State.SRB, true);
    VERIFY(State.SRB, "Failed to create RTXPT denoising guides SRB");
    return State.SRB != nullptr;
}

bool RTXPTDenoisingGuidesBaker::Bake(IDeviceContext*              pContext,
                                     const RTXPTRenderTargets&    RenderTargets,
                                     RTXPTDenoisingGuideDebugView DebugView)
{
    m_Stats.LastBakeExecuted             = false;
    m_Stats.LastDenoiseSpecHitTExecuted  = false;
    m_Stats.LastAvgLayerRadianceExecuted = false;
    m_Stats.LastDebugVizExecuted         = false;

    if (!IsReady() || pContext == nullptr || !RenderTargets.HasRealtimeRenderTargets())
        return false;

    if (!DispatchPass(pContext, RenderTargets, PassId::DenoiseSpecHitT, RTXPTDenoisingGuideDebugView::Disabled, 1u))
        return false;
    InsertUAVBarrier(pContext, RenderTargets.GetScratchFloat1UAV());

    if (!DispatchPass(pContext, RenderTargets, PassId::DenoiseSpecHitT, RTXPTDenoisingGuideDebugView::Disabled, 0u))
        return false;
    InsertUAVBarrier(pContext, RenderTargets.GetSpecularHitTUAV());

    if (!DispatchPass(pContext, RenderTargets, PassId::ComputeAvgLayerRadiance, RTXPTDenoisingGuideDebugView::Disabled, 0u))
        return false;
    InsertUAVBarrier(pContext, RenderTargets.GetDenoiserAvgLayerRadianceHalfResUAV());

    m_Stats.LastDenoiseSpecHitTExecuted = true;
    m_Stats.DenoiseSpecHitTDispatchCount += 2u;
    m_Stats.LastAvgLayerRadianceExecuted = true;
    ++m_Stats.AvgLayerRadianceDispatchCount;

    if (DebugView != RTXPTDenoisingGuideDebugView::Disabled)
    {
        if (!DispatchPass(pContext, RenderTargets, PassId::DebugViz, DebugView, 0u))
            return false;
        InsertUAVBarrier(pContext, RenderTargets.GetProcessedOutputColorUAV());
        m_Stats.LastDebugVizExecuted = true;
        ++m_Stats.DebugVizDispatchCount;
    }

    m_Stats.LastBakeExecuted = true;
    return true;
}

bool RTXPTDenoisingGuidesBaker::DispatchPass(IDeviceContext*              pContext,
                                             const RTXPTRenderTargets&    RenderTargets,
                                             PassId                       Pass,
                                             RTXPTDenoisingGuideDebugView DebugView,
                                             Uint32                       Ping)
{
    PassState& State = m_Passes[static_cast<size_t>(Pass)];
    if (!State.PSO || !State.SRB || pContext == nullptr)
        return false;

    const Uint32 RenderWidth  = RenderTargets.GetRenderWidth();
    const Uint32 RenderHeight = RenderTargets.GetRenderHeight();
    if (RenderWidth == 0 || RenderHeight == 0)
        return false;

    DenoisingGuidesBakerConstants Constants;
    Constants.RenderResolution  = uint2{RenderWidth, RenderHeight};
    Constants.DisplayResolution = uint2{RenderTargets.GetDisplayWidth(), RenderTargets.GetDisplayHeight()};
    Constants.DebugView         = static_cast<Int32>(DebugView);
    Constants.Ping              = Ping;

    {
        MapHelper<DenoisingGuidesBakerConstants> Mapped{pContext, m_Constants, MAP_WRITE, MAP_FLAG_DISCARD};
        VERIFY(Mapped, "Failed to map RTXPT denoising guides baker constants");
        if (!Mapped)
            return false;
        *Mapped = Constants;
    }

    auto SetVariable = [&State, Pass](const char* Name, IDeviceObject* pObject, bool Required) {
        IShaderResourceVariable* pVar = State.SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name);
        if (pVar == nullptr)
        {
            if (Required)
                UNEXPECTED("RTXPT denoising guides dynamic shader variable is missing: ", Name, " for pass: ", GetPassName(Pass));
            return !Required;
        }
        if (pObject == nullptr)
        {
            if (Required)
            {
                DEV_ERROR("RTXPT denoising guides dynamic resource object is null: ", Name, " for pass: ", GetPassName(Pass));
                return false;
            }
            return true;
        }
        pVar->Set(pObject);
        return true;
    };

    const bool NeedsDenoiseSpecHitT = Pass == PassId::DenoiseSpecHitT;
    const bool NeedsAvgLayer        = Pass == PassId::ComputeAvgLayerRadiance;
    const bool NeedsDebugViz        = Pass == PassId::DebugViz;

    const bool Bound =
        SetVariable("t_Depth", RenderTargets.GetDepthSRV(), NeedsDenoiseSpecHitT || NeedsDebugViz) &&
        SetVariable("t_MotionVectors", RenderTargets.GetScreenMotionVectorsSRV(), NeedsAvgLayer || NeedsDebugViz) &&
        SetVariable("u_SpecularHitT", RenderTargets.GetSpecularHitTUAV(), NeedsDenoiseSpecHitT || NeedsDebugViz) &&
        SetVariable("u_ScratchFloat1", RenderTargets.GetScratchFloat1UAV(), NeedsDenoiseSpecHitT) &&
        // Stable radiance is carried by DenoisingLayersContext, but this baker does not read it.
        // Some shader compilers optimize the UAV out of reflection for these entry points.
        SetVariable("u_StableRadiance", RenderTargets.GetStableRadianceUAV(), false) &&
        SetVariable("u_DenoisingLayersHeader", RenderTargets.GetDenoisingLayersHeaderUAV(), NeedsAvgLayer) &&
        SetVariable("u_DenoisingLayersBuffer", RenderTargets.GetDenoisingLayersBufferUAV(), NeedsAvgLayer) &&
        SetVariable("u_DenoiserAvgLayerRadianceHalfRes", RenderTargets.GetDenoiserAvgLayerRadianceHalfResUAV(), NeedsAvgLayer || NeedsDebugViz) &&
        SetVariable("u_DebugOutput", RenderTargets.GetProcessedOutputColorUAV(), NeedsDebugViz);
    if (!Bound)
    {
        DEV_ERROR("Failed to bind RTXPT denoising guides dynamic resources");
        return false;
    }

    pContext->SetPipelineState(State.PSO);
    pContext->CommitShaderResources(State.SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    const bool   HalfResolutionDispatch = Pass == PassId::ComputeAvgLayerRadiance;
    const Uint32 DispatchWidth          = HalfResolutionDispatch ? (RenderWidth + 1u) / 2u : RenderWidth;
    const Uint32 DispatchHeight         = HalfResolutionDispatch ? (RenderHeight + 1u) / 2u : RenderHeight;

    DispatchComputeAttribs DispatchAttribs;
    DispatchAttribs.ThreadGroupCountX = (DispatchWidth + kThreadGroupSize - 1u) / kThreadGroupSize;
    DispatchAttribs.ThreadGroupCountY = (DispatchHeight + kThreadGroupSize - 1u) / kThreadGroupSize;
    DispatchAttribs.ThreadGroupCountZ = 1;
    pContext->DispatchCompute(DispatchAttribs);

    return true;
}

} // namespace Diligent
