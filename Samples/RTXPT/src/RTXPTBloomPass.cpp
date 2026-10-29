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

#include "RTXPTBloomPass.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "DebugUtilities.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"

namespace Diligent
{

namespace
{

struct RTXPTBloomConstants
{
    float2 PixStep            = float2{0.0f, 0.0f};
    float  ArgumentScale      = 0.0f;
    float  NormalizationScale = 0.0f;
    float3 Padding            = float3{0.0f, 0.0f, 0.0f};
    float  NumSamples         = 0.0f;
};
static_assert(sizeof(RTXPTBloomConstants) == 32, "RTXPTBloomConstants must match RTXPTBloomShared.h");

bool SetStaticVariable(IPipelineState* pPSO, SHADER_TYPE ShaderType, const char* Name, IDeviceObject* pObject, bool Required)
{
    IShaderResourceVariable* pVar = pPSO != nullptr ? pPSO->GetStaticVariableByName(ShaderType, Name) : nullptr;
    if (pVar == nullptr)
        return !Required;
    if (pObject == nullptr)
        return !Required;

    pVar->Set(pObject, SET_SHADER_RESOURCE_FLAG_ALLOW_OVERWRITE);
    return true;
}

bool SetSRBVariable(IShaderResourceBinding* pSRB, SHADER_TYPE ShaderType, const char* Name, IDeviceObject* pObject, bool Required)
{
    IShaderResourceVariable* pVar = pSRB != nullptr ? pSRB->GetVariableByName(ShaderType, Name) : nullptr;
    if (pVar == nullptr)
        return !Required;
    if (pObject == nullptr)
        return !Required;

    pVar->Set(pObject);
    return true;
}

bool CreateSRB(IPipelineState* pPSO, RefCntAutoPtr<IShaderResourceBinding>& SRB)
{
    SRB.Release();
    if (pPSO == nullptr)
        return false;

    pPSO->CreateShaderResourceBinding(&SRB, true);
    return SRB != nullptr;
}

bool BindStaticSamplers(IPipelineState* pCopyPSO, IPipelineState* pApplyPSO, IPipelineState* pBlurPSO, ISampler* pSampler)
{
    return SetStaticVariable(pCopyPSO, SHADER_TYPE_PIXEL, "s_LinearSampler", pSampler, true) &&
        SetStaticVariable(pApplyPSO, SHADER_TYPE_PIXEL, "s_LinearSampler", pSampler, true) &&
        SetStaticVariable(pBlurPSO, SHADER_TYPE_PIXEL, "s_LinearSampler", pSampler, true);
}

bool CreateBloomSRBs(IPipelineState*                        pCopyPSO,
                     IPipelineState*                        pApplyPSO,
                     IPipelineState*                        pBlurPSO,
                     RefCntAutoPtr<IShaderResourceBinding>& CopySRB,
                     RefCntAutoPtr<IShaderResourceBinding>& ApplySRB,
                     RefCntAutoPtr<IShaderResourceBinding>& HBlurSRB,
                     RefCntAutoPtr<IShaderResourceBinding>& VBlurSRB)
{
    return CreateSRB(pCopyPSO, CopySRB) &&
        CreateSRB(pApplyPSO, ApplySRB) &&
        CreateSRB(pBlurPSO, HBlurSRB) &&
        CreateSRB(pBlurPSO, VBlurSRB);
}

bool CreateShader(IRenderStateCache*      pStateCache,
                  const ShaderCreateInfo& BaseCI,
                  SHADER_TYPE             ShaderType,
                  const char*             Name,
                  const char*             FilePath,
                  const char*             EntryPoint,
                  RefCntAutoPtr<IShader>& Shader)
{
    ShaderCreateInfo ShaderCI = BaseCI;
    ShaderCI.Desc.ShaderType  = ShaderType;
    ShaderCI.Desc.Name        = Name;
    ShaderCI.FilePath         = FilePath;
    ShaderCI.EntryPoint       = EntryPoint;

    Shader.Release();
    pStateCache->CreateShader(ShaderCI, &Shader);
    VERIFY(Shader, "Failed to create shader: ", Name);
    return Shader != nullptr;
}

void InitFullscreenPipeline(GraphicsPipelineStateCreateInfo& PSOCreateInfo,
                            const char*                      Name,
                            TEXTURE_FORMAT                   Format,
                            IShader*                         pVS,
                            IShader*                         pPS)
{
    PSOCreateInfo.PSODesc.Name                                  = Name;
    PSOCreateInfo.PSODesc.PipelineType                          = PIPELINE_TYPE_GRAPHICS;
    PSOCreateInfo.GraphicsPipeline.NumRenderTargets             = 1;
    PSOCreateInfo.GraphicsPipeline.RTVFormats[0]                = Format;
    PSOCreateInfo.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    PSOCreateInfo.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    PSOCreateInfo.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    PSOCreateInfo.pVS                                           = pVS;
    PSOCreateInfo.pPS                                           = pPS;
}

bool CreateCopyPipeline(IRenderStateCache*             pStateCache,
                        const char*                    Name,
                        TEXTURE_FORMAT                 Format,
                        IShader*                       pVS,
                        IShader*                       pPS,
                        bool                           BlendEnabled,
                        RefCntAutoPtr<IPipelineState>& PSO)
{
    GraphicsPipelineStateCreateInfo PSOCreateInfo;
    InitFullscreenPipeline(PSOCreateInfo, Name, Format, pVS, pPS);
    if (BlendEnabled)
    {
        auto& RT0Blend          = PSOCreateInfo.GraphicsPipeline.BlendDesc.RenderTargets[0];
        RT0Blend.BlendEnable    = True;
        RT0Blend.SrcBlend       = BLEND_FACTOR_BLEND_FACTOR;
        RT0Blend.DestBlend      = BLEND_FACTOR_INV_BLEND_FACTOR;
        RT0Blend.SrcBlendAlpha  = BLEND_FACTOR_ZERO;
        RT0Blend.DestBlendAlpha = BLEND_FACTOR_ONE;
    }

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout
        .AddVariable(SHADER_TYPE_PIXEL, "s_LinearSampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_PIXEL, "t_Source", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    pStateCache->CreateGraphicsPipelineState(PSOCreateInfo, &PSO);
    return PSO != nullptr;
}

bool CreateBlurPipeline(IRenderStateCache*             pStateCache,
                        TEXTURE_FORMAT                 Format,
                        IShader*                       pVS,
                        IShader*                       pPS,
                        RefCntAutoPtr<IPipelineState>& PSO)
{
    GraphicsPipelineStateCreateInfo PSOCreateInfo;
    InitFullscreenPipeline(PSOCreateInfo, "RTXPT bloom blur PSO", Format, pVS, pPS);

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ResourceLayout
        .AddVariable(SHADER_TYPE_PIXEL, "g_BloomConstants", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE)
        .AddVariable(SHADER_TYPE_PIXEL, "s_LinearSampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_PIXEL, "t_Source", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    pStateCache->CreateGraphicsPipelineState(PSOCreateInfo, &PSO);
    return PSO != nullptr;
}

struct BloomTextureViews
{
    ITextureView* pDownscale1SRV = nullptr;
    ITextureView* pDownscale1RTV = nullptr;
    ITextureView* pDownscale2SRV = nullptr;
    ITextureView* pDownscale2RTV = nullptr;
    ITextureView* pBlur1SRV      = nullptr;
    ITextureView* pBlur1RTV      = nullptr;
    ITextureView* pBlur2SRV      = nullptr;
    ITextureView* pBlur2RTV      = nullptr;
};

bool GetBloomTextureViews(ITexture* pDownscale1, ITexture* pDownscale2, ITexture* pBlur1, ITexture* pBlur2, BloomTextureViews& Views)
{
    if (pDownscale1 == nullptr || pDownscale2 == nullptr || pBlur1 == nullptr || pBlur2 == nullptr)
        return false;

    Views.pDownscale1SRV = pDownscale1->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    Views.pDownscale1RTV = pDownscale1->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    Views.pDownscale2SRV = pDownscale2->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    Views.pDownscale2RTV = pDownscale2->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    Views.pBlur1SRV      = pBlur1->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    Views.pBlur1RTV      = pBlur1->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    Views.pBlur2SRV      = pBlur2->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    Views.pBlur2RTV      = pBlur2->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);

    return Views.pDownscale1SRV != nullptr && Views.pDownscale1RTV != nullptr &&
        Views.pDownscale2SRV != nullptr && Views.pDownscale2RTV != nullptr &&
        Views.pBlur1SRV != nullptr && Views.pBlur1RTV != nullptr &&
        Views.pBlur2SRV != nullptr && Views.pBlur2RTV != nullptr;
}

} // namespace

void RTXPTBloomPass::Reset()
{
    m_CopyPSO.Release();
    m_ApplyPSO.Release();
    m_BlurPSO.Release();
    m_CopySRB.Release();
    m_ApplySRB.Release();
    m_HBlurSRB.Release();
    m_VBlurSRB.Release();
    m_HBlurCB.Release();
    m_VBlurCB.Release();
    m_LinearSampler.Release();
    m_Downscale1.Release();
    m_Downscale2.Release();
    m_Blur1.Release();
    m_Blur2.Release();
    m_FullscreenVS.Release();
    m_CopyPS.Release();
    m_BlurPS.Release();
    m_Stats  = {};
    m_Format = TEX_FORMAT_UNKNOWN;
    m_Width  = 0;
    m_Height = 0;
}

bool RTXPTBloomPass::CreateSampler(IRenderDevice* pDevice)
{
    if (pDevice == nullptr)
        return false;

    SamplerDesc LinearSampler;
    LinearSampler.Name      = "RTXPT bloom linear sampler";
    LinearSampler.MinFilter = FILTER_TYPE_LINEAR;
    LinearSampler.MagFilter = FILTER_TYPE_LINEAR;
    LinearSampler.MipFilter = FILTER_TYPE_LINEAR;
    LinearSampler.AddressU  = TEXTURE_ADDRESS_CLAMP;
    LinearSampler.AddressV  = TEXTURE_ADDRESS_CLAMP;
    LinearSampler.AddressW  = TEXTURE_ADDRESS_CLAMP;
    pDevice->CreateSampler(LinearSampler, &m_LinearSampler);

    VERIFY(m_LinearSampler, "Failed to create RTXPT bloom sampler");
    return m_LinearSampler != nullptr;
}

bool RTXPTBloomPass::CreateShaders(IRenderStateCache* pStateCache, IEngineFactory* pEngineFactory)
{
    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    pEngineFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders\\PostProcessing", &pShaderSourceFactory);

    ShaderCreateInfo ShaderCI;
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    return CreateShader(pStateCache, ShaderCI, SHADER_TYPE_VERTEX, "RTXPT bloom VS", "PostProcessing/RTXPTFullscreen.vsh", "main", m_FullscreenVS) &&
        CreateShader(pStateCache, ShaderCI, SHADER_TYPE_PIXEL, "RTXPT bloom copy PS", "PostProcessing/RTXPTBloomCopy.psh", "main", m_CopyPS) &&
        CreateShader(pStateCache, ShaderCI, SHADER_TYPE_PIXEL, "RTXPT bloom blur PS", "PostProcessing/RTXPTBloomBlur.psh", "main", m_BlurPS);
}

bool RTXPTBloomPass::Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache)
{
    Reset();

    if (pDevice == nullptr || pEngineFactory == nullptr)
    {
        DEV_ERROR("RTXPT bloom pass requires a render device and engine factory");
        return false;
    }

    m_pStateCache = pStateCache;

    if (!CreateSampler(pDevice) || !CreateShaders(pStateCache, pEngineFactory))
    {
        DEV_ERROR("RTXPT bloom pass failed to create shaders or sampler");
        return false;
    }

    BufferDesc ConstantsDesc;
    ConstantsDesc.Name           = "RTXPT bloom horizontal constants";
    ConstantsDesc.Size           = sizeof(RTXPTBloomConstants);
    ConstantsDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage          = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_HBlurCB);

