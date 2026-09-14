// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "RTXPTEnvironmentLights.hpp"
#include <cmath>

namespace Diligent
{

class RTXPTLightFeedback
{
public:
    void Reset()
    {
        for (auto& PSO : m_PSOs) PSO.Release();
        for (auto& SRB : m_SRBs) SRB.Release();
        for (auto& Texture : m_Textures) Texture.Release();
        m_Constants.Release();
        m_Counts.Release();
        m_OutputWeights.Release();
        m_OutputCandidates.Release();
        m_Frame         = 0;
        m_PreviousWidth = m_PreviousHeight = 0;
    }

    bool Initialize(IRenderDevice* pDevice, IEngineFactory* pFactory, IRenderStateCache* pCache, Uint32 Width, Uint32 Height)
    {
        Reset();
        RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
        pFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders/PathTracer", &Source);
        const char* Entries[] = {"ResetFeedbackCounts", "PreFilterFeedback", "BlendFeedback", "ReprojectFeedback", "FillFeedbackTile", "RetainFeedbackHistory"};
        for (size_t I = 0; I < m_PSOs.size(); ++I)
        {
            ShaderCreateInfo SCI;
            SCI.Desc                       = {Entries[I], SHADER_TYPE_COMPUTE, false};
            SCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
            SCI.ShaderCompiler             = SHADER_COMPILER_DXC;
            SCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
            SCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
            SCI.FilePath                   = "PathTracer/Lighting/LightFeedback.hlsl";
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
            PCI.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
            PCI.pCS                                        = Shader;
            if (pCache) pCache->CreateComputePipelineState(PCI, &m_PSOs[I]);
            else
                pDevice->CreateComputePipelineState(PCI, &m_PSOs[I]);
            if (!m_PSOs[I]) return false;
            m_PSOs[I]->CreateShaderResourceBinding(&m_SRBs[I], true);
            if (!m_SRBs[I]) return false;
        }
        BufferDesc Constants;
        Constants.Name      = "RTXPT feedback frame constants";
        Constants.Size      = 12 * sizeof(Uint32);
        Constants.BindFlags = BIND_UNIFORM_BUFFER;
        pDevice->CreateBuffer(Constants, nullptr, &m_Constants);
        for (size_t I = 0; I < m_Textures.size(); ++I)
        {
            TextureDesc Desc;
            Desc.Name      = "RTXPT temporal light feedback";
            Desc.Type      = RESOURCE_DIM_TEX_2D;
            Desc.Width     = std::max(Width, 1u);
            Desc.Height    = std::max(Height, 1u);
            Desc.Format    = I == 3 ? TEX_FORMAT_R32_FLOAT : TEX_FORMAT_RG32_UINT;
            Desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
            pDevice->CreateTexture(Desc, nullptr, &m_Textures[I]);
            if (!m_Textures[I]) return false;
        }
        return m_Constants != nullptr;
    }

    bool SetLightCount(IRenderDevice* pDevice, Uint32 Count)
    {
        m_Counts.Release();
        BufferDesc Desc;
        Desc.Name              = "RTXPT per-light temporal feedback counts";
        Desc.Size              = (Uint64{Count} + 1) * sizeof(Uint32);
        Desc.Mode              = BUFFER_MODE_STRUCTURED;
        Desc.ElementByteStride = sizeof(Uint32);
        Desc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        pDevice->CreateBuffer(Desc, nullptr, &m_Counts);
        m_LightCount = Count;
        return m_Counts != nullptr;
    }

