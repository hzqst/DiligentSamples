// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once

#include <algorithm>

#include "EngineFactory.h"
#include "RenderDevice.h"
#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "AsyncPrefix.hpp"

namespace Diligent
{
// Native descriptor templates are captured once after the CPU bootstrap build.
// They preserve backend-specific flags, BLAS addresses and Diligent's SBT mapping.
// Recreate this object whenever the instance topology or BLAS addresses change.
class GPUTLAS
{
public:
    struct InstanceData
    {
        Uint32 TransformIndex;
        Uint32 CustomId;
        Uint32 Mask;
        Uint32 Padding = 0;
    };
    static_assert(sizeof(InstanceData) == 16, "TLAS input layout");

    bool Initialize(IRenderDevice* Device, IDeviceContext* Context, IEngineFactory* Factory, IBuffer* InstanceBuffer, const InstanceData* Instances, Uint32 Count, const char* ShaderRoot = "shaders")
    {
        if (Count == 0 || (Device->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_D3D12 && Device->GetDeviceInfo().Type != RENDER_DEVICE_TYPE_VULKAN)) return false;
        RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
        Factory->CreateDefaultShaderSourceStreamFactory(ShaderRoot, &Source);
        ShaderCreateInfo S;
        S.Desc.Name                  = "GPU TLAS instance preparation";
        S.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
        S.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        S.ShaderCompiler             = SHADER_COMPILER_DXC;
        S.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
        S.HLSLVersion                = {6, 0};
        S.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
        S.FilePath                   = "TLASCull.hlsl";
        S.EntryPoint                 = "main";
        S.pShaderSourceStreamFactory = Source;
        RefCntAutoPtr<IShader> Shader;
        Device->CreateShader(S, &Shader);
        if (!Shader) return false;
        ComputePipelineStateCreateInfo P;
        P.PSODesc.Name                               = "GPU TLAS instance preparation";
        P.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
        P.PSODesc.ImmediateContextMask               = kRTXPTImmediateContextMask;
        P.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        P.pCS                                        = Shader;
        Device->CreateComputePipelineState(P, &m_PSO);
        if (!m_PSO) return false;
        BufferDesc B;
        B.Name                 = "GPU TLAS descriptor templates";
        B.ImmediateContextMask = kRTXPTImmediateContextMask;
        B.Size                 = Uint64{Count} * TLAS_INSTANCE_DATA_SIZE;
        B.BindFlags            = BIND_SHADER_RESOURCE;
        B.Mode                 = BUFFER_MODE_RAW;
        Device->CreateBuffer(B, nullptr, &m_Templates);
        B.Name              = "GPU TLAS instance data";
        B.Size              = Uint64{Count} * sizeof(InstanceData);
        B.Mode              = BUFFER_MODE_STRUCTURED;
        B.ElementByteStride = sizeof(InstanceData);
        BufferData Data{Instances, B.Size};
        Device->CreateBuffer(B, &Data, &m_Instances);
        if (!m_Templates || !m_Instances) return false;
        Context->CopyBuffer(InstanceBuffer, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                            m_Templates, 0, m_Templates->GetDesc().Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        m_PSO->CreateShaderResourceBinding(&m_SRB, true);
        if (!m_SRB) return false;
        m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_Templates")->Set(m_Templates->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_InstanceData")->Set(m_Instances->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        m_Count = Count;
        return true;
    }

    void UpdateInstanceData(IDeviceContext* Context, const InstanceData* Instances)
    {
        Context->UpdateBuffer(m_Instances, 0, m_Instances->GetDesc().Size, Instances, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    bool Prepare(IDeviceContext* Context, IBuffer* Transforms, IBuffer* Output)
    {
        if (!m_SRB || !Transforms || !Output) return false;
        m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_Transforms")->Set(Transforms->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
        m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Instances")->Set(Output->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
        Context->SetPipelineState(m_PSO);
        Context->CommitShaderResources(m_SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        // Two-dimensional dispatch also handles scenes exceeding 65535 groups.
        const Uint32 Groups = (m_Count + 63) / 64;
        Context->DispatchCompute(DispatchComputeAttribs{std::min(Groups, 65535u), (Groups + 65534) / 65535, 1});
        // BuildTLAS transitions the UAV writes to AS build reads on this context.
        return true;
    }

    Uint64 GetMemorySize() const { return m_Templates ? m_Templates->GetDesc().Size + m_Instances->GetDesc().Size : 0; }

private:
    RefCntAutoPtr<IPipelineState>         m_PSO;
    RefCntAutoPtr<IShaderResourceBinding> m_SRB;
    RefCntAutoPtr<IBuffer>                m_Templates;
    RefCntAutoPtr<IBuffer>                m_Instances;
    Uint32                                m_Count = 0;
};
} // namespace Diligent