    ConstantsDesc.Name = "RTXPT bloom vertical constants";
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_VBlurCB);
    if (!m_HBlurCB || !m_VBlurCB)
    {
        DEV_ERROR("RTXPT bloom pass failed to create blur constants");
        return false;
    }

    m_Stats.Ready = true;
    return true;
}

bool RTXPTBloomPass::CreatePipelines(IRenderDevice* pDevice, TEXTURE_FORMAT Format)
{
    if (pDevice == nullptr || m_pStateCache == nullptr || Format == TEX_FORMAT_UNKNOWN || !m_FullscreenVS || !m_CopyPS || !m_BlurPS || !m_LinearSampler || !m_HBlurCB || !m_VBlurCB)
        return false;

    RefCntAutoPtr<IPipelineState>         CopyPSO;
    RefCntAutoPtr<IPipelineState>         ApplyPSO;
    RefCntAutoPtr<IPipelineState>         BlurPSO;
    RefCntAutoPtr<IShaderResourceBinding> CopySRB;
    RefCntAutoPtr<IShaderResourceBinding> ApplySRB;
    RefCntAutoPtr<IShaderResourceBinding> HBlurSRB;
    RefCntAutoPtr<IShaderResourceBinding> VBlurSRB;

    const bool PSOsReady =
        CreateCopyPipeline(m_pStateCache, "RTXPT bloom copy PSO", Format, m_FullscreenVS, m_CopyPS, false, CopyPSO) &&
        CreateCopyPipeline(m_pStateCache, "RTXPT bloom apply PSO", Format, m_FullscreenVS, m_CopyPS, true, ApplyPSO) &&
        CreateBlurPipeline(m_pStateCache, Format, m_FullscreenVS, m_BlurPS, BlurPSO);
    if (!PSOsReady)
        return false;

    if (!BindStaticSamplers(CopyPSO, ApplyPSO, BlurPSO, m_LinearSampler))
        return false;

    if (!CreateBloomSRBs(CopyPSO, ApplyPSO, BlurPSO, CopySRB, ApplySRB, HBlurSRB, VBlurSRB))
        return false;

    const bool BlurConstantsBound =
        SetSRBVariable(HBlurSRB, SHADER_TYPE_PIXEL, "g_BloomConstants", m_HBlurCB, true) &&
        SetSRBVariable(VBlurSRB, SHADER_TYPE_PIXEL, "g_BloomConstants", m_VBlurCB, true);
    VERIFY(BlurConstantsBound, "Failed to bind RTXPT bloom blur constants");
    if (!BlurConstantsBound)
        return false;

    m_CopyPSO  = std::move(CopyPSO);
    m_ApplyPSO = std::move(ApplyPSO);
    m_BlurPSO  = std::move(BlurPSO);
    m_CopySRB  = std::move(CopySRB);
    m_ApplySRB = std::move(ApplySRB);
    m_HBlurSRB = std::move(HBlurSRB);
    m_VBlurSRB = std::move(VBlurSRB);
    return true;
}

