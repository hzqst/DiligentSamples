// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#include "RTXPTSuperResolutionInputPass.hpp"
#include "AsyncPrefix.hpp"
#include "RTXPTShaderSource.hpp"
#include "DebugUtilities.hpp"
#include "Float16.hpp"
#include "GraphicsTypesX.hpp"
#include "RenderStateCache.h"

namespace Diligent
{
namespace
{
constexpr Uint32 kThreadGroupSize = 8;

bool SupportsBindFlags(IRenderDevice* Device, TEXTURE_FORMAT Format, BIND_FLAGS BindFlags)
{
    return (Device->GetTextureFormatInfoExt(Format).BindFlags & BindFlags) == BindFlags;
}

ITextureView* GetSRV(const RefCntAutoPtr<ITexture>& Texture)
{
    return Texture ? Texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
}

ITextureView* GetUAV(const RefCntAutoPtr<ITexture>& Texture)
{
    return Texture ? Texture->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) : nullptr;
}

bool CoversSize(ITextureView* View, TEXTURE_FORMAT Format, Uint32 Width, Uint32 Height)
{
    if (View == nullptr || View->GetDesc().ViewType != TEXTURE_VIEW_SHADER_RESOURCE || View->GetDesc().Format != Format)
        return false;
    const TextureDesc& Desc = View->GetTexture()->GetDesc();
    return Desc.Width >= Width && Desc.Height >= Height;
}
} // namespace

void RTXPTSuperResolutionInputPass::Reset()
{
    m_PSO.Release();
    m_SRB.Release();
    m_Depth.Release();
    m_MotionVectors.Release();
    m_IgnoreHistoryMask.Release();
    m_Exposure.Release();
    m_Width = m_Height = 0;
}

bool RTXPTSuperResolutionInputPass::Initialize(IRenderDevice* Device, IEngineFactory* Factory, IRenderStateCache* Cache, IBuffer* FrameConstants)
{
    Reset();
    if (Device == nullptr || Factory == nullptr || FrameConstants == nullptr ||
        Device->GetDeviceInfo().Features.ComputeShaders != DEVICE_FEATURE_STATE_ENABLED)
        return false;
    constexpr BIND_FLAGS UavFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    if (!SupportsBindFlags(Device, DepthFormat, UavFlags) || !SupportsBindFlags(Device, MotionVectorsFormat, UavFlags) ||
        !SupportsBindFlags(Device, IgnoreHistoryMaskFormat, UavFlags) || !SupportsBindFlags(Device, ExposureFormat, BIND_SHADER_RESOURCE))
        return false;

    RefCntAutoPtr<IShaderSourceInputStreamFactory> Source;
    CreateRTXPTShaderSourceFactory(Factory, "shaders", &Source);
    if (!Source)
        return false;
    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.Desc.Name                  = "RTXPT super resolution inputs";
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = "PostProcessing/RTXPTSuperResolutionInputs.csh";
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.pShaderSourceStreamFactory = Source;
    RefCntAutoPtr<IShader> CS;
    if (Cache != nullptr)
        Cache->CreateShader(ShaderCI, &CS);
    else
        Device->CreateShader(ShaderCI, &CS);
    if (!CS)
        return false;

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name                 = "RTXPT super resolution inputs PSO";
    PSOCreateInfo.PSODesc.PipelineType         = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.PSODesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    PSOCreateInfo.pCS                          = CS;
    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    ResourceLayout.AddVariable(SHADER_TYPE_COMPUTE, "g_Const", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;
    if (Cache != nullptr)
        Cache->CreateComputePipelineState(PSOCreateInfo, &m_PSO);
    else
        Device->CreateComputePipelineState(PSOCreateInfo, &m_PSO);
    if (!m_PSO)
        return false;
    m_PSO->CreateShaderResourceBinding(&m_SRB, true);
    auto* ConstantsVar = m_SRB ? m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "g_Const") : nullptr;
    if (ConstantsVar == nullptr)
        return false;
    ConstantsVar->Set(FrameConstants);

    const Uint16      One = Float16{1.0f}.Raw();
    TextureSubResData ExposureData{&One, sizeof(One)};
    TextureData       ExposureInit{&ExposureData, 1};
    TextureDesc       ExposureDesc;
    ExposureDesc.Name                 = "RTXPT super resolution exposure";
    ExposureDesc.Type                 = RESOURCE_DIM_TEX_2D;
    ExposureDesc.Width                = 1;
    ExposureDesc.Height               = 1;
    ExposureDesc.Format               = ExposureFormat;
    ExposureDesc.Usage                = USAGE_IMMUTABLE;
    ExposureDesc.BindFlags            = BIND_SHADER_RESOURCE;
    ExposureDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    Device->CreateTexture(ExposureDesc, &ExposureInit, &m_Exposure);
    if (!m_SRB || !m_Exposure)
    {
        Reset();
        return false;
    }
    return true;
}

