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
 */

#include "RTXPTTemporalAAPass.hpp"

#include <algorithm>

#include "DebugUtilities.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"

#include "GraphicsTypesX.hpp"

namespace Diligent
{

namespace HLSL
{
#include "Shaders/Common/public/ShaderDefinitions.fxh"
#include "Shaders/Common/public/BasicStructures.fxh"
} // namespace HLSL

namespace
{

float Halton(Uint32 Base, Uint32 Index)
{
    float Result = 0.0f;
    float Factor = 1.0f;
    while (Index > 0)
    {
        Factor /= static_cast<float>(Base);
        Result += Factor * static_cast<float>(Index % Base);
        Index /= Base;
    }
    return Result;
}

float2 PixelJitterToDiligentFXTAAJitter(const float2& PixelJitter, const float2& ViewportSizeInv)
{
    return float2{
        2.0f * PixelJitter.x * ViewportSizeInv.x,
        -2.0f * PixelJitter.y * ViewportSizeInv.y};
}

HLSL::CameraAttribs MakeCameraAttribs(const SampleConstants&      Constants,
                                      const PathTracerViewData&   View,
                                      const PathTracerCameraData& Camera,
                                      Uint32                      FrameIndex)
{
    const float2   TAAJitter    = PixelJitterToDiligentFXTAAJitter(View.PixelOffset, View.ViewportSizeInv);
    const float4x4 JitteredProj = TemporalAntiAliasing::GetJitteredProjMatrix(View.MatViewToClip, TAAJitter);
    const float4x4 ViewProj     = View.MatWorldToView * JitteredProj;

    HLSL::CameraAttribs Attribs = {};
    Attribs.f4Position          = float4{Camera.PosW, 1.0f};
    Attribs.f4ViewportSize      = float4{View.ViewportSize.x, View.ViewportSize.y, View.ViewportSizeInv.x, View.ViewportSizeInv.y};
    Attribs.SetClipPlanes(Camera.NearZ, Camera.FarZ);
    Attribs.fHandness      = 1.0f;
    Attribs.uiFrameIndex   = FrameIndex;
    Attribs.fFocusDistance = Camera.FocalDistance;
    Attribs.fFStop         = Camera.ApertureRadius > 0.0f ? 1.0f / Camera.ApertureRadius : 5.6f;
    Attribs.fFocalLength   = 50.0f;
    Attribs.fSensorWidth   = 36.0f;
    Attribs.fSensorHeight  = 24.0f;
    Attribs.fExposure      = 0.0f;
    Attribs.f2Jitter       = TAAJitter;
    Attribs.mView          = View.MatWorldToView;
    Attribs.mProj          = JitteredProj;
    Attribs.mViewProj      = ViewProj;
    Attribs.mViewInv       = View.MatWorldToView.Inverse();
    Attribs.mProjInv       = JitteredProj.Inverse();
    Attribs.mViewProjInv   = ViewProj.Inverse();
    Attribs.f4ExtraData[0] = Constants.cameraPositionAndTime;
    return Attribs;
}

bool ValidateTemporalInputs(const RTXPTTemporalAAFrameAttribs& Attribs)
{
    if (Attribs.pDevice == nullptr)
        DEV_ERROR("RTXPT Temporal AA requires a render device");
    else if (Attribs.pDeviceContext == nullptr)
        DEV_ERROR("RTXPT Temporal AA requires a device context");
    else if (Attribs.pRenderTargets == nullptr)
        DEV_ERROR("RTXPT Temporal AA requires render targets");
    else if (Attribs.pFrameConstants == nullptr)
        DEV_ERROR("RTXPT Temporal AA requires frame constants");
    else if (Attribs.pRenderTargets->GetRenderWidth() != Attribs.pRenderTargets->GetDisplayWidth() ||
             Attribs.pRenderTargets->GetRenderHeight() != Attribs.pRenderTargets->GetDisplayHeight())
        DEV_ERROR("RTXPT Temporal AA requires render and display dimensions to match");
    else if (Attribs.pFrameConstants->ptConsts.imageWidth != Attribs.pRenderTargets->GetRenderWidth() ||
             Attribs.pFrameConstants->ptConsts.imageHeight != Attribs.pRenderTargets->GetRenderHeight())
        DEV_ERROR("RTXPT Temporal AA frame constants dimensions do not match render targets");
    else if (Attribs.pRenderTargets->GetOutputColorSRV() == nullptr)
        DEV_ERROR("RTXPT Temporal AA requires OutputColor SRV");
    else if (Attribs.pRenderTargets->GetProcessedOutputColorRTV() == nullptr)
        DEV_ERROR("RTXPT Temporal AA requires ProcessedOutputColor RTV");
    else if (Attribs.pRenderTargets->GetDepthSRV() == nullptr)
        DEV_ERROR("RTXPT Temporal AA requires Depth SRV");
    else if (Attribs.pRenderTargets->GetPreviousDepthSRV() == nullptr)
        DEV_ERROR("RTXPT Temporal AA requires PreviousDepth SRV");
    else if (Attribs.pRenderTargets->GetPreviousDepthRTV() == nullptr)
        DEV_ERROR("RTXPT Temporal AA requires PreviousDepth RTV");
    else if (Attribs.pRenderTargets->GetScreenMotionVectorsSRV() == nullptr)
        DEV_ERROR("RTXPT Temporal AA requires ScreenMotionVectors SRV");
    else
        return true;
    return false;
}

bool SupportsBindFlags(IRenderDevice* pDevice, TEXTURE_FORMAT Format, BIND_FLAGS BindFlags)
{
    return pDevice != nullptr && (pDevice->GetTextureFormatInfoExt(Format).BindFlags & BindFlags) == BindFlags;
}

ITextureView* GetDefaultView(const RefCntAutoPtr<ITexture>& Texture, TEXTURE_VIEW_TYPE ViewType)
{
    return Texture ? Texture->GetDefaultView(ViewType) : nullptr;
}

bool SetSRBVariable(IShaderResourceBinding* pSRB, const char* Name, IDeviceObject* pObject)
{
    IShaderResourceVariable* pVar = pSRB != nullptr ? pSRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name) : nullptr;
    if (pVar == nullptr)
    {
        UNEXPECTED("RTXPT Temporal AA input conversion variable is missing: ", Name);
        return false;
    }
    if (pObject == nullptr)
        return false;

