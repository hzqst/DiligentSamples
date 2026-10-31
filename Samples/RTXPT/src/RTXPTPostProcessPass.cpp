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

#include "RTXPTPostProcessPass.hpp"
#include "RTXPTShaderSource.hpp"

#include "DebugUtilities.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"
#include "ShaderMacroHelper.hpp"

#include <string>

#include "AsyncPrefix.hpp"

#ifndef RTXPT_HAS_NRD
#    define RTXPT_HAS_NRD 0
#endif

namespace Diligent
{

namespace
{

constexpr Uint32 kThreadGroupSize = 8;

const char* GetPassName(RTXPTPostProcessPassId Pass)
{
    switch (Pass)
    {
        case RTXPTPostProcessPassId::DenoisingLayersDebugViz: return "DenoisingLayersDebugViz";
        case RTXPTPostProcessPassId::RelaxDenoiserPrepareInputs: return "RelaxDenoiserPrepareInputs";
        case RTXPTPostProcessPassId::ReblurDenoiserPrepareInputs: return "ReblurDenoiserPrepareInputs";
        case RTXPTPostProcessPassId::RelaxDenoiserFinalMerge: return "RelaxDenoiserFinalMerge";
        case RTXPTPostProcessPassId::ReblurDenoiserFinalMerge: return "ReblurDenoiserFinalMerge";
        case RTXPTPostProcessPassId::NoDenoiserFinalMerge: return "NoDenoiserFinalMerge";
        case RTXPTPostProcessPassId::DLSSRRDenoiserPrepareInputs: return "DLSSRRDenoiserPrepareInputs";
        case RTXPTPostProcessPassId::Count: break;
    }
    return "Unknown";
}

const char* GetModeMacro(RTXPTPostProcessPassId Pass)
{
    switch (Pass)
    {
        case RTXPTPostProcessPassId::DenoisingLayersDebugViz: return "RTXPT_POST_PROCESS_DENOISING_LAYERS_DEBUG_VIZ";
        case RTXPTPostProcessPassId::RelaxDenoiserPrepareInputs: return "RTXPT_POST_PROCESS_RELAX_PREPARE_INPUTS";
        case RTXPTPostProcessPassId::ReblurDenoiserPrepareInputs: return "RTXPT_POST_PROCESS_REBLUR_PREPARE_INPUTS";
        case RTXPTPostProcessPassId::RelaxDenoiserFinalMerge: return "RTXPT_POST_PROCESS_RELAX_FINAL_MERGE";
        case RTXPTPostProcessPassId::ReblurDenoiserFinalMerge: return "RTXPT_POST_PROCESS_REBLUR_FINAL_MERGE";
        case RTXPTPostProcessPassId::NoDenoiserFinalMerge: return "RTXPT_POST_PROCESS_NO_DENOISER_FINAL_MERGE";
        case RTXPTPostProcessPassId::DLSSRRDenoiserPrepareInputs: return "RTXPT_POST_PROCESS_DLSS_RR_PREPARE_INPUTS";
        case RTXPTPostProcessPassId::Count: break;
    }
    return "0";
}

bool IsDenoiserPreparePass(RTXPTPostProcessPassId Pass)
{
    return Pass == RTXPTPostProcessPassId::RelaxDenoiserPrepareInputs ||
        Pass == RTXPTPostProcessPassId::ReblurDenoiserPrepareInputs;
}

bool IsDenoiserFinalMergePass(RTXPTPostProcessPassId Pass)
{
    return Pass == RTXPTPostProcessPassId::RelaxDenoiserFinalMerge ||
        Pass == RTXPTPostProcessPassId::ReblurDenoiserFinalMerge;
}

bool SetStaticVariable(IPipelineState*        pPSO,
                       SHADER_TYPE            ShaderType,
                       const char*            Name,
                       IDeviceObject*         pObject,
                       bool                   Required,
                       RTXPTPostProcessPassId Pass,
                       const char*            ResourceName)
{
    IShaderResourceVariable* pVar = pPSO != nullptr ? pPSO->GetStaticVariableByName(ShaderType, Name) : nullptr;
    if (pVar == nullptr)
    {
        if (Required)
            UNEXPECTED("RTXPT static shader variable is missing: ", ResourceName, " (", Name, ") for pass: ", GetPassName(Pass));
        return !Required;
    }
    if (pObject == nullptr)
    {
        if (Required)
            DEV_ERROR("RTXPT static resource object is null: ", ResourceName, " (", Name, ") for pass: ", GetPassName(Pass));
        return !Required;
    }

    pVar->Set(pObject, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    return true;
}

bool SetSRBVariable(IShaderResourceBinding* pSRB,
                    SHADER_TYPE             ShaderType,
                    const char*             Name,
                    IDeviceObject*          pObject,
                    bool                    Required,
                    RTXPTPostProcessPassId  Pass,
                    const char*             ResourceName)
{
    IShaderResourceVariable* pVar = pSRB != nullptr ? pSRB->GetVariableByName(ShaderType, Name) : nullptr;
    if (pVar == nullptr)
    {
        if (Required)
            UNEXPECTED("RTXPT dynamic shader variable is missing: ", ResourceName, " (", Name, ") for pass: ", GetPassName(Pass));
        return !Required;
    }
    if (pObject == nullptr)
    {
        if (Required)
            DEV_ERROR("RTXPT dynamic resource object is null: ", ResourceName, " (", Name, ") for pass: ", GetPassName(Pass));
        return !Required;
    }

    pVar->Set(pObject);
    return true;
}

bool UpdateMiniConstantsBuffer(IDeviceContext*                        pContext,
                               IBuffer*                               pMiniConstants,
                               RTXPTPostProcessPassId                 Pass,
                               const RTXPTDenoiserPostProcessAttribs& Attribs)
{
    SampleMiniConstants MiniConstants = Attribs.MiniConstants;
    // Stable-plane debug viz uses params.x == 0 as the shader-side four-way split convention.
    MiniConstants.params.x = Attribs.PlaneIndex;
    if (IsDenoiserPreparePass(Pass))
        MiniConstants.params.y = Attribs.InitOutput ? 1u : 0u;
    else if (IsDenoiserFinalMergePass(Pass))
        MiniConstants.params.y = Attribs.HasValidation ? 1u : 0u;

    MapHelper<SampleMiniConstants> Mapped{pContext, pMiniConstants, MAP_WRITE, MAP_FLAG_DISCARD};
    if (Mapped == nullptr)
    {
        DEV_ERROR("Failed to map RTXPT post-process SampleMiniConstants");
        return false;
    }
    *Mapped = MiniConstants;
    return true;
}

bool BindDispatchResources(IShaderResourceBinding*                pSRB,
                           RTXPTPostProcessPassId                 Pass,
                           const RTXPTDenoiserPostProcessAttribs& Attribs)
{
    const RTXPTRenderTargets& RenderTargets  = *Attribs.pRenderTargets;
    const bool                PreparePass    = IsDenoiserPreparePass(Pass);
    const bool                FinalMergePass = IsDenoiserFinalMergePass(Pass);
    const bool                RRPreparePass  = Pass == RTXPTPostProcessPassId::DLSSRRDenoiserPrepareInputs;
    const bool                DenoisingLayersRequired =
        PreparePass || RRPreparePass ||
        Pass == RTXPTPostProcessPassId::DenoisingLayersDebugViz ||
        Pass == RTXPTPostProcessPassId::NoDenoiserFinalMerge;
    const bool StableBufferRequired = DenoisingLayersRequired || FinalMergePass;

    ITextureView* pDenoiserOutDiffRadianceHitDistSRV = RenderTargets.GetDenoiserOutDiffRadianceHitDistSRV(Attribs.PlaneIndex);
    ITextureView* pDenoiserOutValidationSRV          = RenderTargets.GetDenoiserOutValidationSRV();
    if (FinalMergePass && !Attribs.HasValidation && pDenoiserOutValidationSRV == nullptr)
    {
        // The shader reflects t_DenoiserOutValidation even when validation is disabled at runtime.
        pDenoiserOutValidationSRV = pDenoiserOutDiffRadianceHitDistSRV;
    }

    return SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_RRDiffuseAlbedo", Attribs.pRRDiffuseAlbedoUAV, RRPreparePass, Pass, "RR diffuse albedo UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_RRSpecAlbedo", Attribs.pRRSpecAlbedoUAV, RRPreparePass, Pass, "RR specular albedo UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_RRNormalsAndRoughness", Attribs.pRRNormalsAndRoughnessUAV, RRPreparePass, Pass, "RR normals and roughness UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_RRDepth", Attribs.pRRDepthUAV, RRPreparePass, Pass, "RR view depth UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_RRMotionVectors", Attribs.pRRMotionVectorsUAV, RRPreparePass, Pass, "RR pixel motion UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_RRSpecMotionVectors", RenderTargets.GetRRSpecMotionVectorsUAV(), RRPreparePass, Pass, "RR specular pixel motion UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "t_Depth", RenderTargets.GetDepthSRV(), RRPreparePass, Pass, "depth SRV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "t_MotionVectors", RenderTargets.GetScreenMotionVectorsSRV(), RRPreparePass, Pass, "motion vectors SRV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "t_SpecularHitT", RenderTargets.GetSpecularHitTSRV(), PreparePass, Pass, "specular hit T SRV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "t_DenoiserOutDiffRadianceHitDist", pDenoiserOutDiffRadianceHitDistSRV, FinalMergePass, Pass, "denoiser output diffuse radiance hit distance SRV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "t_DenoiserOutSpecRadianceHitDist", RenderTargets.GetDenoiserOutSpecRadianceHitDistSRV(Attribs.PlaneIndex), FinalMergePass, Pass, "denoiser output specular radiance hit distance SRV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "t_DenoiserOutValidation", pDenoiserOutValidationSRV, FinalMergePass, Pass, "denoiser validation SRV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "t_DenoiserViewspaceZ", RenderTargets.GetDenoiserViewspaceZSRV(), FinalMergePass, Pass, "denoiser viewspace Z SRV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "t_DenoiserDisocclusionThresholdMix", RenderTargets.GetDenoiserDisocclusionThresholdMixSRV(), false, Pass, "denoiser disocclusion threshold mix SRV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_OutputColor", Attribs.pMergeOutputUAV, true, Pass, "merge output UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_StableRadiance", RenderTargets.GetStableRadianceUAV(), DenoisingLayersRequired, Pass, "stable radiance UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_DenoisingLayersHeader", RenderTargets.GetDenoisingLayersHeaderUAV(), DenoisingLayersRequired, Pass, "stable planes header UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_DenoisingLayersBuffer", RenderTargets.GetDenoisingLayersBufferUAV(), StableBufferRequired, Pass, "stable planes buffer UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_DenoiserViewspaceZ", RenderTargets.GetDenoiserViewspaceZUAV(), PreparePass, Pass, "denoiser viewspace Z UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_DenoiserMotionVectors", RenderTargets.GetDenoiserMotionVectorsUAV(), PreparePass, Pass, "denoiser motion vectors UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_DenoiserNormalRoughness", RenderTargets.GetDenoiserNormalRoughnessUAV(), PreparePass, Pass, "denoiser normal roughness UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_DenoiserDiffRadianceHitDist", RenderTargets.GetDenoiserDiffRadianceHitDistUAV(), PreparePass, Pass, "denoiser diffuse radiance hit distance UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_DenoiserSpecRadianceHitDist", RenderTargets.GetDenoiserSpecRadianceHitDistUAV(), PreparePass, Pass, "denoiser specular radiance hit distance UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_DenoiserDisocclusionThresholdMix", RenderTargets.GetDenoiserDisocclusionThresholdMixUAV(), PreparePass, Pass, "denoiser disocclusion threshold mix UAV") &&
        SetSRBVariable(pSRB, SHADER_TYPE_COMPUTE, "u_CombinedHistoryClampRelax", RenderTargets.GetCombinedHistoryClampRelaxUAV(), PreparePass, Pass, "combined history clamp relax UAV");
}

} // namespace

void RTXPTPostProcessPass::Reset()
{
    for (auto& State : m_Passes)
    {
        State.PSO.Release();
        State.SRB.Release();
    }
    m_FrameConstants.Release();
    m_MiniConstants.Release();
    m_Stats = {};
}

bool RTXPTPostProcessPass::Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache, IBuffer* pFrameConstants, bool ComputeSupported, RTXPTPostProcessPassSet PassSet)
{
    Reset();

    if (!ComputeSupported)
    {
        DEV_ERROR("RTXPT post-process pass requires compute shader support");
        return false;
    }

    if (pDevice == nullptr)
    {
        DEV_ERROR("RTXPT post-process pass requires a render device");
        return false;
    }

    if (pEngineFactory == nullptr)
    {
        DEV_ERROR("RTXPT post-process pass requires an engine factory");
        return false;
    }

    if (pFrameConstants == nullptr)
    {
        DEV_ERROR("RTXPT post-process pass requires frame constants");
        return false;
    }

    m_FrameConstants = pFrameConstants;

    std::string ShaderSearchPaths = "shaders;shaders\\PostProcessing;shaders\\PathTracer";
#if RTXPT_HAS_NRD
    const auto AppendShaderSearchPath = [&ShaderSearchPaths](const char* Path) {
        if (Path == nullptr || Path[0] == '\0')
            return;

        ShaderSearchPaths += ";";
        ShaderSearchPaths += Path;
    };

    AppendShaderSearchPath(RTXPT_NRD_SHADER_INCLUDE_DIR);
    AppendShaderSearchPath(RTXPT_NRD_SHADER_CONFIG_DIR);
    AppendShaderSearchPath(RTXPT_NRD_MATHLIB_INCLUDE_DIR);
#endif

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    CreateRTXPTShaderSourceFactory(pEngineFactory, ShaderSearchPaths.c_str(), &pShaderSourceFactory);
    if (!pShaderSourceFactory)
    {
        DEV_ERROR("Failed to create RTXPT post-process shader source factory");
        return false;
    }

    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = "PostProcessing/RTXPTPostProcess.csh";
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    BufferDesc MiniConstantsDesc;
    MiniConstantsDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    MiniConstantsDesc.Name                 = "RTXPT post-process SampleMiniConstants";
    MiniConstantsDesc.Size                 = sizeof(SampleMiniConstants);
    MiniConstantsDesc.BindFlags            = BIND_UNIFORM_BUFFER;
    MiniConstantsDesc.Usage                = USAGE_DYNAMIC;
    MiniConstantsDesc.CPUAccessFlags       = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(MiniConstantsDesc, nullptr, &m_MiniConstants);
    VERIFY(m_MiniConstants, "Failed to create RTXPT post-process SampleMiniConstants");
    if (!m_MiniConstants)
        return false;

    for (Uint32 PassIndex = 0; PassIndex < static_cast<Uint32>(RTXPTPostProcessPassId::Count); ++PassIndex)
    {
        const auto Pass = static_cast<RTXPTPostProcessPassId>(PassIndex);
        if (PassSet == RTXPTPostProcessPassSet::RealtimeRaw && Pass != RTXPTPostProcessPassId::NoDenoiserFinalMerge)
            continue;
        if (!CreatePostProcessPSO(pDevice, pStateCache, ShaderCI, Pass))
        {
            DEV_ERROR("Failed to create RTXPT post-process pipeline: ", GetPassName(Pass));
            Reset();
            return false;
        }
    }

    m_Stats.Ready = true;
    return true;
}

bool RTXPTPostProcessPass::CreatePostProcessPSO(IRenderDevice*          pDevice,
                                                IRenderStateCache*      pStateCache,
                                                const ShaderCreateInfo& BaseShaderCI,
                                                RTXPTPostProcessPassId  Pass)
{
    const auto PassIndex = static_cast<std::size_t>(Pass);
    if (PassIndex >= m_Passes.size())
        return false;

    PassState& State = m_Passes[PassIndex];
    State.PSO.Release();
    State.SRB.Release();

    ShaderMacroHelper Macros;
    Macros.Add("RTXPT_POST_PROCESS_MODE", GetModeMacro(Pass));
    Macros.Add("RTXPT_HAS_NRD_HEADERS", RTXPT_HAS_NRD && Pass != RTXPTPostProcessPassId::NoDenoiserFinalMerge ? 1 : 0);
#if RTXPT_HAS_NRD
    Macros.Add("NRD_NORMAL_ENCODING", 2);
    Macros.Add("NRD_ROUGHNESS_ENCODING", 1);
#endif

    ShaderCreateInfo ShaderCI = BaseShaderCI;
    ShaderCI.Desc.Name        = GetPassName(Pass);
    if (Pass == RTXPTPostProcessPassId::DLSSRRDenoiserPrepareInputs)
    {
        Macros.Add("RTXPT_PREPARE_DLSS_RR", 1);
        ShaderCI.FilePath   = "PathTracer/DenoisingGuidesBaker.hlsl";
        ShaderCI.EntryPoint = "PrepareDLSSRRGuides";
    }
    ShaderCI.Macros = Macros;

    RefCntAutoPtr<IShader> pCS;
    if (pStateCache != nullptr)
        pStateCache->CreateShader(ShaderCI, &pCS);
    else
        pDevice->CreateShader(ShaderCI, &pCS);
    VERIFY(pCS, "Failed to create shader: ", GetPassName(Pass));
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
        .AddVariable(SHADER_TYPE_COMPUTE, "g_MiniConst", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_Depth", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_MotionVectors", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_SpecularHitT", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_DenoiserOutDiffRadianceHitDist", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_DenoiserOutSpecRadianceHitDist", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_DenoiserOutValidation", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_DenoiserViewspaceZ", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_DenoiserDisocclusionThresholdMix", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_OutputColor", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_RRDiffuseAlbedo", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_RRSpecAlbedo", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_RRNormalsAndRoughness", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_RRDepth", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_RRMotionVectors", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_RRSpecMotionVectors", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_StableRadiance", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoisingLayersHeader", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoisingLayersBuffer", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoiserViewspaceZ", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoiserMotionVectors", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoiserNormalRoughness", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoiserDiffRadianceHitDist", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoiserSpecRadianceHitDist", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_DenoiserDisocclusionThresholdMix", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_CombinedHistoryClampRelax", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    if (pStateCache != nullptr)
        pStateCache->CreateComputePipelineState(PSOCreateInfo, &State.PSO);
    else
        pDevice->CreateComputePipelineState(PSOCreateInfo, &State.PSO);
    VERIFY(State.PSO, "Failed to create RTXPT post-process PSO: ", GetPassName(Pass));
    if (!State.PSO)
        return false;

    const bool MiniConstantsRequired =
        IsDenoiserPreparePass(Pass) ||
        IsDenoiserFinalMergePass(Pass) ||
        Pass == RTXPTPostProcessPassId::DenoisingLayersDebugViz;
    const bool StaticBound =
        SetStaticVariable(State.PSO, SHADER_TYPE_COMPUTE, "g_Const", m_FrameConstants, true, Pass, "frame constants") &&
        SetStaticVariable(State.PSO, SHADER_TYPE_COMPUTE, "g_MiniConst", m_MiniConstants, MiniConstantsRequired, Pass, "mini constants");
    if (!StaticBound)
    {
        DEV_ERROR("RTXPT post-process pass failed to bind static constants for ", GetPassName(Pass));
        return false;
    }

    State.PSO->CreateShaderResourceBinding(&State.SRB, true);
    VERIFY(State.SRB, "Failed to create RTXPT post-process SRB: ", GetPassName(Pass));
    return State.SRB != nullptr;
}

bool RTXPTPostProcessPass::DispatchPass(IDeviceContext*                        pContext,
                                        RTXPTPostProcessPassId                 Pass,
                                        const RTXPTDenoiserPostProcessAttribs& Attribs)
{
    const auto PassIndex = static_cast<std::size_t>(Pass);
    if (PassIndex >= m_Passes.size())
        return false;

    const PassState&          State          = m_Passes[PassIndex];
    const RTXPTRenderTargets* pRenderTargets = Attribs.pRenderTargets;
    if (!IsReady() || pContext == nullptr ||
        pRenderTargets == nullptr ||
        Attribs.pMergeOutputUAV == nullptr ||
        pRenderTargets->GetRenderWidth() == 0 ||
        pRenderTargets->GetRenderHeight() == 0 ||
        !State.PSO ||
        !State.SRB)
        return false;

    if (!IsDenoiserPreparePass(Pass) &&
        !IsDenoiserFinalMergePass(Pass) &&
        Pass != RTXPTPostProcessPassId::DenoisingLayersDebugViz &&
        Pass != RTXPTPostProcessPassId::DLSSRRDenoiserPrepareInputs &&
        Pass != RTXPTPostProcessPassId::NoDenoiserFinalMerge)
        return false;

    if ((pContext->GetDesc().QueueType & COMMAND_QUEUE_TYPE_PRIMARY_MASK) == COMMAND_QUEUE_TYPE_GRAPHICS)
        pContext->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    if (!UpdateMiniConstantsBuffer(pContext, m_MiniConstants, Pass, Attribs))
        return false;

    if (!BindDispatchResources(State.SRB, Pass, Attribs))
        return false;

    pContext->SetPipelineState(State.PSO);
    pContext->CommitShaderResources(State.SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DispatchComputeAttribs DispatchAttribs;
    DispatchAttribs.ThreadGroupCountX = (pRenderTargets->GetRenderWidth() + kThreadGroupSize - 1u) / kThreadGroupSize;
    DispatchAttribs.ThreadGroupCountY = (pRenderTargets->GetRenderHeight() + kThreadGroupSize - 1u) / kThreadGroupSize;
    DispatchAttribs.ThreadGroupCountZ = 1;
    pContext->DispatchCompute(DispatchAttribs);
    return true;
}

bool RTXPTPostProcessPass::RunDenoisingLayersDebugViz(IDeviceContext* pContext, const RTXPTDenoiserPostProcessAttribs& Attribs)
{
    m_Stats.LastDenoisingLayersDebugExecuted = false;

    const bool Executed = DispatchPass(pContext, RTXPTPostProcessPassId::DenoisingLayersDebugViz, Attribs);
    if (Executed)
    {
        m_Stats.LastDenoisingLayersDebugExecuted = true;
        ++m_Stats.DenoisingLayersDebugDispatchCount;
    }
    return Executed;
}

bool RTXPTPostProcessPass::RunDenoiserPrepare(IDeviceContext* pContext, const RTXPTDenoiserPostProcessAttribs& Attribs)
{
    m_Stats.LastDenoiserPrepareExecuted = false;

    const RTXPTPostProcessPassId Pass =
        Attribs.Method == RTXPTNrdMethod::RELAX ?
        RTXPTPostProcessPassId::RelaxDenoiserPrepareInputs :
        RTXPTPostProcessPassId::ReblurDenoiserPrepareInputs;
    const bool Executed = DispatchPass(pContext, Pass, Attribs);
    if (Executed)
    {
        m_Stats.LastDenoiserPrepareExecuted = true;
        ++m_Stats.DenoiserPrepareDispatchCount;
    }
    return Executed;
}

bool RTXPTPostProcessPass::RunDenoiserFinalMerge(IDeviceContext* pContext, const RTXPTDenoiserPostProcessAttribs& Attribs)
{
    m_Stats.LastDenoiserFinalMergeExecuted = false;

    const RTXPTPostProcessPassId Pass =
        Attribs.Method == RTXPTNrdMethod::RELAX ?
        RTXPTPostProcessPassId::RelaxDenoiserFinalMerge :
        RTXPTPostProcessPassId::ReblurDenoiserFinalMerge;
    const bool Executed = DispatchPass(pContext, Pass, Attribs);
    if (Executed)
    {
        m_Stats.LastDenoiserFinalMergeExecuted = true;
        ++m_Stats.DenoiserFinalMergeDispatchCount;
    }
    return Executed;
}

bool RTXPTPostProcessPass::RunNoDenoiserFinalMerge(IDeviceContext* pContext, const RTXPTDenoiserPostProcessAttribs& Attribs)
{
    m_Stats.LastNoDenoiserMergeExecuted = false;

    const bool Executed = DispatchPass(pContext, RTXPTPostProcessPassId::NoDenoiserFinalMerge, Attribs);
    if (Executed)
    {
        m_Stats.LastNoDenoiserMergeExecuted = true;
        ++m_Stats.NoDenoiserMergeDispatchCount;
    }
    return Executed;
}

bool RTXPTPostProcessPass::RunDLSSRRDenoiserPrepareInputs(IDeviceContext* pContext, const RTXPTDenoiserPostProcessAttribs& Attribs)
{
    if (!IsReady() || pContext == nullptr || Attribs.pRenderTargets == nullptr ||
        Attribs.pRenderTargets->GetDepthSRV() == nullptr ||
        Attribs.pRenderTargets->GetScreenMotionVectorsSRV() == nullptr ||
        Attribs.pRenderTargets->GetStableRadianceUAV() == nullptr ||
        Attribs.pRenderTargets->GetDenoisingLayersHeaderUAV() == nullptr ||
        Attribs.pRenderTargets->GetDenoisingLayersBufferUAV() == nullptr)
        return false;

    // Reject missing/stale/aliased outputs before touching the SRB.
    // The subsequent IRayReconstruction dispatch belongs to the frame scheduler.
    const std::array<ITextureView*, 7>  Outputs = {{Attribs.pMergeOutputUAV, Attribs.pRRDiffuseAlbedoUAV,
                                                   Attribs.pRRSpecAlbedoUAV, Attribs.pRRNormalsAndRoughnessUAV, Attribs.pRRDepthUAV, Attribs.pRRMotionVectorsUAV,
                                                   Attribs.pRenderTargets->GetRRSpecMotionVectorsUAV()}};
    const std::array<TEXTURE_FORMAT, 7> Formats = {{TEX_FORMAT_RGBA16_FLOAT, TEX_FORMAT_RGBA16_FLOAT,
                                                    TEX_FORMAT_RGBA16_FLOAT, TEX_FORMAT_RGBA16_FLOAT, TEX_FORMAT_R32_FLOAT, TEX_FORMAT_RG16_FLOAT, TEX_FORMAT_RG16_FLOAT}};
    for (size_t Index = 0; Index < Outputs.size(); ++Index)
    {
        ITextureView* pView = Outputs[Index];
        if (pView == nullptr || pView->GetDesc().ViewType != TEXTURE_VIEW_UNORDERED_ACCESS)
            return false;
        const TextureDesc& Desc = pView->GetTexture()->GetDesc();
        if (Desc.Type != RESOURCE_DIM_TEX_2D || Desc.Format != Formats[Index] ||
            pView->GetDesc().Format != Formats[Index] ||
            Desc.Width != Attribs.pRenderTargets->GetRenderWidth() ||
            Desc.Height != Attribs.pRenderTargets->GetRenderHeight() ||
            Desc.MipLevels != 1 || Desc.SampleCount != 1 ||
            pView->GetTexture() == Attribs.pRenderTargets->GetStableRadianceUAV()->GetTexture() ||
            pView->GetTexture() == Attribs.pRenderTargets->GetDepthSRV()->GetTexture() ||
            pView->GetTexture() == Attribs.pRenderTargets->GetScreenMotionVectorsSRV()->GetTexture())
            return false;
        for (size_t Previous = 0; Previous < Index; ++Previous)
            if (pView->GetTexture() == Outputs[Previous]->GetTexture())
                return false;
    }
    // BUILD/FILL and prepare access stable planes through UAVs. An explicit UAV
    // dependency is needed even when their tracked state does not change.
    if ((pContext->GetDesc().QueueType & COMMAND_QUEUE_TYPE_PRIMARY_MASK) == COMMAND_QUEUE_TYPE_GRAPHICS)
        pContext->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const RTXPTRenderTargets& Targets            = *Attribs.pRenderTargets;
    StateTransitionDesc       ProducerBarriers[] = {
        {Targets.GetStableRadianceUAV()->GetTexture(), RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE},
        {Targets.GetDenoisingLayersHeaderUAV()->GetTexture(), RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE},
        {Targets.GetDenoisingLayersBuffer(), RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE}};
    pContext->TransitionResourceStates(3, ProducerBarriers);
    if (!DispatchPass(pContext, RTXPTPostProcessPassId::DLSSRRDenoiserPrepareInputs, Attribs))
        return false;
    std::array<StateTransitionDesc, 7> ConsumerBarriers;
    for (size_t Index = 0; Index < Outputs.size(); ++Index)
        ConsumerBarriers[Index] = StateTransitionDesc{Outputs[Index]->GetTexture(), RESOURCE_STATE_UNORDERED_ACCESS,
                                                      RESOURCE_STATE_SHADER_RESOURCE, STATE_TRANSITION_FLAG_UPDATE_STATE};
    pContext->TransitionResourceStates(static_cast<Uint32>(ConsumerBarriers.size()), ConsumerBarriers.data());
    // Do not keep resized/mode-specific textures alive through cached bindings.
    auto* SRB = m_Passes[static_cast<size_t>(RTXPTPostProcessPassId::DLSSRRDenoiserPrepareInputs)].SRB.RawPtr();
    for (const char* Name : {"t_Depth", "t_MotionVectors", "u_OutputColor", "u_RRDiffuseAlbedo", "u_RRSpecAlbedo",
                             "u_RRNormalsAndRoughness", "u_RRDepth", "u_RRMotionVectors", "u_RRSpecMotionVectors", "t_SpecularHitT", "u_StableRadiance",
                             "u_DenoisingLayersHeader", "u_DenoisingLayersBuffer"})
        if (auto* Variable = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name))
            Variable->Set(nullptr);
    return true;
}

bool RTXPTPostProcessPass::RunDLSSRRDenoiserPrepareInputs(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets)
{
    if (!RenderTargets.HasRayReconstructionRenderTargets())
        return false;
    RTXPTDenoiserPostProcessAttribs Attribs;
    Attribs.pRenderTargets            = &RenderTargets;
    Attribs.pMergeOutputUAV           = RenderTargets.GetOutputColorUAV();
    Attribs.pRRDiffuseAlbedoUAV       = RenderTargets.GetRRDiffuseAlbedoUAV();
    Attribs.pRRSpecAlbedoUAV          = RenderTargets.GetRRSpecAlbedoUAV();
    Attribs.pRRNormalsAndRoughnessUAV = RenderTargets.GetRRNormalsAndRoughnessUAV();
    Attribs.pRRDepthUAV               = RenderTargets.GetRRDepthUAV();
    Attribs.pRRMotionVectorsUAV       = RenderTargets.GetRRMotionVectorsUAV();
    return RunDLSSRRDenoiserPrepareInputs(pContext, Attribs);
}

} // namespace Diligent