bool RTXPTSuperResolutionInputPass::Resize(IRenderDevice* Device, Uint32 Width, Uint32 Height)
{
    m_Depth.Release();
    m_MotionVectors.Release();
    m_IgnoreHistoryMask.Release();
    m_Width = m_Height = 0;
    if (!m_PSO || Device == nullptr || Width == 0 || Height == 0)
        return false;
    const auto Create = [&](const char* Name, TEXTURE_FORMAT Format, RefCntAutoPtr<ITexture>& Texture) {
        TextureDesc Desc;
        Desc.Name                 = Name;
        Desc.Type                 = RESOURCE_DIM_TEX_2D;
        Desc.Width                = Width;
        Desc.Height               = Height;
        Desc.Format               = Format;
        Desc.BindFlags            = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
        Device->CreateTexture(Desc, nullptr, &Texture);
        return GetSRV(Texture) != nullptr && GetUAV(Texture) != nullptr;
    };
    if (!Create("RTXPT super resolution depth", DepthFormat, m_Depth) ||
        !Create("RTXPT super resolution motion vectors", MotionVectorsFormat, m_MotionVectors) ||
        !Create("RTXPT super resolution ignore-history mask", IgnoreHistoryMaskFormat, m_IgnoreHistoryMask))
    {
        m_Depth.Release();
        m_MotionVectors.Release();
        m_IgnoreHistoryMask.Release();
        return false;
    }
    m_Width  = Width;
    m_Height = Height;
    return true;
}

bool RTXPTSuperResolutionInputPass::Prepare(IDeviceContext* Context, ITextureView* RayDistanceSRV, ITextureView* MotionVectorsSRV)
{
    if (!IsReady() || Context == nullptr ||
        !CoversSize(RayDistanceSRV, TEX_FORMAT_R32_FLOAT, m_Width, m_Height) ||
        !CoversSize(MotionVectorsSRV, TEX_FORMAT_RGBA16_FLOAT, m_Width, m_Height))
        return false;
    const auto Bind = [&](const char* Name, IDeviceObject* Object) {
        auto* Variable = m_SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name);
        if (Variable == nullptr)
            return false;
        Variable->Set(Object);
        return true;
    };
    if (!Bind("t_RTXPTDepth", RayDistanceSRV) || !Bind("t_RTXPTMotionVectors", MotionVectorsSRV) ||
        !Bind("u_SRDepth", GetUAV(m_Depth)) || !Bind("u_SRMotionVectors", GetUAV(m_MotionVectors)) ||
        !Bind("u_SRIgnoreHistoryMask", GetUAV(m_IgnoreHistoryMask)))
        return false;
    Context->SetPipelineState(m_PSO);
    Context->CommitShaderResources(m_SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Context->DispatchCompute(DispatchComputeAttribs{(m_Width + kThreadGroupSize - 1) / kThreadGroupSize,
                                                    (m_Height + kThreadGroupSize - 1) / kThreadGroupSize, 1});
    return true;
}

ITextureView* RTXPTSuperResolutionInputPass::GetDepthSRV() const { return GetSRV(m_Depth); }
ITextureView* RTXPTSuperResolutionInputPass::GetMotionVectorsSRV() const { return GetSRV(m_MotionVectors); }
ITextureView* RTXPTSuperResolutionInputPass::GetIgnoreHistoryMaskSRV() const { return GetSRV(m_IgnoreHistoryMask); }
ITextureView* RTXPTSuperResolutionInputPass::GetExposureSRV() const { return GetSRV(m_Exposure); }
} // namespace Diligent
