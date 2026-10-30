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

#include "RTXPTRayTracingPass.hpp"
#include "RTXPTShaderSource.hpp"
#include "RTXPTPipelineStateCache.hpp"
#include "DebugUtilities.hpp"
#include "RTXPTFrameConstants.hpp"

#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"
#include "ShaderMacroHelper.hpp"

#include "AsyncPrefix.hpp"

#include <algorithm>
#include <cstring>

namespace Diligent
{

namespace
{

const char* GetVariantName(RTXPTPathTraceVariant Variant)
{
    switch (Variant)
    {
        case RTXPTPathTraceVariant::Reference: return "Reference";
        case RTXPTPathTraceVariant::BuildDenoisingLayers: return "BuildDenoisingLayers";
        case RTXPTPathTraceVariant::FillDenoisingLayers: return "FillDenoisingLayers";
        case RTXPTPathTraceVariant::SharcTrain: return "SharcTrain";
        case RTXPTPathTraceVariant::SharcFill: return "SharcFill";
        default: return "Unknown";
    }
}

int GetVariantModeMacro(RTXPTPathTraceVariant Variant)
{
    switch (Variant)
    {
        case RTXPTPathTraceVariant::Reference: return 0;
        case RTXPTPathTraceVariant::BuildDenoisingLayers: return 1;
        case RTXPTPathTraceVariant::FillDenoisingLayers: return 2;
        case RTXPTPathTraceVariant::SharcFill: return 2;
        default: return 0;
    }
}

constexpr Uint32 kRTXPTPrimaryRayIndex    = 0;
constexpr Uint32 kRTXPTVisibilityRayIndex = 1;

size_t GetVariantIndex(RTXPTPathTraceVariant Variant)
{
    return static_cast<size_t>(Variant);
}

} // namespace

void RTXPTRayTracingPass::Reset()
{
    for (VariantState& State : m_Variants)
    {
        State.PSO.Release();
        State.SRB.Release();
        State.SBT.Release();
        State.Stats = {};
    }
    m_MiniConstantsCB.Release();
    m_IndexBufferView.Release();
    m_IndexBuffer.Release();
    m_Bound       = {};
    m_HasBindings = false;
    m_BoundTextures.clear();
    m_IndexValueType      = VT_UNDEFINED;
    m_UseTextures         = false;
    m_UseAnyHitTextures   = false;
    m_UseHitObject        = false;
    m_UseInlineVisibility = false;
    m_Stats               = {};
}

void RTXPTRayTracingPass::UnbindSharcBuffers()
{
    for (auto Variant : {RTXPTPathTraceVariant::SharcTrain, RTXPTPathTraceVariant::SharcFill})
    {
        auto& State = m_Variants[GetVariantIndex(Variant)];
        if (!State.SRB) continue;
        for (auto Stage : {SHADER_TYPE_RAY_GEN, SHADER_TYPE_RAY_MISS, SHADER_TYPE_RAY_CLOSEST_HIT})
            for (const char* Name : {"cbSharc", "u_SharcHash", "u_SharcAccumulation", "u_SharcResolved", "u_SharcPathState"})
                if (auto* Var = State.SRB->GetVariableByName(Stage, Name)) Var->Set(nullptr);
    }
}

bool RTXPTRayTracingPass::Initialize(IRenderDevice* pDevice, IDeviceContext* pContext, const RTXPTRayTracingPassCreateInfo& CreateInfo)
{
    Reset();

    m_Stats.NEECandidateSamples = CreateInfo.NEECandidateSamples;
    m_Stats.NEEFullSamples      = CreateInfo.NEEFullSamples;

    if (pDevice == nullptr || pContext == nullptr || CreateInfo.pEngineFactory == nullptr)
    {
        DEV_ERROR("RTXPT path tracer requires device, context, and engine factory");
        return false;
    }

    if (!CreateInfo.RayTracingSupported)
    {
        DEV_ERROR("RTXPT reference path tracer requires ray tracing support");
        return false;
    }

    if (!CreateInfo.StandaloneRTShadersSupported)
    {
        DEV_ERROR("RTXPT reference path tracer requires standalone ray tracing shader support");
        return false;
    }

    if (CreateInfo.pFrameConstants == nullptr)
    {
        DEV_ERROR("RTXPT reference path tracer requires frame constants");
        return false;
    }

    if (CreateInfo.IndexValueType != VT_UINT16 && CreateInfo.IndexValueType != VT_UINT32)
    {
        LOG_ERROR_MESSAGE("Reference path tracer requires VT_UINT16 or VT_UINT32 indices");
        return false;
    }

    const bool UseTextures         = CreateInfo.EnableMaterialTextures && CreateInfo.MaterialTextureCount > 0;
    const bool UseAnyHit           = CreateInfo.EnableAnyHit;
    const bool UseInlineVisibility = CreateInfo.EnableInlineVisibility &&
        (pDevice->GetAdapterInfo().RayTracing.CapFlags & RAY_TRACING_CAP_FLAG_INLINE_RAY_TRACING) != 0;
    const auto& DeviceInfo        = pDevice->GetDeviceInfo();
    const bool  SupportsHitObject = DeviceInfo.Type == RENDER_DEVICE_TYPE_D3D12 &&
        DeviceInfo.MaxShaderVersion.HLSL >= ShaderVersion{6, 9};
    const bool UseHitObject     = CreateInfo.EnableHitObject && SupportsHitObject;
    const bool UseThreadReorder = UseHitObject && CreateInfo.EnableThreadReorder;
    LOG_INFO_MESSAGE("RTXPT scatter rays: ", UseHitObject ? (UseThreadReorder ? "HitObject + reorder" : "HitObject") : "TraceRay");

    m_IndexValueType      = CreateInfo.IndexValueType;
    m_UseTextures         = UseTextures;
    m_UseAnyHitTextures   = UseTextures && UseAnyHit && (!UseHitObject || !UseInlineVisibility);
    m_UseHitObject        = UseHitObject;
    m_UseInlineVisibility = UseInlineVisibility;

    BufferDesc MiniCBDesc;
    MiniCBDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    MiniCBDesc.Name                 = "RTXPT SampleMiniConstants";
    MiniCBDesc.Size                 = sizeof(SampleMiniConstants);
    MiniCBDesc.BindFlags            = BIND_UNIFORM_BUFFER;
    MiniCBDesc.Usage                = USAGE_DYNAMIC;
    MiniCBDesc.CPUAccessFlags       = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(MiniCBDesc, nullptr, &m_MiniConstantsCB);
    if (!m_MiniConstantsCB)
    {
        DEV_ERROR("Failed to create RTXPT SampleMiniConstants buffer");
        return false;
    }

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    CreateRTXPTShaderSourceFactory(CreateInfo.pEngineFactory, "shaders;shaders/PathTracer;shaders/SHARC;" RTXPT_SHARC_SHADER_DIR, &pShaderSourceFactory);
    if (!pShaderSourceFactory)
    {
        DEV_ERROR("Failed to create RTXPT shader source factory");
        return false;
    }

    // Material data is read by raygen for nested dielectric media and by hit shaders for surface shading.
    const SHADER_TYPE HitStages = (UseAnyHit && (!UseHitObject || !UseInlineVisibility) ?
                                       (SHADER_TYPE_RAY_CLOSEST_HIT | SHADER_TYPE_RAY_ANY_HIT) :
                                       SHADER_TYPE_RAY_CLOSEST_HIT) |
        (UseHitObject ? SHADER_TYPE_RAY_GEN : SHADER_TYPE_UNKNOWN);
    const SHADER_TYPE EnvStages = SHADER_TYPE_RAY_GEN | HitStages |
        (!UseHitObject || !UseInlineVisibility ? SHADER_TYPE_RAY_MISS : SHADER_TYPE_UNKNOWN);
    const SHADER_TYPE ConstStages   = EnvStages | HitStages;
    const SHADER_TYPE DynamicStages = EnvStages;

    m_Stats.MaterialTextureCount = UseTextures ? CreateInfo.MaterialTextureCount : 0;

    auto CreateVariant = [&](RTXPTPathTraceVariant Variant) {
        VariantState& State = m_Variants[GetVariantIndex(Variant)];

        ShaderCreateInfo ShaderCI;
        ShaderCI.Desc.UseCombinedTextureSamplers = false;
        ShaderCI.SourceLanguage                  = SHADER_SOURCE_LANGUAGE_HLSL;
        ShaderCI.ShaderCompiler                  = SHADER_COMPILER_DXC;
        ShaderCI.CompileFlags                    = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
        const bool SharcVariant                  = Variant == RTXPTPathTraceVariant::SharcTrain || Variant == RTXPTPathTraceVariant::SharcFill;
        if (CreateInfo.UseFp16Types || SharcVariant)
            ShaderCI.CompileFlags |= SHADER_COMPILE_FLAG_ENABLE_16BIT_TYPES;
        ShaderCI.ShaderOptimizationLevel = SHADER_OPTIMIZATION_LEVEL_3;
        ShaderCI.HLSLVersion             = SupportsHitObject ? ShaderVersion{6, 9} : ShaderVersion{6, 5};
        if (SharcVariant && !SupportsHitObject) ShaderCI.HLSLVersion = {6, 6};
        ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

        ShaderMacroHelper Macros;
        Macros.Add("RTXPT_SHARC_TRAIN", Variant == RTXPTPathTraceVariant::SharcTrain ? 1 : 0);
        Macros.Add("RTXPT_SHARC_CONSUME", Variant == RTXPTPathTraceVariant::SharcFill ? 1 : 0);
        Macros.Add("USE_DX_HIT_OBJECT_EXTENSION", UseHitObject ? 1 : 0);
        Macros.Add("USE_DX_MAYBE_REORDER_THREADS", UseThreadReorder ? 1 : 0);
        Macros.Add("RTXPT_LP_TYPES_USE_16BIT_PRECISION", CreateInfo.UseFp16Types ? 1 : 0);
        Macros.Add("PATH_TRACER_MODE", GetVariantModeMacro(Variant));
        Macros.Add("RTXPT_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", CreateInfo.NEECandidateSamples);
        Macros.Add("RTXPT_NEE_FULL_SAMPLE_COUNT", CreateInfo.NEEFullSamples);
        Macros.Add("RTXPT_USE_APPROXIMATE_MIS", CreateInfo.NEEMISType == 2 || (CreateInfo.NEEMISType == 1 && Variant != RTXPTPathTraceVariant::Reference) ? 1 : 0);
        Macros.Add("RTXPT_USE_INLINE_VISIBILITY", UseInlineVisibility ? 1 : 0);
        Macros.Add("RTXPT_INDEX_BUFFER_UINT16", CreateInfo.IndexValueType == VT_UINT16 ? 1 : 0);
        if (UseTextures)
        {
            Macros.Add("ENABLE_MATERIAL_TEXTURES", 1);
            Macros.Add("MATERIAL_TEXTURE_COUNT", static_cast<int>(CreateInfo.MaterialTextureCount));
        }
        Macros.Add("RTXPT_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", CreateInfo.EnableLDSamplerForBSDF ? 1 : 0);
        ShaderCI.Macros = Macros;

        RefCntAutoPtr<IShader> pRayGen;
        ShaderCI.Desc.ShaderType = SHADER_TYPE_RAY_GEN;
        ShaderCI.Desc.Name       = "RTXPT path trace raygen";
        ShaderCI.FilePath        = "PathTracer/PathTracerSample.rgen";
        ShaderCI.EntryPoint      = "main";
        CreateInfo.pStateCache->CreateShader(ShaderCI, &pRayGen);

        RefCntAutoPtr<IShader> pMiss;
        if (!UseHitObject)
        {
            ShaderCI.Desc.ShaderType = SHADER_TYPE_RAY_MISS;
            ShaderCI.Desc.Name       = "RTXPT path trace miss";
            ShaderCI.FilePath        = "PathTracer/PathTracerMiss.rmiss";
            ShaderCI.EntryPoint      = "main";
            CreateInfo.pStateCache->CreateShader(ShaderCI, &pMiss);
        }

        RefCntAutoPtr<IShader> pVisibilityMiss;
        if (!UseInlineVisibility)
        {
            ShaderCI.Desc.ShaderType = SHADER_TYPE_RAY_MISS;
            ShaderCI.Desc.Name       = "RTXPT path trace visibility miss";
            ShaderCI.FilePath        = "PathTracer/PathTracerVisibilityMiss.rmiss";
            ShaderCI.EntryPoint      = "main";
            CreateInfo.pStateCache->CreateShader(ShaderCI, &pVisibilityMiss);
        }

        RefCntAutoPtr<IShader> pClosestHit;
        ShaderCI.Desc.ShaderType = SHADER_TYPE_RAY_CLOSEST_HIT;
        ShaderCI.Desc.Name       = "RTXPT path trace closest hit";
        ShaderCI.FilePath        = "PathTracer/PathTracerClosestHit.rchit";
        ShaderCI.EntryPoint      = "main";
        CreateInfo.pStateCache->CreateShader(ShaderCI, &pClosestHit);

        RefCntAutoPtr<IShader> pAnyHit;
        RefCntAutoPtr<IShader> pVisibilityAnyHit;
        if (UseAnyHit)
        {
            ShaderCI.Desc.ShaderType = SHADER_TYPE_RAY_ANY_HIT;
            ShaderCI.Desc.Name       = "RTXPT path trace any hit";
            ShaderCI.FilePath        = "PathTracer/PathTracerAnyHit.rahit";
            ShaderCI.EntryPoint      = "main";
            if (!UseHitObject)
                CreateInfo.pStateCache->CreateShader(ShaderCI, &pAnyHit);
            if (!UseInlineVisibility)
            {
                ShaderCI.Desc.Name  = "RTXPT visibility any hit";
                ShaderCI.EntryPoint = "VisibilityMain";
                Macros.Add("RTXPT_VISIBILITY_ANY_HIT", 1);
                ShaderCI.Macros = Macros;
                CreateInfo.pStateCache->CreateShader(ShaderCI, &pVisibilityAnyHit);
            }
        }

        const bool VisibilityShadersReady = UseInlineVisibility || (pVisibilityMiss && (!UseAnyHit || pVisibilityAnyHit));
        VERIFY(pRayGen && (UseHitObject || pMiss) && VisibilityShadersReady && pClosestHit && (UseHitObject || !UseAnyHit || pAnyHit),
               "Failed to create RTXPT path tracing shaders");
        if (!pRayGen || (!UseHitObject && !pMiss) || !VisibilityShadersReady || !pClosestHit || (!UseHitObject && UseAnyHit && !pAnyHit))
        {
            DEV_ERROR("Failed to create RTXPT path tracing shaders for variant: ", GetVariantName(Variant));
            return false;
        }

        RayTracingPipelineStateCreateInfoX PSOCreateInfo;
        PSOCreateInfo.PSODesc.Name                 = "RTXPT path trace RT PSO";
        PSOCreateInfo.PSODesc.PipelineType         = PIPELINE_TYPE_RAY_TRACING;
        PSOCreateInfo.PSODesc.ImmediateContextMask = kRTXPTImmediateContextMask;
        PSOCreateInfo.AddGeneralShader("Main", pRayGen);
        if (!UseHitObject)
            PSOCreateInfo.AddGeneralShader("PrimaryMiss", pMiss);
        if (!UseInlineVisibility)
        {
            PSOCreateInfo.AddGeneralShader("VisibilityMiss", pVisibilityMiss);
            PSOCreateInfo.AddTriangleHitShader("VisibilityHit", pClosestHit, pVisibilityAnyHit);
        }
        if (UseAnyHit && !UseHitObject)
            PSOCreateInfo.AddTriangleHitShader("PrimaryHit", pClosestHit, pAnyHit);
        else
            PSOCreateInfo.AddTriangleHitShader("PrimaryHit", pClosestHit);
        const bool TracesVisibilityFromHit =
            Variant == RTXPTPathTraceVariant::Reference ||
            Variant == RTXPTPathTraceVariant::FillDenoisingLayers || SharcVariant;
        PSOCreateInfo.RayTracingPipeline.MaxRecursionDepth =
            TracesVisibilityFromHit && !UseInlineVisibility ? 2 : 1;
        PSOCreateInfo.RayTracingPipeline.ShaderRecordSize = 0;
        PSOCreateInfo.MaxAttributeSize                    = static_cast<Uint32>(sizeof(float) * 2);
        PSOCreateInfo.MaxPayloadSize                      = static_cast<Uint32>(sizeof(float) * 20);

        PipelineResourceLayoutDescX ResourceLayout;
        ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
        if (Variant == RTXPTPathTraceVariant::FillDenoisingLayers || Variant == RTXPTPathTraceVariant::SharcFill)
            ResourceLayout.AddVariable(ConstStages, "u_SubSamplingPixelList", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
        if (SharcVariant)
            for (const char* Name : {"cbSharc", "u_SharcHash", "u_SharcAccumulation", "u_SharcResolved", "u_SharcPathState"})
                ResourceLayout.AddVariable(ConstStages, Name, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
        ResourceLayout
            .AddVariable(SHADER_TYPE_RAY_GEN, "u_Output", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
            .AddVariable(SHADER_TYPE_RAY_GEN, "u_ScreenMotionVectors", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
            .AddVariable(DynamicStages, "u_OutputColor", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
            .AddVariable(DynamicStages, "u_Depth", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
            .AddVariable(DynamicStages, "u_MotionVectors", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
            .AddVariable(DynamicStages, "u_Throughput", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
            .AddVariable(DynamicStages, "u_SpecularHitT", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
            .AddVariable(DynamicStages, "u_StableRadiance", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
            .AddVariable(DynamicStages, "u_DenoisingLayersHeader", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
            .AddVariable(DynamicStages, "u_DenoisingLayersBuffer", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);

        // Scene, lighting and environment resources are mutable: Bind creates new resource bindings when they change.
        // None of their buffers is USAGE_DYNAMIC, so they need no dynamic offsets, whose Vulkan descriptors are limited.
        ResourceLayout
            .AddVariable(ConstStages, "g_Const", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
            .AddVariable(ConstStages, "g_MiniConst", SHADER_RESOURCE_VARIABLE_TYPE_STATIC);
        for (const char* Name : {"t_PTMaterialData", "t_SubInstanceData", "t_VertexBuffer", "t_SkinnedVertexBuffer", "t_Lights", "t_LightsEx",
                                 "t_LightingControl", "t_LightProxyCounters", "t_LightSamplingProxies", "t_LocalSamplingBuffer"})
            ResourceLayout.AddVariable(ConstStages, Name, SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE, SHADER_VARIABLE_FLAG_NO_DYNAMIC_BUFFERS);

        if (UseTextures)
        {
            ResourceLayout.AddVariable(HitStages, "t_BindlessTextures", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE);

            const SamplerDesc MaterialSamplerDesc{
                FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR, FILTER_TYPE_LINEAR,
                TEXTURE_ADDRESS_WRAP, TEXTURE_ADDRESS_WRAP, TEXTURE_ADDRESS_WRAP};
            ResourceLayout.AddImmutableSampler(HitStages, "s_MaterialSampler", MaterialSamplerDesc);
        }
        PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

        PSOCreateInfo.pPSOCache = SelectRTXPTPipelineStateCache(pDevice, PSOCreateInfo, CreateInfo.pPSOCache);
        CreateInfo.pStateCache->CreateRayTracingPipelineState(PSOCreateInfo, &State.PSO);
        VERIFY(State.PSO, "Failed to create RTXPT path trace RT PSO");
        if (!State.PSO)
        {
            DEV_ERROR("Failed to create RTXPT path trace RT PSO for variant: ", GetVariantName(Variant));
            return false;
        }

        auto SetStatic = [&](const char* Name, IDeviceObject* pObject, bool Required) {
            bool FoundAny = false;
            for (SHADER_TYPE Stage : {SHADER_TYPE_RAY_GEN, SHADER_TYPE_RAY_MISS, SHADER_TYPE_RAY_CLOSEST_HIT, SHADER_TYPE_RAY_ANY_HIT})
            {
                // Stages outside ConstStages are inactive in some variants and would warn on lookup.
                if ((ConstStages & Stage) == 0)
                    continue;
                if (IShaderResourceVariable* pVar = State.PSO->GetStaticVariableByName(Stage, Name))
                {
                    pVar->Set(pObject, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
                    FoundAny = true;
                }
            }
            if (!FoundAny && Required)
            {
                UNEXPECTED("RTXPT static shader variable is missing: ", Name, " for variant: ", GetVariantName(Variant));
                return false;
            }
            return true;
        };

        // BUILD always reads the current sub-sample from g_MiniConst. FILL may reflect it through
        // hit shaders, but some Fill PSO resource tables omit it, so bind opportunistically there.
        const bool MiniConstantsRequired = Variant == RTXPTPathTraceVariant::BuildDenoisingLayers;
        if (!SetStatic("g_Const", CreateInfo.pFrameConstants, true) ||
            !SetStatic("g_MiniConst", m_MiniConstantsCB, MiniConstantsRequired))
            return false;

        ShaderBindingTableDesc SBTDesc;
        SBTDesc.Name = "RTXPT path trace SBT";
        SBTDesc.pPSO = State.PSO;
        pDevice->CreateSBT(SBTDesc, &State.SBT);
        VERIFY(State.SBT, "Failed to create RTXPT path trace SBT");
        if (!State.SBT)
        {
            DEV_ERROR("Failed to create RTXPT path trace SBT for variant: ", GetVariantName(Variant));
            return false;
        }

        // Hit groups follow the instances of each TLAS build and are bound by Bind.
        State.SBT->BindRayGenShader("Main");
        if (!UseHitObject || !UseInlineVisibility)
            State.SBT->BindMissShader(UseHitObject ? nullptr : "PrimaryMiss", kRTXPTPrimaryRayIndex);
        if (!UseInlineVisibility)
            State.SBT->BindMissShader("VisibilityMiss", kRTXPTVisibilityRayIndex);

        State.Stats.Ready = true;
        return true;
    };

    const RTXPTPathTraceVariant Variants[] =
        {
            RTXPTPathTraceVariant::Reference,
            RTXPTPathTraceVariant::BuildDenoisingLayers,
            RTXPTPathTraceVariant::FillDenoisingLayers,
        };
    for (RTXPTPathTraceVariant Variant : Variants)
    {
        if (!CreateVariant(Variant))
        {
            Reset();
            return false;
        }
    }

    if (CreateInfo.EnableSharc && (!CreateVariant(RTXPTPathTraceVariant::SharcTrain) || !CreateVariant(RTXPTPathTraceVariant::SharcFill)))
        return false;

    m_Stats.AnyHitEnabled           = UseAnyHit;
    m_Stats.InlineVisibilityEnabled = UseInlineVisibility;
    LOG_INFO_MESSAGE("RTXPT visibility rays: ", UseInlineVisibility ? "inline RayQuery" : "TraceRay");
    m_Stats.Ready = true;
    return true;
}

bool RTXPTRayTracingPass::Bind(IDeviceContext* pContext, const RTXPTRayTracingBindings& Bindings)
{
    if (!m_Stats.Ready || pContext == nullptr)
    {
        DEV_ERROR("RTXPT path tracer must be initialized before binding resources");
        return false;
    }
    if (Bindings.pTLAS == nullptr || Bindings.pMaterialBuffer == nullptr || Bindings.pSubInstanceBuffer == nullptr ||
        Bindings.pVertexBuffer == nullptr || Bindings.pSkinnedVertexBuffer == nullptr || Bindings.pIndexBuffer == nullptr)
    {
        DEV_ERROR("RTXPT path tracer requires a TLAS and the material, sub-instance, vertex, skinned vertex and index buffers");
        return false;
    }
    if (m_UseTextures && (Bindings.pMaterialTextures == nullptr || Bindings.MaterialTextureCount != m_Stats.MaterialTextureCount))
    {
        DEV_ERROR("RTXPT path tracer was compiled for ", m_Stats.MaterialTextureCount, " material textures, but ",
                  Bindings.MaterialTextureCount, " are bound");
        return false;
    }

    const Uint32 TextureCount  = m_UseTextures ? Bindings.MaterialTextureCount : 0;
    const bool   TexturesEqual = m_BoundTextures.size() == TextureCount &&
        std::equal(m_BoundTextures.begin(), m_BoundTextures.end(), Bindings.pMaterialTextures);
    RTXPTRayTracingBindings Key = Bindings;
    Key.TLASBuildIndex          = m_Bound.TLASBuildIndex;
    Key.pMaterialTextures       = m_Bound.pMaterialTextures;
    const bool ObjectsEqual     = m_HasBindings && TexturesEqual && std::memcmp(&Key, &m_Bound, sizeof(Key)) == 0;

    if (!ObjectsEqual && !CreateBindings(Bindings))
    {
        m_HasBindings = false;
        return false;
    }

    if (!ObjectsEqual || Bindings.TLASBuildIndex != m_Bound.TLASBuildIndex)
    {
        // The hit groups follow the instances and geometries of this TLAS build.
        for (VariantState& State : m_Variants)
        {
            if (!State.Stats.Ready)
                continue;
            State.SBT->ResetHitGroups();
            State.SBT->BindHitGroupForTLAS(Bindings.pTLAS, kRTXPTPrimaryRayIndex, "PrimaryHit");
            // The shared TLAS keeps a two-ray stride even when visibility uses RayQuery.
            // Explicitly bind its unused slot so SBT validation sees a complete table.
            State.SBT->BindHitGroupForTLAS(Bindings.pTLAS, kRTXPTVisibilityRayIndex, m_UseInlineVisibility ? nullptr : "VisibilityHit");
            pContext->UpdateSBT(State.SBT);
        }
        ++m_Stats.SBTRefreshCount;
    }

    m_Bound                   = Bindings;
    m_Bound.pMaterialTextures = nullptr;
    m_BoundTextures.assign(Bindings.pMaterialTextures, Bindings.pMaterialTextures + TextureCount);
    m_HasBindings = true;
    return true;
}

bool RTXPTRayTracingPass::CreateBindings(const RTXPTRayTracingBindings& Bindings)
{
    if (m_IndexBuffer != Bindings.pIndexBuffer)
    {
        // Index buffers are BUFFER_MODE_FORMATTED without a pre-created typed view; HLSL declares Buffer<uint>.
        m_IndexBufferView.Release();
        m_IndexBuffer = Bindings.pIndexBuffer;
        BufferViewDesc IndexViewDesc;
        IndexViewDesc.Name                 = "RTXPT reference index buffer SRV";
        IndexViewDesc.ViewType             = BUFFER_VIEW_SHADER_RESOURCE;
        IndexViewDesc.Format.ValueType     = m_IndexValueType;
        IndexViewDesc.Format.NumComponents = 1;
        IndexViewDesc.Format.IsNormalized  = false;
        Bindings.pIndexBuffer->CreateView(IndexViewDesc, &m_IndexBufferView);
        if (!m_IndexBufferView)
        {
            m_IndexBuffer.Release();
            DEV_ERROR("Failed to create RTXPT index buffer view");
            return false;
        }
    }

    auto BufferSRV = [](IBuffer* pBuffer) -> IDeviceObject* {
        return pBuffer != nullptr ? pBuffer->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE) : nullptr;
    };
    struct Resource
    {
        const char*    Name;
        IDeviceObject* pObject;
        bool           Required;
    };
    const Resource Resources[] = {
        {"t_SceneBVH", Bindings.pTLAS, true},
        {"t_PTMaterialData", BufferSRV(Bindings.pMaterialBuffer), true},
        {"t_SubInstanceData", BufferSRV(Bindings.pSubInstanceBuffer), true},
        {"t_VertexBuffer", BufferSRV(Bindings.pVertexBuffer), true},
        {"t_SkinnedVertexBuffer", BufferSRV(Bindings.pSkinnedVertexBuffer), true},
        {"t_IndexBuffer", m_IndexBufferView, true},
        {"t_Lights", BufferSRV(Bindings.pLightBuffer), false},
        {"t_LightsEx", BufferSRV(Bindings.pLightExBuffer), false},
        {"t_LightingControl", BufferSRV(Bindings.pLightingControlBuffer), false},
        {"t_LightProxyCounters", BufferSRV(Bindings.pLightProxyCounters), false},
        {"t_LightSamplingProxies", BufferSRV(Bindings.pLightSamplingProxies), false},
        {"t_LocalSamplingBuffer", BufferSRV(Bindings.pLocalSamplingBuffer), false},
        {"u_FeedbackTotalWeight", Bindings.pFeedbackTotalWeightUAV, false},
        {"u_FeedbackCandidates", Bindings.pFeedbackCandidatesUAV, false},
        {"t_EnvironmentLightLookup", Bindings.pEnvironmentLightLookup, false},
        {"t_EnvironmentMap", Bindings.pEnvironmentMapSRV, false},
        {"t_EnvironmentImportanceMap", Bindings.pEnvironmentImportanceMapSRV, false},
        {"t_EnvironmentRadianceMap", Bindings.pEnvironmentRadianceMapSRV, false},
        {"s_EnvironmentMapSampler", Bindings.pEnvironmentSampler, false},
        {"s_EnvironmentImportanceSampler", Bindings.pEnvironmentImportanceSampler, false},
    };

    bool EnvironmentReflected = false;
    for (VariantState& State : m_Variants)
    {
        if (!State.Stats.Ready)
            continue;

        RefCntAutoPtr<IShaderResourceBinding> pSRB;
        State.PSO->CreateShaderResourceBinding(&pSRB, true);
        if (!pSRB)
        {
            DEV_ERROR("Failed to create RTXPT path trace RT SRB");
            return false;
        }

        for (const Resource& R : Resources)
        {
            bool FoundAny = false;
            for (SHADER_TYPE Stage : {SHADER_TYPE_RAY_GEN, SHADER_TYPE_RAY_MISS, SHADER_TYPE_RAY_CLOSEST_HIT, SHADER_TYPE_RAY_ANY_HIT})
            {
                IShaderResourceVariable* pVar = pSRB->GetVariableByName(Stage, R.Name);
                if (pVar == nullptr)
                    continue;
                FoundAny = true;
                if (R.pObject == nullptr)
                {
                    // A reflected resource must be bound, even when only some variants use it.
                    DEV_ERROR("RTXPT resource object is null: ", R.Name);
                    return false;
                }
                pVar->Set(R.pObject);
            }
            if (!FoundAny && R.Required)
            {
                UNEXPECTED("RTXPT shader variable is missing: ", R.Name);
                return false;
            }
            if (FoundAny && (std::strncmp(R.Name, "t_Environment", 13) == 0 || std::strncmp(R.Name, "s_Environment", 13) == 0))
                EnvironmentReflected = true;
        }

        if (m_UseTextures)
        {
            IShaderResourceVariable* pClosestHitTexVar = pSRB->GetVariableByName(SHADER_TYPE_RAY_CLOSEST_HIT, "t_BindlessTextures");
            IShaderResourceVariable* pAnyHitTexVar     = m_UseAnyHitTextures ?
                pSRB->GetVariableByName(SHADER_TYPE_RAY_ANY_HIT, "t_BindlessTextures") :
                nullptr;
            if (pClosestHitTexVar == nullptr || (m_UseAnyHitTextures && pAnyHitTexVar == nullptr))
            {
                UNEXPECTED("Failed to find RTXPT material texture array binding");
                return false;
            }
            pClosestHitTexVar->SetArray(Bindings.pMaterialTextures, 0, Bindings.MaterialTextureCount);
            if (m_UseHitObject)
            {
                if (IShaderResourceVariable* pRayGenTexVar = pSRB->GetVariableByName(SHADER_TYPE_RAY_GEN, "t_BindlessTextures"))
                    pRayGenTexVar->SetArray(Bindings.pMaterialTextures, 0, Bindings.MaterialTextureCount);
            }
            if (pAnyHitTexVar != nullptr)
                pAnyHitTexVar->SetArray(Bindings.pMaterialTextures, 0, Bindings.MaterialTextureCount);
        }
        State.SRB = std::move(pSRB);
    }

    m_Stats.MaterialBridgeBound      = true;
    m_Stats.SubInstanceBound         = true;
    m_Stats.VertexBufferBound        = true;
    m_Stats.SkinnedVertexBufferBound = true;
    m_Stats.IndexBufferBound         = true;
    m_Stats.LightBridgeBound         = true;
    m_Stats.LightsBakerBridgeBound   = true;
    m_Stats.EmissiveLightBridgeBound = true;
    m_Stats.EnvironmentBridgeBound   = EnvironmentReflected;
    m_Stats.MaterialTexturesBound    = m_UseTextures;
    ++m_Stats.BindingCount;
    return true;
}

bool RTXPTRayTracingPass::Dispatch(IDeviceContext*                pContext,
                                   RTXPTPathTraceVariant          Variant,
                                   const RTXPTRayTracingDispatch& DispatchInfo)
{
    m_Stats.LastTraceExecuted = false;

    const size_t VariantIdx = GetVariantIndex(Variant);
    if (VariantIdx >= m_Variants.size())
    {
        DEV_ERROR("RTXPT dispatch received invalid path trace variant");
        return false;
    }

    VariantState& State           = m_Variants[VariantIdx];
    State.Stats.LastTraceExecuted = false;

    if (!IsReady(Variant))
    {
        DEV_ERROR("RTXPT path trace variant is not ready: ", GetVariantName(Variant));
        return false;
    }
    if (!m_HasBindings)
    {
        DEV_ERROR("RTXPT path trace resources are not bound");
        return false;
    }
    if (pContext == nullptr)
        return false;
    const bool TrainVariant                = Variant == RTXPTPathTraceVariant::SharcTrain;
    const bool ReferenceVariant            = Variant == RTXPTPathTraceVariant::Reference || TrainVariant;
    const bool BuildDenoisingLayersVariant = Variant == RTXPTPathTraceVariant::BuildDenoisingLayers;
    const bool RealtimeVariant             = !ReferenceVariant;
    const bool FinalOutputRequired         = ReferenceVariant && !TrainVariant;
    const bool SurfaceExportsRequired      = (ReferenceVariant && !TrainVariant) || BuildDenoisingLayersVariant;
    const bool ThroughputRequired          = BuildDenoisingLayersVariant;
    const bool SpecularHitTRequired        = RealtimeVariant;
    const bool StableRadianceRequired      = BuildDenoisingLayersVariant;
    const bool DenoisingLayersRequired     = RealtimeVariant;

    if (DispatchInfo.Width == 0 || DispatchInfo.Height == 0)
        return false;
    if ((FinalOutputRequired && DispatchInfo.pOutputColorUAV == nullptr) ||
        (SurfaceExportsRequired &&
         (DispatchInfo.pDepthUAV == nullptr ||
          DispatchInfo.pMotionVectorsUAV == nullptr)) ||
        (ThroughputRequired && DispatchInfo.pThroughputUAV == nullptr) ||
        (SpecularHitTRequired && DispatchInfo.pSpecularHitTUAV == nullptr) ||
        (StableRadianceRequired && DispatchInfo.pStableRadianceUAV == nullptr) ||
        (DenoisingLayersRequired &&
         (DispatchInfo.pDenoisingLayersHeaderUAV == nullptr ||
          DispatchInfo.pDenoisingLayersBufferUAV == nullptr)))
        return false;

    auto SetDynamicForStages = [&](const char* Name, IDeviceObject* pObject, bool Required, bool* pFoundAny = nullptr) {
        bool FoundAny = false;
        bool Ok       = true;
        for (SHADER_TYPE Stage : {SHADER_TYPE_RAY_GEN, SHADER_TYPE_RAY_MISS, SHADER_TYPE_RAY_CLOSEST_HIT, SHADER_TYPE_RAY_ANY_HIT})
        {
            IShaderResourceVariable* pVar = State.SRB->GetVariableByName(Stage, Name);
            if (pVar == nullptr)
                continue;
            FoundAny = true;

            if (pObject == nullptr)
            {
                if (Required)
                {
                    DEV_ERROR("RTXPT dynamic resource object is null: ", Name, " for variant: ", GetVariantName(Variant));
                    Ok = false;
                }
                continue;
            }

            pVar->Set(pObject);
        }

        if (pFoundAny != nullptr)
            *pFoundAny = FoundAny;

        if (!FoundAny && Required)
        {
            LOG_ERROR_MESSAGE("RTXPT dynamic shader variable is missing: ", Name, " for variant: ", GetVariantName(Variant));
            return false;
        }

        return Ok;
    };

    // Compatibility bridge: bind either the source-compatible names or the legacy output names.
    auto SetDynamicForNamePair = [&](const char* SourceName, const char* LegacyName, IDeviceObject* pObject, bool Required) {
        bool       SourceFound = false;
        bool       LegacyFound = false;
        const bool SourceOk    = SetDynamicForStages(SourceName, pObject, false, &SourceFound);
        const bool LegacyOk    = SetDynamicForStages(LegacyName, pObject, false, &LegacyFound);
        if (!SourceOk || !LegacyOk)
            return false;
        if (!SourceFound && !LegacyFound && Required)
        {
            UNEXPECTED("RTXPT dynamic shader variable is missing: ",
                       SourceName, " or ", LegacyName,
                       " for variant: ", GetVariantName(Variant));
            return false;
        }
        return true;
    };

    if (TrainVariant || Variant == RTXPTPathTraceVariant::SharcFill)
    {
        if (!SetDynamicForStages("cbSharc", DispatchInfo.pSharcConstants, true) ||
            !SetDynamicForStages("u_SharcHash", DispatchInfo.pSharcHash, true) ||
            !SetDynamicForStages("u_SharcResolved", DispatchInfo.pSharcResolved, true) ||
            !SetDynamicForStages("u_SharcAccumulation", DispatchInfo.pSharcAccumulation, TrainVariant) ||
            !SetDynamicForStages("u_SharcPathState", DispatchInfo.pSharcPathState, TrainVariant)) return false;
    }
    if (Variant == RTXPTPathTraceVariant::FillDenoisingLayers || Variant == RTXPTPathTraceVariant::SharcFill)
        if (!SetDynamicForStages("u_SubSamplingPixelList", DispatchInfo.pSubSamplingPixelList, true)) return false;
    const bool OutputBound =
        SetDynamicForNamePair("u_OutputColor", "u_Output", DispatchInfo.pOutputColorUAV, FinalOutputRequired);
    const bool DepthBound =
        SetDynamicForStages("u_Depth", DispatchInfo.pDepthUAV, SurfaceExportsRequired);
    const bool MotionVectorsBound =
        SetDynamicForNamePair("u_MotionVectors", "u_ScreenMotionVectors", DispatchInfo.pMotionVectorsUAV, SurfaceExportsRequired);
    const bool ThroughputBound =
        SetDynamicForStages("u_Throughput", DispatchInfo.pThroughputUAV, ThroughputRequired);
    const bool SpecularHitTBound =
        SetDynamicForStages("u_SpecularHitT", DispatchInfo.pSpecularHitTUAV, SpecularHitTRequired);
    const bool StableRadianceBound =
        SetDynamicForStages("u_StableRadiance", DispatchInfo.pStableRadianceUAV, StableRadianceRequired);
    const bool DenoisingLayersHeaderBound =
        SetDynamicForStages("u_DenoisingLayersHeader", DispatchInfo.pDenoisingLayersHeaderUAV, DenoisingLayersRequired);
    const bool DenoisingLayersBufferBound =
        SetDynamicForStages("u_DenoisingLayersBuffer", DispatchInfo.pDenoisingLayersBufferUAV, DenoisingLayersRequired);

    if (!OutputBound ||
        !DepthBound ||
        !MotionVectorsBound ||
        !ThroughputBound ||
        !SpecularHitTBound ||
        !StableRadianceBound ||
        !DenoisingLayersHeaderBound ||
        !DenoisingLayersBufferBound)
    {
        return false;
    }

    SampleMiniConstants Mini = DispatchInfo.pMiniConstants != nullptr ? *DispatchInfo.pMiniConstants : SampleMiniConstants{};
    {
        MapHelper<SampleMiniConstants> Mapped{pContext, m_MiniConstantsCB, MAP_WRITE, MAP_FLAG_DISCARD};
        if (Mapped == nullptr)
        {
            DEV_ERROR("Failed to map RTXPT SampleMiniConstants buffer");
            return false;
        }
        *Mapped = Mini;
    }

    pContext->SetPipelineState(State.PSO);
    pContext->CommitShaderResources(State.SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    TraceRaysAttribs Attribs;
    Attribs.DimensionX = DispatchInfo.Width;
    Attribs.DimensionY = DispatchInfo.Height;
    Attribs.pSBT       = State.SBT;
    pContext->TraceRays(Attribs);

    State.Stats.LastTraceExecuted = true;
    ++State.Stats.TraceCount;
    m_Stats.LastTraceExecuted = true;
    ++m_Stats.TraceCount;
    return true;
}

bool RTXPTRayTracingPass::Trace(IDeviceContext* pContext, const RTXPTRayTracingDispatch& DispatchInfo)
{
    return Dispatch(pContext, RTXPTPathTraceVariant::Reference, DispatchInfo);
}

void RTXPTRayTracingPass::InsertUAVBarrier(IDeviceContext* pContext, ITextureView* pTextureUAV)
{
    if (pContext == nullptr || pTextureUAV == nullptr || pTextureUAV->GetTexture() == nullptr)
        return;

    StateTransitionDesc Barrier{pTextureUAV->GetTexture(),
                                RESOURCE_STATE_UNORDERED_ACCESS,
                                RESOURCE_STATE_UNORDERED_ACCESS,
                                STATE_TRANSITION_FLAG_UPDATE_STATE};
    pContext->TransitionResourceState(Barrier);
}

void RTXPTRayTracingPass::InsertUAVBarrier(IDeviceContext* pContext, IBuffer* pBuffer)
{
    if (pContext == nullptr || pBuffer == nullptr)
        return;

    StateTransitionDesc Barrier{pBuffer,
                                RESOURCE_STATE_UNORDERED_ACCESS,
                                RESOURCE_STATE_UNORDERED_ACCESS,
                                STATE_TRANSITION_FLAG_UPDATE_STATE};
    pContext->TransitionResourceState(Barrier);
}

void RTXPTRayTracingPass::InsertUAVBarrier(IDeviceContext* pContext, IBufferView* pBufferUAV)
{
    InsertUAVBarrier(pContext, pBufferUAV != nullptr ? pBufferUAV->GetBuffer() : nullptr);
}

bool RTXPTRayTracingPass::IsReady(RTXPTPathTraceVariant Variant) const
{
    const size_t VariantIdx = GetVariantIndex(Variant);
    return VariantIdx < m_Variants.size() && m_Variants[VariantIdx].Stats.Ready;
}

const RTXPTRayTracingVariantStats& RTXPTRayTracingPass::GetVariantStats(RTXPTPathTraceVariant Variant) const
{
    const size_t VariantIdx = GetVariantIndex(Variant);
    if (VariantIdx < m_Variants.size())
        return m_Variants[VariantIdx].Stats;

    static constexpr RTXPTRayTracingVariantStats EmptyStats{};
    return EmptyStats;
}

} // namespace Diligent