bool RTXPTBloomPass::CreateIntermediateTexture(IRenderDevice*           pDevice,
                                               const char*              Name,
                                               Uint32                   Width,
                                               Uint32                   Height,
                                               TEXTURE_FORMAT           Format,
                                               RefCntAutoPtr<ITexture>& Texture)
{
    Texture.Release();
    if (pDevice == nullptr || Width == 0 || Height == 0 || Format == TEX_FORMAT_UNKNOWN)
        return false;

    TextureDesc Desc;
    Desc.Name      = Name;
    Desc.Type      = RESOURCE_DIM_TEX_2D;
    Desc.Width     = Width;
    Desc.Height    = Height;
    Desc.Format    = Format;
    Desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    Desc.Usage     = USAGE_DEFAULT;
    pDevice->CreateTexture(Desc, nullptr, &Texture);

    return Texture != nullptr &&
        Texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) != nullptr &&
        Texture->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET) != nullptr;
}

bool RTXPTBloomPass::CreateIntermediateTextures(IRenderDevice* pDevice, TEXTURE_FORMAT Format, IntermediateTextures& Textures)
{
    return CreateIntermediateTexture(pDevice, "RTXPT bloom downscale 1", Textures.Downscale1Width, Textures.Downscale1Height, Format, Textures.Downscale1) &&
        CreateIntermediateTexture(pDevice, "RTXPT bloom downscale 2", Textures.Downscale2Width, Textures.Downscale2Height, Format, Textures.Downscale2) &&
        CreateIntermediateTexture(pDevice, "RTXPT bloom blur 1", Textures.Downscale2Width, Textures.Downscale2Height, Format, Textures.Blur1) &&
        CreateIntermediateTexture(pDevice, "RTXPT bloom blur 2", Textures.Downscale2Width, Textures.Downscale2Height, Format, Textures.Blur2);
}

