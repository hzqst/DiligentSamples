// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <array>
#include <algorithm>

#include "Buffer.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "RenderStateCache.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"
#include "../shaders/PathTracer/Lighting/EnvironmentLightConfig.h"

#include "AsyncPrefix.hpp"

namespace Diligent
{

// Owns the upstream two-stage environment quadtree and direction-to-light map.
// Rebuilt with the processed environment; RTXPTLightHistory maps changed node IDs
// through the previous and current lookup maps before consuming temporal feedback.
class RTXPTEnvironmentLights
{
public:
    void Reset()
    {
        for (auto& PSO : m_PSOs) PSO.Release();
        for (auto& SRB : m_SRBs) SRB.Release();
        m_Lights.Release();
        m_BaseNodes.Release();
        m_Lookup.Release();
        m_Resolution = 0;
    }

    bool Initialize(IRenderDevice* pDevice, IEngineFactory* pFactory, IRenderStateCache* pCache)
    {
        Reset();
        RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
        pFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders/PathTracer", &Source);
        const char* Entries[] = {"EnvLightsSubdivideBase", "EnvLightsSubdivideBoost", "EnvLightsFillLookupMap"};
        for (size_t I = 0; I < m_PSOs.size(); ++I)
        {
            ShaderCreateInfo SCI;
            SCI.Desc                       = {Entries[I], SHADER_TYPE_COMPUTE, false};
            SCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
            SCI.ShaderCompiler             = SHADER_COMPILER_DXC;
            SCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
            SCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
            SCI.FilePath                   = "PathTracer/Lighting/EnvironmentLightsBaker.hlsl";
            SCI.EntryPoint                 = Entries[I];
            SCI.pShaderSourceStreamFactory = Source;
            RefCntAutoPtr<IShader> Shader;
            if (pCache) pCache->CreateShader(SCI, &Shader);
            else
                pDevice->CreateShader(SCI, &Shader);
            if (!Shader) return false;
            ComputePipelineStateCreateInfo PCI;
            PCI.PSODesc.Name                               = Entries[I];
            PCI.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
            PCI.PSODesc.ImmediateContextMask               = kRTXPTImmediateContextMask;
            PCI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
            PCI.pCS                                        = Shader;
            if (pCache) pCache->CreateComputePipelineState(PCI, &m_PSOs[I]);
            else
                pDevice->CreateComputePipelineState(PCI, &m_PSOs[I]);
            if (!m_PSOs[I]) return false;
            m_PSOs[I]->CreateShaderResourceBinding(&m_SRBs[I], true);
            if (!m_SRBs[I]) return false;
        }
        return true;
    }

    bool CreateResources(IRenderDevice* pDevice, Uint32 Resolution)
    {
        // The base pass reserves three MIP levels for boost subdivision. Smaller
        // maps cannot accommodate the upstream fixed number of leaves.
        if (Resolution != 0 && (Resolution < 128 || (Resolution & (Resolution - 1)) != 0))
            return false;
        if (m_Lights && m_Lookup && m_Resolution == Resolution) return true;
        m_Resolution      = Resolution;
        auto CreateBuffer = [&](const char* Name, Uint32 Count, Uint32 Stride, RefCntAutoPtr<IBuffer>& Buffer) {
            Buffer.Release();
            BufferDesc Desc;
            Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
            Desc.Name              = Name;
            Desc.Size              = Uint64{Count} * Stride;
            Desc.Mode              = BUFFER_MODE_STRUCTURED;
            Desc.ElementByteStride = Stride;
            Desc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
            pDevice->CreateBuffer(Desc, nullptr, &Buffer);
            return Buffer != nullptr;
        };
        // Upstream PolymorphicLightInfo: two packed uint4 blocks in both backends.
        if (!CreateBuffer("RTXPT environment quads", RTXPT_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT, 8 * sizeof(Uint32), m_Lights) ||
            !CreateBuffer("RTXPT environment base nodes", RTXPT_NEEAT_ENVMAP_QT_UNBOOSTED_NODE_COUNT, sizeof(Uint32), m_BaseNodes))
            return false;
        m_Lookup.Release();
        TextureDesc Desc;
        Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
        Desc.Name  = "RTXPT environment light lookup";
        Desc.Type  = RESOURCE_DIM_TEX_2D;
        Desc.Width = Desc.Height = std::max(Resolution, 1u);
        Desc.Format              = TEX_FORMAT_R32_UINT;
        Desc.BindFlags           = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        pDevice->CreateTexture(Desc, nullptr, &m_Lookup);
        return m_Lookup != nullptr;
    }

    bool Build(IDeviceContext* pContext, IBuffer* pControl, ITextureView* pRadianceMap)
    {
        if (m_Resolution == 0) return true;
        if (!m_Lights || !m_Lookup || !pRadianceMap || !pControl) return false;
        const Uint32 Groups[] = {1u, RTXPT_NEEAT_ENVMAP_QT_UNBOOSTED_NODE_COUNT, RTXPT_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT};
        for (size_t I = 0; I < m_PSOs.size(); ++I)
        {
            if (!m_PSOs[I] || !m_SRBs[I]) return false;
            auto Set = [&](const char* Name, IDeviceObject* Object) {
                if (auto* Variable = m_SRBs[I]->GetVariableByName(SHADER_TYPE_COMPUTE, Name)) Variable->Set(Object);
            };
            Set("t_LightingControl", pControl->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
            Set("t_EnvironmentRadianceMap", pRadianceMap);
            Set("u_EnvironmentBaseNodes", m_BaseNodes->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
            Set("u_EnvironmentLights", m_Lights->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
            Set("u_EnvironmentLightLookup", m_Lookup->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            pContext->SetPipelineState(m_PSOs[I]);
            pContext->CommitShaderResources(m_SRBs[I], RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            DispatchComputeAttribs Dispatch;
            Dispatch.ThreadGroupCountX = Groups[I];
            pContext->DispatchCompute(Dispatch);
            if (I < 2)
            {
                StateTransitionDesc Barrier{I == 0 ? m_BaseNodes.RawPtr() : m_Lights.RawPtr(),
                                            RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS,
                                            STATE_TRANSITION_FLAG_UPDATE_STATE};
                pContext->TransitionResourceState(Barrier);
            }
        }
        return true;
    }

    Uint32        GetLightCount() const { return m_Resolution != 0 ? RTXPT_NEEAT_ENVMAP_QT_TOTAL_NODE_COUNT : 0; }
    IBuffer*      GetLights() const { return m_Lights; }
    ITextureView* GetLookupSRV() const { return m_Lookup ? m_Lookup->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr; }

    void ReleaseQueueResources(IDeviceContext* Context)
    {
        ReleasePrefixResource(Context, m_Lookup.RawPtr());
        ReleasePrefixResource(Context, m_Lights.RawPtr());
        ReleasePrefixResource(Context, m_BaseNodes.RawPtr());
    }

private:
    std::array<RefCntAutoPtr<IPipelineState>, 3>         m_PSOs;
    std::array<RefCntAutoPtr<IShaderResourceBinding>, 3> m_SRBs;
    RefCntAutoPtr<IBuffer>                               m_Lights;
    RefCntAutoPtr<IBuffer>                               m_BaseNodes;
    RefCntAutoPtr<ITexture>                              m_Lookup;
    Uint32                                               m_Resolution = 0;
};

} // namespace Diligent
