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

#pragma once

#include "BasicMath.hpp"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "GraphicsTypes.h"

namespace Diligent
{

struct IRenderStateCache;

struct RTXPTBloomParameters
{
    bool  Enabled   = true;
    float Radius    = 8.0f;
    float Intensity = 0.004f;
};

struct RTXPTBloomRenderAttribs
{
    ITextureView*        pSourceSRV = nullptr;
    ITextureView*        pTargetRTV = nullptr;
    Uint32               Width      = 0;
    Uint32               Height     = 0;
    TEXTURE_FORMAT       Format     = TEX_FORMAT_UNKNOWN;
    RTXPTBloomParameters Params;
};

struct RTXPTBloomPassStats
{
    bool   Ready              = false;
    bool   LastRenderExecuted = false;
    Uint32 RenderCount        = 0;
    Uint32 DownscaleWidth     = 0;
    Uint32 DownscaleHeight    = 0;
    Uint32 BlurWidth          = 0;
    Uint32 BlurHeight         = 0;
};

class RTXPTBloomPass
{
public:
    void Reset();
    bool Initialize(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache);
    bool ResizeResources(IRenderDevice* pDevice, Uint32 Width, Uint32 Height, TEXTURE_FORMAT Format);
    bool Render(IDeviceContext* pContext, const RTXPTBloomRenderAttribs& Attribs);

    bool                       IsReady() const { return m_Stats.Ready; }
    const RTXPTBloomPassStats& GetStats() const { return m_Stats; }

private:
    struct IntermediateTextures
    {
        Uint32                  Downscale1Width  = 0;
        Uint32                  Downscale1Height = 0;
        Uint32                  Downscale2Width  = 0;
        Uint32                  Downscale2Height = 0;
        RefCntAutoPtr<ITexture> Downscale1;
        RefCntAutoPtr<ITexture> Downscale2;
        RefCntAutoPtr<ITexture> Blur1;
        RefCntAutoPtr<ITexture> Blur2;
    };

    struct CopyAttribs
    {
        ITextureView* pSourceSRV   = nullptr;
        ITextureView* pTargetRTV   = nullptr;
        Uint32        Width        = 0;
        Uint32        Height       = 0;
        bool          BlendEnabled = false;
        float         BlendFactor  = 1.0f;
    };

    bool CreateSampler(IRenderDevice* pDevice);
    bool CreateShaders(IRenderStateCache* pStateCache, IEngineFactory* pEngineFactory);
    bool CreatePipelines(IRenderDevice* pDevice, TEXTURE_FORMAT Format);
    bool CreateIntermediateTexture(IRenderDevice* pDevice, const char* Name, Uint32 Width, Uint32 Height, TEXTURE_FORMAT Format, RefCntAutoPtr<ITexture>& Texture);
    // Creates the textures of Textures at its sizes.
    bool CreateIntermediateTextures(IRenderDevice* pDevice, TEXTURE_FORMAT Format, IntermediateTextures& Textures);
    bool DrawFullscreen(IDeviceContext* pContext, IPipelineState* pPSO, IShaderResourceBinding* pSRB, ITextureView* pRTV, Uint32 Width, Uint32 Height);
    bool DrawCopy(IDeviceContext* pContext, const CopyAttribs& Attribs);
    bool DrawBlur(IDeviceContext* pContext, IShaderResourceBinding* pSRB, ITextureView* pSourceSRV, ITextureView* pTargetRTV, Uint32 Width, Uint32 Height);
    bool UpdateBlurConstants(IDeviceContext* pContext, IBuffer* pBuffer, const float2& PixStep, float EffectiveSigma);

private:
    RTXPTBloomPassStats                   m_Stats;
    IRenderStateCache*                    m_pStateCache = nullptr; // Owned by RTXPTSample; set in Initialize.
    RefCntAutoPtr<IShader>                m_FullscreenVS;
    RefCntAutoPtr<IShader>                m_CopyPS;
    RefCntAutoPtr<IShader>                m_BlurPS;
    RefCntAutoPtr<IPipelineState>         m_CopyPSO;
    RefCntAutoPtr<IPipelineState>         m_ApplyPSO;
    RefCntAutoPtr<IPipelineState>         m_BlurPSO;
    RefCntAutoPtr<IShaderResourceBinding> m_CopySRB;
    RefCntAutoPtr<IShaderResourceBinding> m_ApplySRB;
    RefCntAutoPtr<IShaderResourceBinding> m_HBlurSRB;
    RefCntAutoPtr<IShaderResourceBinding> m_VBlurSRB;
    RefCntAutoPtr<IBuffer>                m_HBlurCB;
    RefCntAutoPtr<IBuffer>                m_VBlurCB;
    RefCntAutoPtr<ISampler>               m_LinearSampler;
    RefCntAutoPtr<ITexture>               m_Downscale1;
    RefCntAutoPtr<ITexture>               m_Downscale2;
    RefCntAutoPtr<ITexture>               m_Blur1;
    RefCntAutoPtr<ITexture>               m_Blur2;
    TEXTURE_FORMAT                        m_Format = TEX_FORMAT_UNKNOWN;
    Uint32                                m_Width  = 0;
    Uint32                                m_Height = 0;
};

} // namespace Diligent