bool RTXPTBloomPass::ResizeResources(IRenderDevice* pDevice, Uint32 Width, Uint32 Height, TEXTURE_FORMAT Format)
{
    if (!IsReady() || pDevice == nullptr || Width == 0 || Height == 0 || Format == TEX_FORMAT_UNKNOWN)
        return false;

    IntermediateTextures Textures;
    Textures.Downscale1Width  = std::max(1u, (Width + 1u) / 2u);
    Textures.Downscale1Height = std::max(1u, (Height + 1u) / 2u);
    Textures.Downscale2Width  = std::max(1u, (Textures.Downscale1Width + 1u) / 2u);
    Textures.Downscale2Height = std::max(1u, (Textures.Downscale1Height + 1u) / 2u);

    const bool FormatChanged    = Format != m_Format;
    const bool RecreateTextures = FormatChanged || m_Width != Width || m_Height != Height ||
        !m_Downscale1 || !m_Downscale2 || !m_Blur1 || !m_Blur2;

    if (RecreateTextures && !CreateIntermediateTextures(pDevice, Format, Textures))
    {
        DEV_ERROR("RTXPT bloom pass failed to create intermediate textures");
        return false;
    }

    if ((FormatChanged || !m_CopyPSO || !m_ApplyPSO || !m_BlurPSO) &&
        !CreatePipelines(pDevice, Format))
    {
        DEV_ERROR("RTXPT bloom pass failed to create pipelines");
        return false;
    }

    if (!RecreateTextures)
        return true;

    m_Downscale1            = std::move(Textures.Downscale1);
    m_Downscale2            = std::move(Textures.Downscale2);
    m_Blur1                 = std::move(Textures.Blur1);
    m_Blur2                 = std::move(Textures.Blur2);
    m_Format                = Format;
    m_Width                 = Width;
    m_Height                = Height;
    m_Stats.DownscaleWidth  = Textures.Downscale1Width;
    m_Stats.DownscaleHeight = Textures.Downscale1Height;
    m_Stats.BlurWidth       = Textures.Downscale2Width;
    m_Stats.BlurHeight      = Textures.Downscale2Height;
    return true;
}