    pVar->Set(pObject);
    return true;
}

} // namespace

void RTXPTTemporalAAPass::Reset()
{
    m_TemporalAA.reset();
    m_PostFXContext.reset();
    m_InputConversionPSO.Release();
    m_InputConversionSRB.Release();
    m_FrameConstantsBuffer.Release();
    m_TAADepth.Release();
    m_TAAMotion.Release();
    m_InputWidth  = 0;
    m_InputHeight = 0;
    m_Stats       = {};
}

bool RTXPTTemporalAAPass::Initialize(IRenderDevice* pDevice, IRenderStateCache* pStateCache)
{
    Reset();
    if (pDevice == nullptr)
    {
        DEV_ERROR("RTXPT Temporal AA pass requires a render device");
        return false;
    }

    PostFXContext::CreateInfo PostFXCI;
    PostFXCI.EnableAsyncCreation = false;
    PostFXCI.PackMatrixRowMajor  = true;
    m_PostFXContext              = std::make_unique<PostFXContext>(pDevice, PostFXCI);

    TemporalAntiAliasing::CreateInfo TAACI;
    TAACI.EnableAsyncCreation = false;
    m_TemporalAA              = std::make_unique<TemporalAntiAliasing>(pDevice, TAACI);

    if (!CreateInputConversionPipeline(pDevice, pStateCache))
        return false;

    m_Stats.Ready = m_PostFXContext != nullptr && m_TemporalAA != nullptr && m_InputConversionPSO && m_InputConversionSRB;
    if (!m_Stats.Ready)
        DEV_ERROR("RTXPT Temporal AA pass failed to create DiligentFX TAA objects");
    return m_Stats.Ready;
}

