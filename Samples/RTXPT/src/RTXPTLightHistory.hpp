// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once
#include "RTXPTShaderSource.hpp"

#include "RTXPTEnvironmentLights.hpp"
#include "RTXPTLightSourceHistory.hpp"
#include <memory>

#include "AsyncPrefix.hpp"

namespace Diligent
{

class RTXPTLightHistory
{
public:
    void Reset()
    {
        m_PSO.Release();
        m_SRB.Release();
        m_Device.Release();
        m_Constants.Release();
        m_CurrentToPast.Release();
        m_PastToCurrent.Release();
        m_PreviousEnvironment.Release();
        m_PreviousLookup.Release();
        ClearHistory();
    }
    void ClearHistory()
    {
        m_CurrentSources.clear();
        m_PreviousSources.clear();
        m_CurrentCount = m_PreviousCount = m_CurrentEnvCount = m_PreviousEnvCount = 0;
        m_Available                                                               = false;
        m_TableChanged                                                            = true;
    }
    bool Initialize(IRenderDevice* pDevice, IEngineFactory* pFactory, IRenderStateCache* pCache, IPipelineStateCache* pPSOCache)
    {
        Reset();
        m_Device = pDevice;
        RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
        CreateRTXPTShaderSourceFactory(pFactory, "shaders;shaders/PathTracer", &Source);
        ShaderCreateInfo SCI;
        SCI.Desc                       = {"RTXPT light history", SHADER_TYPE_COMPUTE, false};
        SCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        SCI.ShaderCompiler             = SHADER_COMPILER_DXC;
        SCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
        SCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
        SCI.FilePath                   = "PathTracer/Lighting/LightHistory.hlsl";
        SCI.EntryPoint                 = "MapLightHistory";
        SCI.pShaderSourceStreamFactory = Source;
        RefCntAutoPtr<IShader> Shader;
        if (pCache) pCache->CreateShader(SCI, &Shader);
        else
            pDevice->CreateShader(SCI, &Shader);
        if (!Shader) return false;
        ComputePipelineStateCreateInfo PCI;
        PCI.PSODesc.Name                               = "RTXPT light history";
        PCI.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
        PCI.PSODesc.ImmediateContextMask               = kRTXPTImmediateContextMask;
        PCI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        PCI.pCS                                        = Shader;
        PCI.pPSOCache                                  = SelectRTXPTPipelineStateCache(pDevice, PCI, pPSOCache);
        if (pCache) pCache->CreateComputePipelineState(PCI, &m_PSO);
        else
            pDevice->CreateComputePipelineState(PCI, &m_PSO);
        if (!m_PSO) return false;
        m_PSO->CreateShaderResourceBinding(&m_SRB, true);
        BufferDesc Desc;
        Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
        Desc.Name                 = "RTXPT light history constants";
        Desc.Size                 = 32;
        Desc.BindFlags            = BIND_UNIFORM_BUFFER;
        pDevice->CreateBuffer(Desc, nullptr, &m_Constants);
        return m_SRB && m_Constants;
    }
    void SetSources(std::vector<RTXPTLightSourceRange> Sources, Uint32 Count, Uint32 EnvCount)
    {
        m_CurrentSources  = std::move(Sources);
        m_CurrentCount    = Count;
        m_CurrentEnvCount = EnvCount;
        m_TableChanged    = true;
    }
    bool Prepare(IDeviceContext* pContext, IBuffer* pControl, IBuffer* pEnvironment, ITextureView* pLookup)
    {
        // Allocate the snapshot with the live environment resources on first use.
        // It has no contents to preserve during scene initialization.
        if (!m_PreviousEnvironment)
        {
            BufferDesc Desc = pEnvironment->GetDesc();
            Desc.Name       = "RTXPT previous environment nodes";
            Desc.BindFlags  = BIND_SHADER_RESOURCE;
            m_Device->CreateBuffer(Desc, nullptr, &m_PreviousEnvironment);
            if (!m_PreviousEnvironment) return false;
        }
        const Uint64 Size = Uint64{std::max({m_CurrentCount, m_PreviousCount, 1u})} * sizeof(Uint32);
        for (auto* Buffer : {std::addressof(m_CurrentToPast), std::addressof(m_PastToCurrent)})
        {
            if (*Buffer && (*Buffer)->GetDesc().Size >= Size) continue;
            Buffer->Release();
            BufferDesc Desc;
            Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
            Desc.Name                 = "RTXPT light history map";
            Desc.Size                 = Size;
            Desc.Mode                 = BUFFER_MODE_STRUCTURED;
            Desc.ElementByteStride    = sizeof(Uint32);
            Desc.BindFlags            = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
            m_Device->CreateBuffer(Desc, nullptr, &*Buffer);
            if (!*Buffer) return false;
        }
        if (!m_PreviousLookup && !CreateLookup(pLookup->GetTexture()->GetDesc())) return false;
        if (m_TableChanged && m_Available)
        {
            std::vector<Uint32> Forward, Reverse;
            BuildLightHistoryMaps(m_CurrentSources, m_PreviousSources, m_CurrentCount, m_PreviousCount, Forward, Reverse);
            if (!Forward.empty()) pContext->UpdateBuffer(m_CurrentToPast, 0, Forward.size() * sizeof(Uint32), Forward.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            if (!Reverse.empty()) pContext->UpdateBuffer(m_PastToCurrent, 0, Reverse.size() * sizeof(Uint32), Reverse.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        const Uint32 Constants[] = {m_CurrentCount, m_PreviousCount, m_CurrentEnvCount, m_PreviousEnvCount, m_TableChanged ? 1u : 0u, m_Available ? 1u : 0u, 0u, 0u};
        pContext->UpdateBuffer(m_Constants, 0, sizeof(Constants), Constants, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        auto Set = [&](const char* Name, IDeviceObject* Object) {
            if (auto* V = m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name)) V->Set(Object);
        };
        Set("g_HistoryConstants", m_Constants);
        Set("u_LightingControl", pControl->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Set("u_CurrentToPast", m_CurrentToPast->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Set("u_PastToCurrent", m_PastToCurrent->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Set("t_CurrentEnvironment", pEnvironment->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        Set("t_PreviousEnvironment", m_PreviousEnvironment->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        Set("t_CurrentLookup", pLookup);
        Set("t_PreviousLookup", m_PreviousLookup->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
        pContext->SetPipelineState(m_PSO);
        pContext->CommitShaderResources(m_SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        DispatchComputeAttribs Dispatch;
        Dispatch.ThreadGroupCountX = (std::max({m_CurrentCount, m_PreviousCount, 1u}) + 255u) / 256u;
        pContext->DispatchCompute(Dispatch);
        StateTransitionDesc Barrier{pControl, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE};
        pContext->TransitionResourceState(Barrier);
        return true;
    }
    bool Finish(IDeviceContext* pContext, IBuffer* pEnvironment, ITextureView* pLookup)
    {
        if (m_TableChanged || !m_Available)
        {
            const auto& Desc = pLookup->GetTexture()->GetDesc();
            if ((!m_PreviousLookup || m_PreviousLookup->GetDesc().Width != Desc.Width) && !CreateLookup(Desc)) return false;
            pContext->CopyBuffer(pEnvironment, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, m_PreviousEnvironment, 0,
                                 pEnvironment->GetDesc().Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            CopyTextureAttribs Copy;
            Copy.pSrcTexture              = pLookup->GetTexture();
            Copy.pDstTexture              = m_PreviousLookup;
            Copy.SrcTextureTransitionMode = Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            pContext->CopyTexture(Copy);
            m_PreviousSources = m_CurrentSources;
        }
        m_PreviousCount    = m_CurrentCount;
        m_PreviousEnvCount = m_CurrentEnvCount;
        m_Available        = true;
        m_TableChanged     = false;
        return true;
    }
    IBuffer* GetCurrentToPast() const { return m_CurrentToPast; }
    IBuffer* GetPastToCurrent() const { return m_PastToCurrent; }

    void ReleaseQueueResources(IDeviceContext* Context)
    {
        ReleasePrefixResource(Context, m_PreviousLookup.RawPtr());
        for (auto* Buffer : {m_Constants.RawPtr(), m_CurrentToPast.RawPtr(), m_PastToCurrent.RawPtr(), m_PreviousEnvironment.RawPtr()})
            ReleasePrefixResource(Context, Buffer);
    }

private:
    bool CreateLookup(TextureDesc Desc)
    {
        m_PreviousLookup.Release();
        Desc.Name      = "RTXPT previous environment lookup";
        Desc.BindFlags = BIND_SHADER_RESOURCE;
        m_Device->CreateTexture(Desc, nullptr, &m_PreviousLookup);
        return m_PreviousLookup != nullptr;
    }
    RefCntAutoPtr<IRenderDevice>          m_Device;
    RefCntAutoPtr<IPipelineState>         m_PSO;
    RefCntAutoPtr<IShaderResourceBinding> m_SRB;
    RefCntAutoPtr<IBuffer>                m_Constants, m_CurrentToPast, m_PastToCurrent, m_PreviousEnvironment;
    RefCntAutoPtr<ITexture>               m_PreviousLookup;
    std::vector<RTXPTLightSourceRange>    m_CurrentSources, m_PreviousSources;
    Uint32                                m_CurrentCount = 0, m_PreviousCount = 0, m_CurrentEnvCount = 0, m_PreviousEnvCount = 0;
    bool                                  m_Available = false, m_TableChanged = true;
};

} // namespace Diligent