bool RTXPTBloomPass::DrawFullscreen(IDeviceContext*         pContext,
                                    IPipelineState*         pPSO,
                                    IShaderResourceBinding* pSRB,
                                    ITextureView*           pRTV,
                                    Uint32                  Width,
                                    Uint32                  Height)
{
    if (pContext == nullptr || pPSO == nullptr || pSRB == nullptr || pRTV == nullptr || Width == 0 || Height == 0)
        return false;

    ITextureView* pRenderTarget = pRTV;
    pContext->SetRenderTargets(1, &pRenderTarget, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->SetViewports(1, nullptr, Width, Height);
    pContext->SetPipelineState(pPSO);
    pContext->CommitShaderResources(pSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->Draw(DrawAttribs{3, DRAW_FLAG_VERIFY_ALL});
    return true;
}

bool RTXPTBloomPass::DrawCopy(IDeviceContext* pContext, const CopyAttribs& Attribs)
{
    IShaderResourceBinding* pSRB = Attribs.BlendEnabled ? m_ApplySRB.RawPtr() : m_CopySRB.RawPtr();
    IPipelineState*         pPSO = Attribs.BlendEnabled ? m_ApplyPSO.RawPtr() : m_CopyPSO.RawPtr();
    if (!SetSRBVariable(pSRB, SHADER_TYPE_PIXEL, "t_Source", Attribs.pSourceSRV, true))
        return false;

    if (Attribs.BlendEnabled)
    {
        const float BlendFactors[4] = {Attribs.BlendFactor, Attribs.BlendFactor, Attribs.BlendFactor, Attribs.BlendFactor};
        pContext->SetBlendFactors(BlendFactors);
    }

    return DrawFullscreen(pContext, pPSO, pSRB, Attribs.pTargetRTV, Attribs.Width, Attribs.Height);
}

bool RTXPTBloomPass::DrawBlur(IDeviceContext*         pContext,
                              IShaderResourceBinding* pSRB,
                              ITextureView*           pSourceSRV,
                              ITextureView*           pTargetRTV,
                              Uint32                  Width,
                              Uint32                  Height)
{
    if (!SetSRBVariable(pSRB, SHADER_TYPE_PIXEL, "t_Source", pSourceSRV, true))
        return false;

    return DrawFullscreen(pContext, m_BlurPSO, pSRB, pTargetRTV, Width, Height);
}

bool RTXPTBloomPass::UpdateBlurConstants(IDeviceContext* pContext, IBuffer* pBuffer, const float2& PixStep, float EffectiveSigma)
{
    if (pContext == nullptr || pBuffer == nullptr || EffectiveSigma <= 0.0f)
        return false;

    RTXPTBloomConstants Constants;
    Constants.PixStep            = PixStep;
    Constants.ArgumentScale      = -1.0f / (2.0f * EffectiveSigma * EffectiveSigma);
    Constants.NormalizationScale = 1.0f / (std::sqrt(2.0f * PI_F) * EffectiveSigma);
    Constants.NumSamples         = std::round(EffectiveSigma * 4.0f);

    MapHelper<RTXPTBloomConstants> Mapped{pContext, pBuffer, MAP_WRITE, MAP_FLAG_DISCARD};
    if (!Mapped)
        return false;

    *Mapped = Constants;
    return true;
}

bool RTXPTBloomPass::Render(IDeviceContext* pContext, const RTXPTBloomRenderAttribs& Attribs)
{
    m_Stats.LastRenderExecuted = false;

    const bool Enabled =
        Attribs.Params.Enabled &&
        Attribs.Params.Intensity > 0.0f &&
        Attribs.Params.Radius > 0.0f;
    if (!Enabled)
        return true;

    const float EffectiveSigma = std::clamp(Attribs.Params.Radius * 0.25f, 1.0f, 100.0f);

    if (!IsReady() || pContext == nullptr || Attribs.pSourceSRV == nullptr || Attribs.pTargetRTV == nullptr ||
        Attribs.Width == 0 || Attribs.Height == 0 || Attribs.Format != m_Format ||
        Attribs.Width != m_Width || Attribs.Height != m_Height ||
        !m_Downscale1 || !m_Downscale2 || !m_Blur1 || !m_Blur2)
        return false;

    BloomTextureViews Views;
    if (!GetBloomTextureViews(m_Downscale1, m_Downscale2, m_Blur1, m_Blur2, Views))
        return false;

    CopyAttribs Downscale1Copy;
    Downscale1Copy.pSourceSRV = Attribs.pSourceSRV;
    Downscale1Copy.pTargetRTV = Views.pDownscale1RTV;
    Downscale1Copy.Width      = m_Stats.DownscaleWidth;
    Downscale1Copy.Height     = m_Stats.DownscaleHeight;

    CopyAttribs Downscale2Copy;
    Downscale2Copy.pSourceSRV = Views.pDownscale1SRV;
    Downscale2Copy.pTargetRTV = Views.pDownscale2RTV;
    Downscale2Copy.Width      = m_Stats.BlurWidth;
    Downscale2Copy.Height     = m_Stats.BlurHeight;

    CopyAttribs ApplyCopy;
    ApplyCopy.pSourceSRV   = Views.pBlur2SRV;
    ApplyCopy.pTargetRTV   = Attribs.pTargetRTV;
    ApplyCopy.Width        = Attribs.Width;
    ApplyCopy.Height       = Attribs.Height;
    ApplyCopy.BlendEnabled = true;
    ApplyCopy.BlendFactor  = Attribs.Params.Intensity;

    const bool Rendered =
        DrawCopy(pContext, Downscale1Copy) &&
        DrawCopy(pContext, Downscale2Copy) &&
        UpdateBlurConstants(pContext, m_HBlurCB, float2{1.0f / static_cast<float>(m_Stats.BlurWidth), 0.0f}, EffectiveSigma) &&
        UpdateBlurConstants(pContext, m_VBlurCB, float2{0.0f, 1.0f / static_cast<float>(m_Stats.BlurHeight)}, EffectiveSigma) &&
        DrawBlur(pContext, m_HBlurSRB, Views.pDownscale2SRV, Views.pBlur1RTV, m_Stats.BlurWidth, m_Stats.BlurHeight) &&
        DrawBlur(pContext, m_VBlurSRB, Views.pBlur1SRV, Views.pBlur2RTV, m_Stats.BlurWidth, m_Stats.BlurHeight) &&
        DrawCopy(pContext, ApplyCopy);
    if (!Rendered)
        return false;

    m_Stats.LastRenderExecuted = true;
    ++m_Stats.RenderCount;
    return true;
}

} // namespace Diligent
