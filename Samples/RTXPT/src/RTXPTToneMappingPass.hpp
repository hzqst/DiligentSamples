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

#include "Buffer.h"
#include "BufferView.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Sampler.h"
#include "Shader.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"
#include "TextureView.h"

namespace Diligent
{

struct IRenderStateCache;
struct IPipelineStateCache;

enum class RTXPTExposureMode : Uint32
{
    AperturePriority = 0,
    ShutterPriority  = 1,
};

enum class RTXPTToneMapperOperator : Uint32
{
    Linear           = 0,
    Reinhard         = 1,
    ReinhardModified = 2,
    HejiHableAlu     = 3,
    HableUc2         = 4,
    Aces             = 5,
};

struct RTXPTToneMappingParameters
{
    RTXPTExposureMode       ExposureMode         = RTXPTExposureMode::AperturePriority;
    RTXPTToneMapperOperator ToneMapOperator      = RTXPTToneMapperOperator::HableUc2;
    bool                    AutoExposure         = false;
    float                   ExposureCompensation = 0.0f;
    float                   ExposureValue        = 0.0f;
    float                   FilmSpeed            = 100.0f;
    float                   FNumber              = 1.0f;
    float                   Shutter              = 1.0f;
    bool                    WhiteBalance         = false;
    float                   WhitePoint           = 6500.0f;
    float                   WhiteMaxLuminance    = 1.0f;
    float                   WhiteScale           = 5.1f;
    bool                    Clamped              = true;
    float                   ExposureValueMin     = -16.0f;
    float                   ExposureValueMax     = 16.0f;
};

struct RTXPTToneMappingPassStats
{
    bool   Ready              = false;
    bool   AutoExposureReady  = false;
    bool   LastRenderExecuted = false;
    Uint32 RenderCount        = 0;
    float  LastAvgLuminance   = 1.0f;
};

struct RTXPTToneMappingRenderAttribs
{
    ITextureView*                     pSourceSRV = nullptr;
    ITextureView*                     pLdrRTV    = nullptr;
    Uint32                            Width      = 0;
    Uint32                            Height     = 0;
    bool                              Enabled    = true;
    const RTXPTToneMappingParameters* pParams    = nullptr;
};

class RTXPTToneMappingPass
{
public:
    void Reset();
    bool Initialize(IRenderDevice*       pDevice,
                    IEngineFactory*      pEngineFactory,
                    IRenderStateCache*   pStateCache,
                    IPipelineStateCache* pPSOCache,
                    TEXTURE_FORMAT       LdrFormat,
                    bool                 ComputeSupported);
    bool ResizeResources(IRenderDevice* pDevice, Uint32 Width, Uint32 Height, TEXTURE_FORMAT SourceFormat);
    bool Render(IDeviceContext* pContext, const RTXPTToneMappingRenderAttribs& Attribs);

    float ComputePreExposedGrayLuminance(const RTXPTToneMappingParameters& Params, bool Enabled) const;

    bool                             IsReady() const { return m_Stats.Ready; }
    const RTXPTToneMappingPassStats& GetStats() const { return m_Stats; }

private:
    bool CreateSamplers(IRenderDevice* pDevice);
    bool CreateLuminanceResources(IRenderDevice* pDevice, Uint32 Width, Uint32 Height, TEXTURE_FORMAT SourceFormat);
    bool UpdateToneMappingConstants(IDeviceContext* pContext, const RTXPTToneMappingParameters& Params, bool Enabled);

    IRenderStateCache*                    m_pStateCache = nullptr; // Owned by RTXPTSample; set in Initialize.
    IPipelineStateCache*                  m_pPSOCache   = nullptr; // Owned by the caller of Initialize; may be null.
    RefCntAutoPtr<IPipelineState>         m_LuminancePSO;
    RefCntAutoPtr<IPipelineState>         m_ToneMapPSO;
    RefCntAutoPtr<IShaderResourceBinding> m_LuminanceSRB;
    RefCntAutoPtr<IShaderResourceBinding> m_ToneMapSRB;
    RefCntAutoPtr<IBuffer>                m_ToneMappingCB;
    RefCntAutoPtr<ITexture>               m_LuminanceTexture;
    RefCntAutoPtr<ITextureView>           m_LuminanceRTV;
    RefCntAutoPtr<ITextureView>           m_LuminanceSRV;
    RefCntAutoPtr<ITexture>               m_FallbackLuminanceTexture;
    RefCntAutoPtr<ITextureView>           m_FallbackLuminanceSRV;
    RefCntAutoPtr<ISampler>               m_LinearSampler;
    RefCntAutoPtr<ISampler>               m_PointSampler;
    RTXPTToneMappingPassStats             m_Stats;
    Uint32                                m_Width  = 0;
    Uint32                                m_Height = 0;

    RefCntAutoPtr<IShader> m_FullscreenVS;
    RefCntAutoPtr<IShader> m_LuminancePS;
    TEXTURE_FORMAT         m_LuminanceFormat  = TEX_FORMAT_UNKNOWN;
    bool                   m_ComputeSupported = false;
};

} // namespace Diligent