    bool Prepare(IDeviceContext* pContext, IBuffer* pControl, IBuffer* pProxies, IBuffer* pLocal, ITexture* pWeights, ITexture* pCandidates, ITextureView* pDepth, ITextureView* pMotion, bool ResetHistory, IBuffer* pHistoryRemap = nullptr)
    {
        if (!m_Counts || !m_Constants || !pDepth || !pMotion) return false;
        m_OutputWeights       = pWeights;
        m_OutputCandidates    = pCandidates;
        const auto& DepthDesc = pDepth->GetTexture()->GetDesc();
        m_Width               = DepthDesc.Width;
        m_Height              = DepthDesc.Height;
        if (m_Width > m_Textures[0]->GetDesc().Width || m_Height > m_Textures[0]->GetDesc().Height) return false;
        ResetHistory |= m_Frame == 0u || m_PreviousWidth != m_Width || m_PreviousHeight != m_Height;
        m_PreviousWidth  = m_Width;
        m_PreviousHeight = m_Height;
        if (ResetHistory)
        {
            m_Frame   = 0;
            m_JitterX = m_JitterY = 0;
            m_JitterFX = m_JitterFY = 0.0f;
        }
        const Uint32 PreviousJitterX = m_JitterX, PreviousJitterY = m_JitterY;
        if (m_Frame % 1024u == 0u) m_JitterFX = m_JitterFY = 0.0f;
        constexpr float PlasticConstant = 1.32471795724474602596f;
        m_JitterFX                      = std::fmod(m_JitterFX + 1.0f / PlasticConstant, 1.0f);
        m_JitterFY                      = std::fmod(m_JitterFY + 1.0f / (PlasticConstant * PlasticConstant), 1.0f);
        m_JitterX                       = std::min(static_cast<Uint32>(m_JitterFX * 8.0f), 7u);
        m_JitterY                       = std::min(static_cast<Uint32>(m_JitterFY * 8.0f), 7u);
        const Uint32 Constants[]        = {m_Width, m_Height, ++m_Frame, ResetHistory ? 1u : 0u,
                                    m_JitterX, m_JitterY, PreviousJitterX, PreviousJitterY, pHistoryRemap ? 1u : 0u, 0u, 0u, 0u};
        pContext->UpdateBuffer(m_Constants, 0, sizeof(Constants), Constants, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        m_TilesX = (m_Width + 14u) / 8u;
        m_TilesY = (m_Height + 14u) / 8u;
        for (auto& SRB : m_SRBs)
        {
            auto Set = [&](const char* Name, IDeviceObject* Object) {
                if (auto* Variable = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name)) Variable->Set(Object);
            };
            Set("g_FeedbackConstants", m_Constants);
            Set("u_LightingControl", pControl->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
            Set("u_FeedbackCounts", m_Counts->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
            Set("t_FeedbackTotalWeight", pWeights->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
            Set("t_FeedbackCandidates", pCandidates->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
            Set("u_FeedbackTotalWeight", pWeights->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            Set("u_FeedbackCandidates", pCandidates->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            Set("u_Filtered", m_Textures[0]->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            Set("u_Blended", m_Textures[1]->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            Set("u_Processed", m_Textures[2]->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            Set("u_HistoryDepth", m_Textures[3]->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
            Set("t_Depth", pDepth);
            Set("t_Motion", pMotion);
            Set("t_LightSamplingProxies", pProxies->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
            Set("t_HistoryRemapPastToCurrent", (pHistoryRemap ? pHistoryRemap : pProxies)->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
            Set("u_LocalSamplingBuffer", pLocal->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        }
        Dispatch(pContext, 0, (m_LightCount + 256u) / 256u, 1u);
        Barrier(pContext, pControl);
        Barrier(pContext, m_Counts);
        Dispatch(pContext, 1, (m_Width + 7u) / 8u, (m_Height + 7u) / 8u);
        Barrier(pContext, m_Textures[0]);
        Barrier(pContext, m_Counts);
        return true;
    }

    bool Finish(IDeviceContext* pContext, IBuffer* pLocal)
    {
        Dispatch(pContext, 2, (m_Width + 15u) / 16u, (m_Height + 15u) / 16u);
        Barrier(pContext, m_Textures[1]);
        Dispatch(pContext, 3, (m_Width + 7u) / 8u, (m_Height + 7u) / 8u);
        Barrier(pContext, m_Textures[2]);
        Barrier(pContext, pLocal);
        Dispatch(pContext, 4, m_TilesX, m_TilesY);
        Barrier(pContext, pLocal);
        Dispatch(pContext, 5, (m_Width + 7u) / 8u, (m_Height + 7u) / 8u);
        // FILL accesses retained reservoirs without changing their UAV state.
        // Finish scratch reads before the following frame overwrites them.
        Barrier(pContext, m_OutputWeights);
        Barrier(pContext, m_OutputCandidates);
        for (const auto& Texture : m_Textures)
            Barrier(pContext, Texture);
        return true;
    }

    IBuffer* GetCounts() const { return m_Counts; }
    Uint32   GetHistoryFrameCount() const { return m_Frame; }

private:
    template <typename ResourceType>
    static void Barrier(IDeviceContext* pContext, const ResourceType& pObject)
    {
        StateTransitionDesc Transition{pObject, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE};
        pContext->TransitionResourceState(Transition);
    }
    void Dispatch(IDeviceContext* pContext, size_t Pass, Uint32 X, Uint32 Y)
    {
        pContext->SetPipelineState(m_PSOs[Pass]);
        pContext->CommitShaderResources(m_SRBs[Pass], RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        DispatchComputeAttribs Attribs;
        Attribs.ThreadGroupCountX = X;
        Attribs.ThreadGroupCountY = Y;
        pContext->DispatchCompute(Attribs);
    }
    std::array<RefCntAutoPtr<IPipelineState>, 6>         m_PSOs;
    std::array<RefCntAutoPtr<IShaderResourceBinding>, 6> m_SRBs;
    std::array<RefCntAutoPtr<ITexture>, 4>               m_Textures;
    RefCntAutoPtr<IBuffer>                               m_Constants, m_Counts;
    RefCntAutoPtr<ITexture>                              m_OutputWeights, m_OutputCandidates;
    Uint32                                               m_LightCount = 0, m_Frame = 0;
    Uint32                                               m_Width = 0, m_Height = 0, m_TilesX = 0, m_TilesY = 0;
    Uint32                                               m_PreviousWidth = 0, m_PreviousHeight = 0;
    Uint32                                               m_JitterX = 0, m_JitterY = 0;
    float                                                m_JitterFX = 0.0f, m_JitterFY = 0.0f;
};

} // namespace Diligent
