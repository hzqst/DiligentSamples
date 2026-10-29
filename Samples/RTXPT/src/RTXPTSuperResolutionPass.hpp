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

#include <string>
#include <vector>

#include "BasicMath.hpp"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "SuperResolution.h"
#include "SuperResolutionFactory.h"
#include "Texture.h"

#include "RTXPTRenderTargets.hpp"

namespace Diligent
{

struct IRenderStateCache;

struct RTXPTSuperResolutionSettings
{
    bool                               Enabled          = false;
    Int32                              ActiveVariantIdx = 0;
    SUPER_RESOLUTION_OPTIMIZATION_TYPE OptimizationType = SUPER_RESOLUTION_OPTIMIZATION_TYPE_BALANCED;
    float                              Sharpness        = 0.0f;
};

struct RTXPTSuperResolutionFrameDesc
{
    RTXPTRenderTargetDimensions Dimensions;
    INTERFACE_ID                VariantId        = {};
    SUPER_RESOLUTION_TYPE       Type             = SUPER_RESOLUTION_TYPE_SPATIAL;
    TEXTURE_FORMAT              ColorFormat      = TEX_FORMAT_UNKNOWN;
    TEXTURE_FORMAT              DepthFormat      = TEX_FORMAT_UNKNOWN;
    TEXTURE_FORMAT              MotionFormat     = TEX_FORMAT_UNKNOWN;
    TEXTURE_FORMAT              OutputFormat     = TEX_FORMAT_UNKNOWN;
    bool                        Enabled          = false;
    bool                        Temporal         = false;
    bool                        ResetHistory     = false;
    float2                      Jitter           = float2{0.0f, 0.0f};
    float                       Sharpness        = 0.0f;
    float                       TimeDeltaSeconds = 0.0f;
};

struct RTXPTSuperResolutionExecuteAttribs
{
    const RTXPTRenderTargets*            pRenderTargets     = nullptr;
    const RTXPTSuperResolutionFrameDesc* pFrameDesc         = nullptr;
    float                                CameraNear         = 0.0f;
    float                                CameraFar          = 0.0f;
    float                                CameraFovAngleVert = 0.0f;
    ITextureView*                        pColorSRV          = nullptr;
    ITextureView*                        pOutputUAV         = nullptr;
};

struct RTXPTSuperResolutionStats
{
    bool        FactoryReady      = false;
    bool        UpscalerReady     = false;
    bool        LastExecute       = false;
    bool        LastFrameTemporal = false;
    Uint32      VariantCount      = 0;
    Uint32      ExecuteCount      = 0;
    Uint32      RenderWidth       = 0;
    Uint32      RenderHeight      = 0;
    Uint32      DisplayWidth      = 0;
    Uint32      DisplayHeight     = 0;
    std::string DisabledReason;
};

class RTXPTSuperResolutionPass
{
public:
    void Reset();
    bool Initialize(IRenderDevice* pDevice, IRenderStateCache* pStateCache);

    RTXPTSuperResolutionFrameDesc ResolveFrameDesc(const RTXPTSuperResolutionSettings& Settings,
                                                   Uint32                              DisplayWidth,
                                                   Uint32                              DisplayHeight,
                                                   TEXTURE_FORMAT                      OutputFormat,
                                                   bool                                ResetHistory,
                                                   float                               TimeDeltaSeconds);
    bool                          Execute(IDeviceContext* pContext, const RTXPTSuperResolutionExecuteAttribs& ExecuteAttribs);

    const std::vector<SuperResolutionInfo>& GetVariants() const { return m_Variants; }
    const RTXPTSuperResolutionStats&        GetStats() const { return m_Stats; }
    bool                                    HasTemporalVariant() const;
    bool                                    SupportsSharpness(const SuperResolutionInfo& Info) const;

private:
    const SuperResolutionInfo* GetActiveVariant(const RTXPTSuperResolutionSettings& Settings) const;
    bool                       CreateMotionConversionPipeline(IRenderDevice* pDevice, IRenderStateCache* pStateCache);
    bool                       EnsureMotionConversionResources(IRenderDevice* pDevice, const RTXPTRenderTargets& RenderTargets);
    bool                       ConvertMotionVectors(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets);
    ITextureView*              GetSRMotionSRV() const;
    ITextureView*              GetSRMotionUAV() const;
    bool                       EnsureUpscaler(const RTXPTSuperResolutionFrameDesc& FrameDesc);
    SUPER_RESOLUTION_FLAGS     GetFlags(const SuperResolutionInfo& Variant, float Sharpness) const;

private:
    RefCntAutoPtr<IRenderDevice>           m_Device;
    RefCntAutoPtr<ISuperResolutionFactory> m_Factory;
    RefCntAutoPtr<ISuperResolution>        m_Upscaler;
    RefCntAutoPtr<IPipelineState>          m_MotionConversionPSO;
    RefCntAutoPtr<IShaderResourceBinding>  m_MotionConversionSRB;
    RefCntAutoPtr<ITexture>                m_SRMotionVectors;
    std::vector<SuperResolutionInfo>       m_Variants;
    RTXPTSuperResolutionStats              m_Stats;
    SuperResolutionDesc                    m_UpscalerDesc;
    Uint32                                 m_SRMotionWidth  = 0;
    Uint32                                 m_SRMotionHeight = 0;
};

} // namespace Diligent