bool RTXPTTemporalAAPass::CreateInputConversionPipeline(IRenderDevice* pDevice, IRenderStateCache* pStateCache)
{
    if (pDevice->GetDeviceInfo().Features.ComputeShaders != DEVICE_FEATURE_STATE_ENABLED)
    {
        DEV_ERROR("RTXPT Temporal AA input conversion requires compute shader support");
        return false;
    }

    if (!SupportsBindFlags(pDevice, TEX_FORMAT_R32_FLOAT, BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS))
    {
        DEV_ERROR("R32F SRV/UAV texture is not supported for RTXPT Temporal AA depth conversion");
        return false;
    }

    if (!SupportsBindFlags(pDevice, TEX_FORMAT_RG16_FLOAT, BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS))
    {
        DEV_ERROR("RG16F SRV/UAV texture is not supported for RTXPT Temporal AA motion conversion");
        return false;
    }

    BufferDesc ConstantsDesc;
    ConstantsDesc.Name           = "RTXPT Temporal AA frame constants";
    ConstantsDesc.Size           = sizeof(SampleConstants);
    ConstantsDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage          = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_FrameConstantsBuffer);
    VERIFY(m_FrameConstantsBuffer, "Failed to create RTXPT Temporal AA frame constants");
    if (!m_FrameConstantsBuffer)
    {
        DEV_ERROR("Failed to create RTXPT Temporal AA frame constants");
        return false;
    }

    IEngineFactory* pEngineFactory = pDevice->GetEngineFactory();
    if (pEngineFactory == nullptr)
    {
        DEV_ERROR("RTXPT Temporal AA input conversion requires an engine factory");
        return false;
    }

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    pEngineFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders\\PostProcessing;shaders\\PathTracer", &pShaderSourceFactory);
    if (!pShaderSourceFactory)
    {
        DEV_ERROR("Failed to create RTXPT Temporal AA shader source factory");
        return false;
    }

    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.Desc.Name                  = "RTXPT Temporal AA input conversion";
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = "PostProcessing/RTXPTTemporalAAInputs.csh";
    ShaderCI.EntryPoint                 = "main";
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    RefCntAutoPtr<IShader> pCS;
    pStateCache->CreateShader(ShaderCI, &pCS);
    VERIFY(pCS, "Failed to create RTXPT Temporal AA input conversion shader");
    if (!pCS)
    {
        DEV_ERROR("Failed to create RTXPT Temporal AA input conversion shader");
        return false;
    }

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = "RTXPT Temporal AA input conversion PSO";
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.pCS                  = pCS;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout
        .AddVariable(SHADER_TYPE_COMPUTE, "g_Const", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_RTXPTDepth", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "t_RTXPTMotionVectors", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_TAADepth", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_COMPUTE, "u_TAAMotion", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    pStateCache->CreateComputePipelineState(PSOCreateInfo, &m_InputConversionPSO);
    VERIFY(m_InputConversionPSO, "Failed to create RTXPT Temporal AA input conversion PSO");
    if (!m_InputConversionPSO)
    {
        DEV_ERROR("Failed to create RTXPT Temporal AA input conversion PSO");
        return false;
    }

    m_InputConversionPSO->CreateShaderResourceBinding(&m_InputConversionSRB, true);
    VERIFY(m_InputConversionSRB, "Failed to create RTXPT Temporal AA input conversion SRB");
    if (!m_InputConversionSRB)
    {
        DEV_ERROR("Failed to create RTXPT Temporal AA input conversion SRB");
        return false;
    }

    if (!SetSRBVariable(m_InputConversionSRB, "g_Const", m_FrameConstantsBuffer))
    {
        DEV_ERROR("RTXPT Temporal AA input conversion failed to bind frame constants");
        return false;
    }

    return true;
}

float2 RTXPTTemporalAAPass::ComputePixelJitter(Uint32 FrameIndex)
{
    constexpr auto SampleCount = Uint32{16};
    const Uint32   Sample      = (FrameIndex % SampleCount) + 1u;
    return float2{
        Halton(2u, Sample) - 0.5f,
        Halton(3u, Sample) - 0.5f};
}

ITextureView* RTXPTTemporalAAPass::GetTAADepthSRV() const
{
    return GetDefaultView(m_TAADepth, TEXTURE_VIEW_SHADER_RESOURCE);
}

ITextureView* RTXPTTemporalAAPass::GetTAADepthUAV() const
{
    return GetDefaultView(m_TAADepth, TEXTURE_VIEW_UNORDERED_ACCESS);
}

