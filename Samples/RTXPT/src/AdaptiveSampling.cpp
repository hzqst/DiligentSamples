// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#include "AdaptiveSampling.hpp"
#include "RTXPTFrameConstants.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"
#include "GraphicsTypesX.hpp"
#include "../shaders/PathTracer/SubSampling.h"

#include "AsyncPrefix.hpp"

namespace Diligent
{
bool AdaptiveSampling::Initialize(IRenderDevice* Device, IEngineFactory* Factory, IRenderStateCache* Cache, IBuffer* FrameConstants)
{
    BufferDesc CB;
    CB.ImmediateContextMask = kRTXPTImmediateContextMask;
    CB.Name                 = "SubSampling mini constants";
    CB.Size                 = sizeof(SampleMiniConstants);
    CB.BindFlags            = BIND_UNIFORM_BUFFER;
    CB.Usage                = USAGE_DYNAMIC;
    CB.CPUAccessFlags       = CPU_ACCESS_WRITE;
    if (!m_MiniConstants)
        Device->CreateBuffer(CB, nullptr, &m_MiniConstants);
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    Factory->CreateDefaultShaderSourceStreamFactory("shaders", &Source);
    ShaderCreateInfo S;
    S.Desc                       = {"SubSampling compaction", SHADER_TYPE_COMPUTE, false};
    S.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    S.ShaderCompiler             = SHADER_COMPILER_DXC;
    S.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    S.HLSLVersion                = {6, 0};
    S.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    S.FilePath                   = "PathTracer/SubSamplingCompaction.hlsl";
    S.EntryPoint                 = "main";
    S.pShaderSourceStreamFactory = Source;
    RefCntAutoPtr<IShader> Shader;
    Cache->CreateShader(S, &Shader);
    if (!Shader || !m_MiniConstants) return false;
    ComputePipelineStateCreateInfo P;
    P.PSODesc.Name                               = "SubSampling compaction";
    P.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
    P.PSODesc.ImmediateContextMask               = kRTXPTImmediateContextMask;
    P.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    P.pCS                                        = Shader;
    m_CompactionSRB.Release();
    m_CompactionPSO.Release();
    Cache->CreateComputePipelineState(P, &m_CompactionPSO);
    if (!m_CompactionPSO) return false;
    m_CompactionPSO->CreateShaderResourceBinding(&m_CompactionSRB, true);
    m_CompactionSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "g_Const")->Set(FrameConstants);
    m_CompactionSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "g_MiniConst")->Set(m_MiniConstants);
    return true;
}

bool AdaptiveSampling::PrepareSubSampling(IRenderDevice* Device, IDeviceContext* Context, const RTXPTRealtimeSettings& Settings, SampleMiniConstants& Mini, RTXPTRayTracingDispatch& Dispatch)
{
    const Uint32 Width = Dispatch.Width, Height = Dispatch.Height;
    const int    Mode    = Width <= 65535 && Height <= 65535 ? Settings.ActualSubSamplingMode() : 0;
    const Uint32 Pattern = static_cast<Uint32>(std::clamp(Settings.RealtimeSubSamplingPattern, 0, 3));
    const Uint32 Block   = Settings.RealtimeSubSamplingBlockSize <= 4 ? 4 : (Settings.RealtimeSubSamplingBlockSize <= 8 ? 8 : 16);
    const Uint32 ScaleX = Mode ? 2 : 1, ScaleY = Mode == 1 || Mode == 3 ? 2 : 1;
    const Uint32 RateNum = Mode == 1 ? 3 : 1;
    Mini.params1         = uint4{ScaleX, ScaleY, ScaleX * ScaleY, RateNum};
    Mini.params2         = uint4{Pattern, Block, 0, 0};
    const bool   Compact = Mode && Pattern == RTXPT_SUB_SAMPLING_PATTERN_STOCHASTIC_COMPACTED;
    const Uint64 Bytes   = RTXPT_SUB_SAMPLING_LIST_HEADER_BYTES + (Compact ? Uint64{Width} * Height * sizeof(Uint32) : 0);
    if (!m_PixelList || m_PixelList->GetDesc().Size != Bytes)
    {
        if (m_CompactionSRB)
            m_CompactionSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_SubSamplingPixelList")->Set(nullptr);
        m_PixelList.Release();
        BufferDesc B;
        B.ImmediateContextMask = kRTXPTImmediateContextMask;
        B.Name                 = "SubSampling pixel list";
        B.Size                 = Bytes;
        B.BindFlags            = BIND_UNORDERED_ACCESS;
        B.Mode                 = BUFFER_MODE_RAW;
        Device->CreateBuffer(B, nullptr, &m_PixelList);
    }
    if (!m_PixelList) return false;
    Dispatch.pSubSamplingPixelList = m_PixelList->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
    if (Compact)
    {
        if (!m_CompactionSRB) return false;
        const Uint32 Zero = 0;
        Context->UpdateBuffer(m_PixelList, 0, sizeof(Zero), &Zero, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        MapHelper<SampleMiniConstants> Mapped{Context, m_MiniConstants, MAP_WRITE, MAP_FLAG_DISCARD};
        if (!Mapped) return false;
        *Mapped = Mini;
        Mapped.Unmap();
        m_CompactionSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_SubSamplingPixelList")->Set(Dispatch.pSubSamplingPixelList);
        Context->SetPipelineState(m_CompactionPSO);
        Context->CommitShaderResources(m_CompactionSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->DispatchCompute(DispatchComputeAttribs{(Width + 7) / 8, (Height + 7) / 8, 1});
        RTXPTRayTracingPass::InsertUAVBarrier(Context, m_PixelList);
        // Absolute upper bound: surplus threads exit before tracing. No selected pixel is dropped.
    }
    else if (Mode)
    {
        if (Pattern == RTXPT_SUB_SAMPLING_PATTERN_BLOCK_JITTER)
        {
            Dispatch.Width  = ((Width + ScaleX - 1) / ScaleX) * RateNum;
            Dispatch.Height = (Height + ScaleY - 1) / ScaleY;
        }
        else
        {
            Dispatch.Width  = ((Width + Block - 1) / Block) * (Block * Block * RateNum / (ScaleX * ScaleY));
            Dispatch.Height = (Height + Block - 1) / Block;
        }
    }
    return true;
}
} // namespace Diligent