ITextureView* RTXPTTemporalAAPass::GetTAAMotionSRV() const
{
    return GetDefaultView(m_TAAMotion, TEXTURE_VIEW_SHADER_RESOURCE);
}

ITextureView* RTXPTTemporalAAPass::GetTAAMotionUAV() const
{
    return GetDefaultView(m_TAAMotion, TEXTURE_VIEW_UNORDERED_ACCESS);
}

bool RTXPTTemporalAAPass::EnsureInputConversionResources(IRenderDevice* pDevice, const RTXPTRenderTargets& RenderTargets)
{
    const Uint32 Width  = RenderTargets.GetRenderWidth();
    const Uint32 Height = RenderTargets.GetRenderHeight();

    if (Width == 0 || Height == 0)
    {
        DEV_ERROR("RTXPT Temporal AA input conversion dimensions are invalid");
        return false;
    }

    if (m_TAADepth && m_TAAMotion && m_InputWidth == Width && m_InputHeight == Height)
        return true;

    m_TAADepth.Release();
    m_TAAMotion.Release();
    m_InputWidth  = 0;
    m_InputHeight = 0;

    TextureDesc DepthDesc;
    DepthDesc.Name      = "RTXPT Temporal AA depth";
    DepthDesc.Type      = RESOURCE_DIM_TEX_2D;
    DepthDesc.Width     = Width;
    DepthDesc.Height    = Height;
    DepthDesc.Format    = TEX_FORMAT_R32_FLOAT;
    DepthDesc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    pDevice->CreateTexture(DepthDesc, nullptr, &m_TAADepth);
    if (!GetTAADepthSRV() || !GetTAADepthUAV())
    {
        m_TAADepth.Release();
        DEV_ERROR("Failed to create RTXPT Temporal AA depth texture");
        return false;
    }

    TextureDesc MotionDesc;
    MotionDesc.Name      = "RTXPT Temporal AA motion vectors";
    MotionDesc.Type      = RESOURCE_DIM_TEX_2D;
    MotionDesc.Width     = Width;
    MotionDesc.Height    = Height;
    MotionDesc.Format    = TEX_FORMAT_RG16_FLOAT;
    MotionDesc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    pDevice->CreateTexture(MotionDesc, nullptr, &m_TAAMotion);
    if (!GetTAAMotionSRV() || !GetTAAMotionUAV())
    {
        m_TAAMotion.Release();
        DEV_ERROR("Failed to create RTXPT Temporal AA motion texture");
        return false;
    }

    m_InputWidth  = Width;
    m_InputHeight = Height;
    return true;
}

bool RTXPTTemporalAAPass::ConvertInputs(const RTXPTTemporalAAFrameAttribs& Attribs)
{
    if (!EnsureInputConversionResources(Attribs.pDevice, *Attribs.pRenderTargets))
        return false;

    {
        MapHelper<SampleConstants> Constants{Attribs.pDeviceContext, m_FrameConstantsBuffer, MAP_WRITE, MAP_FLAG_DISCARD};
        VERIFY(Constants, "Failed to map RTXPT Temporal AA frame constants");
        if (!Constants)
        {
            DEV_ERROR("Failed to update RTXPT Temporal AA frame constants");
            return false;
        }

        *Constants = *Attribs.pFrameConstants;
    }

    const bool Bound =
        SetSRBVariable(m_InputConversionSRB, "t_RTXPTDepth", Attribs.pRenderTargets->GetDepthSRV()) &&
        SetSRBVariable(m_InputConversionSRB, "t_RTXPTMotionVectors", Attribs.pRenderTargets->GetScreenMotionVectorsSRV()) &&
        SetSRBVariable(m_InputConversionSRB, "u_TAADepth", GetTAADepthUAV()) &&
        SetSRBVariable(m_InputConversionSRB, "u_TAAMotion", GetTAAMotionUAV());
    if (!Bound)
    {
        DEV_ERROR("RTXPT Temporal AA input conversion failed to bind resources");
        return false;
    }

    Attribs.pDeviceContext->SetPipelineState(m_InputConversionPSO);
    Attribs.pDeviceContext->CommitShaderResources(m_InputConversionSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Attribs.pDeviceContext->DispatchCompute(DispatchComputeAttribs{(m_InputWidth + 7u) / 8u, (m_InputHeight + 7u) / 8u, 1u});
    return true;
}

bool RTXPTTemporalAAPass::PreparePostFX(const RTXPTTemporalAAFrameAttribs& Attribs)
{
    PostFXContext::FrameDesc FrameDesc;
    FrameDesc.Index        = Attribs.FrameIndex;
    FrameDesc.Width        = Attribs.pRenderTargets->GetRenderWidth();
    FrameDesc.Height       = Attribs.pRenderTargets->GetRenderHeight();
    FrameDesc.OutputWidth  = Attribs.pRenderTargets->GetDisplayWidth();
    FrameDesc.OutputHeight = Attribs.pRenderTargets->GetDisplayHeight();

    m_PostFXContext->PrepareResources(Attribs.pDevice, FrameDesc, PostFXContext::FEATURE_FLAG_NONE);
    m_TemporalAA->PrepareResources(Attribs.pDevice,
                                   Attribs.pDeviceContext,
                                   m_PostFXContext.get(),
                                   TemporalAntiAliasing::FEATURE_FLAG_BICUBIC_FILTER);
    return true;
}

bool RTXPTTemporalAAPass::CopyOutputToProcessed(IRenderDevice*            pDevice,
                                                IDeviceContext*           pContext,
                                                const RTXPTRenderTargets& RenderTargets)
{
    m_Stats.LastCopyToProcessed = false;
    if (!m_PostFXContext)
    {
        DEV_ERROR("RTXPT Temporal AA copy requires initialized PostFXContext");
        return false;
    }
    if (pDevice == nullptr || pContext == nullptr)
    {
        DEV_ERROR("RTXPT Temporal AA copy requires a device and context");
        return false;
    }
    if (RenderTargets.GetOutputColorSRV() == nullptr || RenderTargets.GetProcessedOutputColorRTV() == nullptr)
    {
        DEV_ERROR("RTXPT Temporal AA copy requires OutputColor SRV and ProcessedOutputColor RTV");
        return false;
    }
    if (RenderTargets.GetRenderWidth() != RenderTargets.GetDisplayWidth() ||
        RenderTargets.GetRenderHeight() != RenderTargets.GetDisplayHeight())
    {
        DEV_ERROR("RTXPT Temporal AA copy requires render and display dimensions to match");
        return false;
    }

    PostFXContext::TextureOperationAttribs CopyAttribs;
    CopyAttribs.pDevice        = pDevice;
    CopyAttribs.pDeviceContext = pContext;
    m_PostFXContext->CopyTextureColor(CopyAttribs,
                                      RenderTargets.GetOutputColorSRV(),
                                      RenderTargets.GetProcessedOutputColorRTV());
    m_Stats.LastCopyToProcessed = true;
    return true;
}

bool RTXPTTemporalAAPass::CopyCurrentDepthToPrevious(IRenderDevice*            pDevice,
                                                     IDeviceContext*           pContext,
                                                     const RTXPTRenderTargets& RenderTargets)
{
    m_Stats.LastPreviousDepthCopy = false;
    if (GetTAADepthSRV() == nullptr || RenderTargets.GetPreviousDepthRTV() == nullptr)
    {
        DEV_ERROR("RTXPT Temporal AA previous-depth update requires Temporal AA depth SRV and PreviousDepth RTV");
        return false;
    }

    PostFXContext::TextureOperationAttribs CopyAttribs;
    CopyAttribs.pDevice        = pDevice;
    CopyAttribs.pDeviceContext = pContext;
    m_PostFXContext->CopyTextureDepth(CopyAttribs,
                                      GetTAADepthSRV(),
                                      RenderTargets.GetPreviousDepthRTV());
    m_Stats.LastPreviousDepthCopy = true;
    return true;
}

bool RTXPTTemporalAAPass::Execute(const RTXPTTemporalAAFrameAttribs& Attribs)
{
    m_Stats.LastExecute           = false;
    m_Stats.LastCopyToProcessed   = false;
    m_Stats.LastPreviousDepthCopy = false;

    if (!m_Stats.Ready || !m_PostFXContext || !m_TemporalAA)
    {
        DEV_ERROR("RTXPT Temporal AA pass is not initialized");
        return false;
    }

    if (!ValidateTemporalInputs(Attribs))
        return false;

    PreparePostFX(Attribs);
    if (!ConvertInputs(Attribs))
        return false;

    const SampleConstants&    Constants          = *Attribs.pFrameConstants;
    const bool                ResetAccumulation  = Attribs.ResetHistory || !Attribs.PreviousViewValid;
    const bool                UsePrevious        = !ResetAccumulation;
    const Uint32              PreviousFrameIndex = Attribs.FrameIndex > 0 ? Attribs.FrameIndex - 1u : 0u;
    const HLSL::CameraAttribs CurrentCamera =
        MakeCameraAttribs(Constants, Constants.view, Constants.ptConsts.camera, Attribs.FrameIndex);
    const HLSL::CameraAttribs PreviousCamera =
        UsePrevious ?
        MakeCameraAttribs(Constants, Constants.previousView, Constants.ptConsts.prevCamera, PreviousFrameIndex) :
        CurrentCamera;

    PostFXContext::RenderAttributes PostFXAttribs;
    PostFXAttribs.pDevice             = Attribs.pDevice;
    PostFXAttribs.pDeviceContext      = Attribs.pDeviceContext;
    PostFXAttribs.pCurrDepthBufferSRV = GetTAADepthSRV();
    PostFXAttribs.pPrevDepthBufferSRV = UsePrevious ?
        Attribs.pRenderTargets->GetPreviousDepthSRV() :
        GetTAADepthSRV();
    PostFXAttribs.pMotionVectorsSRV = GetTAAMotionSRV();
    PostFXAttribs.pCurrCamera       = &CurrentCamera;
    PostFXAttribs.pPrevCamera       = &PreviousCamera;
    m_PostFXContext->Execute(PostFXAttribs);
    if (!m_PostFXContext->IsPSOsReady())
    {
        DEV_ERROR("RTXPT Temporal AA PostFX resources are not ready");
        return false;
    }

    HLSL::TemporalAntiAliasingAttribs TAAAttribs = {};
    TAAAttribs.TemporalStabilityFactor           = std::clamp(Attribs.Settings.TemporalStabilityFactor, 0.0f, 1.0f);
    TAAAttribs.ResetAccumulation                 = ResetAccumulation ? TRUE : FALSE;
    TAAAttribs.SkipRejection                     = Attribs.Settings.SkipRejection ? TRUE : FALSE;

    TemporalAntiAliasing::RenderAttributes TAARenderAttribs;
    TAARenderAttribs.pDevice         = Attribs.pDevice;
    TAARenderAttribs.pDeviceContext  = Attribs.pDeviceContext;
    TAARenderAttribs.pPostFXContext  = m_PostFXContext.get();
    TAARenderAttribs.pColorBufferSRV = Attribs.pRenderTargets->GetOutputColorSRV();
    TAARenderAttribs.pTAAAttribs     = &TAAAttribs;
    m_TemporalAA->Execute(TAARenderAttribs);

    ITextureView* pTaaOutputSRV = m_TemporalAA->GetAccumulatedFrameSRV(false);
    if (pTaaOutputSRV == nullptr)
    {
        DEV_ERROR("RTXPT Temporal AA accumulated output SRV is null");
        return false;
    }

    if (!CopyCurrentDepthToPrevious(Attribs.pDevice, Attribs.pDeviceContext, *Attribs.pRenderTargets))
        return false;

    PostFXContext::TextureOperationAttribs CopyAttribs;
    CopyAttribs.pDevice        = Attribs.pDevice;
    CopyAttribs.pDeviceContext = Attribs.pDeviceContext;
    m_PostFXContext->CopyTextureColor(CopyAttribs,
                                      pTaaOutputSRV,
                                      Attribs.pRenderTargets->GetProcessedOutputColorRTV());
    m_Stats.LastCopyToProcessed = true;

    m_Stats.LastExecute = true;
    ++m_Stats.ExecuteCount;
    return true;
}

} // namespace Diligent
