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

#include "RTXPTToneMappingPass.hpp"
#include "RTXPTShaderSource.hpp"
#include "RTXPTPipelineStateCache.hpp"

#include <algorithm>
#include <cmath>

#include "DebugUtilities.hpp"
#include "GraphicsAccessories.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"

namespace Diligent
{

namespace
{

constexpr float kDefaultAvgLuminance       = 1.0f;
constexpr float kShutterMin                = 0.001f;
constexpr float kShutterMax                = 10000.0f;
constexpr float kFNumberMin                = 0.1f;
constexpr float kFNumberMax                = 100.0f;
constexpr float kWhitePointMin             = 1667.0f;
constexpr float kWhitePointMax             = 25000.0f;
constexpr float kExposureValueSanitizeMin  = -32.0f;
constexpr float kExposureValueSanitizeMax  = 32.0f;
constexpr float kExposureCompensationMin   = -32.0f;
constexpr float kExposureCompensationMax   = 32.0f;
constexpr float kMinPositiveParameterValue = 1.0e-6f;
constexpr float kFilmSpeedMax              = 1000000.0f;
constexpr float kWhiteMaxLuminanceMax      = 1000000.0f;
constexpr float kWhiteScaleMin             = 0.01f;
constexpr float kWhiteScaleMax             = 1000000.0f;

struct RTXPTToneMappingConstants
{
    float  WhiteScale              = 5.1f;
    float  WhiteMaxLuminance       = 1.0f;
    Uint32 ToneMapOperator         = 0;
    Uint32 Clamped                 = 1;
    Uint32 AutoExposure            = 0;
    float  AvgLuminance            = 0.0f;
    float  AutoExposureLumValueMin = 0.0f;
    float  AutoExposureLumValueMax = 0.0f;
    float4 ColorTransform[3]       = {float4{1.0f, 0.0f, 0.0f, 0.0f}, float4{0.0f, 1.0f, 0.0f, 0.0f}, float4{0.0f, 0.0f, 1.0f, 0.0f}};
    Uint32 Enabled                 = 1;
    Uint32 _Padding0               = 0;
    Uint32 _Padding1               = 0;
    Uint32 _Padding2               = 0;
};
static_assert(sizeof(RTXPTToneMappingConstants) == 96, "RTXPTToneMappingConstants must match ToneMappingShared.h");

float ClampFloat(float Value, float MinValue, float MaxValue)
{
    return std::max(MinValue, std::min(MaxValue, Value));
}

float SanitizeFinite(float Value, float Fallback)
{
    return std::isfinite(Value) ? Value : Fallback;
}

float SanitizeFiniteRange(float Value, float Fallback, float MinValue, float MaxValue)
{
    return ClampFloat(SanitizeFinite(Value, Fallback), MinValue, MaxValue);
}

float3 Mul(const float3x3& Matrix, const float3& Value)
{
    return float3{
        Matrix._11 * Value.x + Matrix._12 * Value.y + Matrix._13 * Value.z,
        Matrix._21 * Value.x + Matrix._22 * Value.y + Matrix._23 * Value.z,
        Matrix._31 * Value.x + Matrix._32 * Value.y + Matrix._33 * Value.z};
}

float3x3 Diagonal(const float3& Value)
{
    return float3x3{
        Value.x, 0.0f, 0.0f,
        0.0f, Value.y, 0.0f,
        0.0f, 0.0f, Value.z};
}

float3 ColorTemperatureToXYZ(float Temperature, float Luminance = 1.0f)
{
    const double T  = ClampFloat(Temperature, kWhitePointMin, kWhitePointMax);
    const double T2 = T * T;
    const double T3 = T2 * T;

    const double X = T < 4000.0 ?
        -0.2661239e9 / T3 - 0.2343580e6 / T2 + 0.8776956e3 / T + 0.179910 :
        -3.0258469e9 / T3 + 2.1070379e6 / T2 + 0.2226347e3 / T + 0.240390;

    const double X2 = X * X;
    const double X3 = X2 * X;
    double       Y  = 0.0;
    if (T < 2222.0)
        Y = -1.1063814 * X3 - 1.34811020 * X2 + 2.18555832 * X - 0.20219683;
    else if (T < 4000.0)
        Y = -0.9549476 * X3 - 1.37418593 * X2 + 2.09137015 * X - 0.16748867;
    else
        Y = 3.0817580 * X3 - 5.87338670 * X2 + 3.75112997 * X - 0.37001483;

    return float3{
        static_cast<float>(X * Luminance / Y),
        Luminance,
        static_cast<float>((1.0 - X - Y) * Luminance / Y)};
}

float3x3 CalculateWhiteBalanceTransformRGBRec709(float WhitePoint)
{
    const float3x3 RGBToXYZRec709{
        0.4123907992659595f, 0.3575843393838780f, 0.1804807884018343f,
        0.2126390058715104f, 0.7151686787677559f, 0.0721923153607337f,
        0.0193308187155918f, 0.1191947797946259f, 0.9505321522496608f};
    const float3x3 XYZToRGBRec709{
        3.2409699419045213f, -1.5373831775700935f, -0.4986107602930033f,
        -0.9692436362808798f, 1.8759675015077206f, 0.0415550574071756f,
        0.0556300796969936f, -0.2039769588889765f, 1.0569715142428784f};
    const float3x3 XYZToLMSCAT02{
        0.7328f, 0.4296f, -0.1624f,
        -0.7036f, 1.6975f, 0.0061f,
        0.0030f, 0.0136f, 0.9834f};
    const float3x3 LMSToXYZCAT02{
        1.096123820835514f, -0.278869000218287f, 0.182745179382773f,
        0.454369041975359f, 0.473533154307412f, 0.072097803717229f,
        -0.009627608738429f, -0.005698031216113f, 1.015325639954543f};

    const float3x3 RGBToLMS = XYZToLMSCAT02 * RGBToXYZRec709;
    const float3x3 LMSToRGB = XYZToRGBRec709 * LMSToXYZCAT02;

    const float3 DstWhite = Mul(XYZToLMSCAT02, ColorTemperatureToXYZ(6500.0f));
    const float3 SrcWhite = Mul(XYZToLMSCAT02, ColorTemperatureToXYZ(WhitePoint));
    const float3 Scale{
        DstWhite.x / SrcWhite.x,
        DstWhite.y / SrcWhite.y,
        DstWhite.z / SrcWhite.z};

    return LMSToRGB * Diagonal(Scale) * RGBToLMS;
}

float ComputeRec709Luminance(const float3& Color)
{
    return dot(Color, float3{0.2126f, 0.7152f, 0.0722f});
}

float3 MulRowVector(const float3& Value, const float3x3& Matrix)
{
    return float3{
        Value.x * Matrix._11 + Value.y * Matrix._21 + Value.z * Matrix._31,
        Value.x * Matrix._12 + Value.y * Matrix._22 + Value.z * Matrix._32,
        Value.x * Matrix._13 + Value.y * Matrix._23 + Value.z * Matrix._33};
}

float GetValidAverageLuminance(float AvgLuminance)
{
    return (std::isfinite(AvgLuminance) && AvgLuminance > 0.0f) ? AvgLuminance : kDefaultAvgLuminance;
}

float ComputeManualExposureScale(const RTXPTToneMappingParameters& Params)
{
    const float Shutter = std::max(Params.Shutter, 0.001f);
    const float FNumber = std::max(Params.FNumber, 0.1f);
    return (Params.FilmSpeed / 100.0f) / (Shutter * FNumber * FNumber);
}

float3x3 ComputeWhiteBalanceTransform(const RTXPTToneMappingParameters& Params)
{
    return Params.WhiteBalance ?
        CalculateWhiteBalanceTransformRGBRec709(Params.WhitePoint) :
        float3x3::Identity();
}

float ComputeToneMappingExposureScale(const RTXPTToneMappingParameters& Params)
{
    const float ExposureScale = std::pow(2.0f, Params.ExposureCompensation);
    return ExposureScale * (Params.AutoExposure ? 1.0f : ComputeManualExposureScale(Params));
}

float3x3 ComputeToneMappingColorTransform(const RTXPTToneMappingParameters& Params)
{
    return ComputeWhiteBalanceTransform(Params) * ComputeToneMappingExposureScale(Params);
}

float ComputePreExposedGrayScale(const RTXPTToneMappingParameters& Params)
{
    const float ExposureScale = std::pow(2.0f, Params.ExposureCompensation);
    return ExposureScale * ComputeManualExposureScale(Params);
}

void UpdateExposureValue(RTXPTToneMappingParameters& Params)
{
    const float EVMin    = std::log2(kShutterMin * kFNumberMin * kFNumberMin);
    const float EVMax    = std::log2(kShutterMax * kFNumberMax * kFNumberMax);
    Params.ExposureValue = ClampFloat(Params.ExposureValue, EVMin, EVMax);

    if (Params.ExposureMode == RTXPTExposureMode::AperturePriority)
    {
        Params.Shutter = std::pow(2.0f, Params.ExposureValue) / (Params.FNumber * Params.FNumber);
        Params.Shutter = ClampFloat(Params.Shutter, kShutterMin, kShutterMax);
    }
    else
    {
        Params.FNumber = std::sqrt(std::pow(2.0f, Params.ExposureValue) / Params.Shutter);
        Params.FNumber = ClampFloat(Params.FNumber, kFNumberMin, kFNumberMax);
    }
}

RTXPTToneMappingParameters SanitizeToneMappingParameters(const RTXPTToneMappingParameters& Input)
{
    RTXPTToneMappingParameters Params = Input;

    if (Params.ExposureMode != RTXPTExposureMode::AperturePriority &&
        Params.ExposureMode != RTXPTExposureMode::ShutterPriority)
        Params.ExposureMode = RTXPTExposureMode::AperturePriority;

    const Uint32 Operator  = std::min(static_cast<Uint32>(Params.ToneMapOperator), static_cast<Uint32>(RTXPTToneMapperOperator::Aces));
    Params.ToneMapOperator = static_cast<RTXPTToneMapperOperator>(Operator);

    Params.ExposureCompensation = SanitizeFiniteRange(Params.ExposureCompensation, 0.0f, kExposureCompensationMin, kExposureCompensationMax);
    Params.ExposureValue        = SanitizeFiniteRange(Params.ExposureValue, 0.0f, kExposureValueSanitizeMin, kExposureValueSanitizeMax);
    Params.ExposureValueMin     = SanitizeFiniteRange(Params.ExposureValueMin, -16.0f, kExposureValueSanitizeMin, kExposureValueSanitizeMax);
    Params.ExposureValueMax     = SanitizeFiniteRange(Params.ExposureValueMax, 16.0f, kExposureValueSanitizeMin, kExposureValueSanitizeMax);
    if (Params.ExposureValueMin > Params.ExposureValueMax)
        std::swap(Params.ExposureValueMin, Params.ExposureValueMax);

    Params.FilmSpeed         = SanitizeFiniteRange(Params.FilmSpeed, 100.0f, 1.0f, kFilmSpeedMax);
    Params.FNumber           = SanitizeFiniteRange(Params.FNumber, 1.0f, kFNumberMin, kFNumberMax);
    Params.Shutter           = SanitizeFiniteRange(Params.Shutter, 1.0f, kShutterMin, kShutterMax);
    Params.WhitePoint        = SanitizeFiniteRange(Params.WhitePoint, 6500.0f, kWhitePointMin, kWhitePointMax);
    Params.WhiteMaxLuminance = SanitizeFiniteRange(Params.WhiteMaxLuminance, 1.0f, kMinPositiveParameterValue, kWhiteMaxLuminanceMax);
    Params.WhiteScale        = SanitizeFiniteRange(Params.WhiteScale, 5.1f, kWhiteScaleMin, kWhiteScaleMax);

    return Params;
}

RTXPTToneMappingParameters PrepareToneMappingParameters(const RTXPTToneMappingParameters& Input, bool ComputeSupported)
{
    RTXPTToneMappingParameters Params = SanitizeToneMappingParameters(Input);
    Params.AutoExposure               = Params.AutoExposure && ComputeSupported;
    UpdateExposureValue(Params);
    return Params;
}

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

bool CreateFallbackLuminanceTexture(IRenderDevice* pDevice, RefCntAutoPtr<ITexture>& Texture, RefCntAutoPtr<ITextureView>& SRV)
{
    Texture.Release();
    SRV.Release();

    if (pDevice == nullptr)
        return false;

    TextureDesc Desc;
    Desc.Name      = "RTXPT fallback luminance texture";
    Desc.Type      = RESOURCE_DIM_TEX_2D;
    Desc.Usage     = USAGE_IMMUTABLE;
    Desc.BindFlags = BIND_SHADER_RESOURCE;
    Desc.Format    = TEX_FORMAT_R32_FLOAT;
    Desc.Width     = 1;
    Desc.Height    = 1;
    Desc.MipLevels = 1;

    const float       LogLuminance = 0.0f;
    TextureSubResData Subres{&LogLuminance, sizeof(LogLuminance)};
    TextureData       InitData{&Subres, 1};
    pDevice->CreateTexture(Desc, &InitData, &Texture);

    SRV = Texture ? Texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    return SRV != nullptr;
}

} // namespace

void RTXPTToneMappingPass::Reset()
{
    m_LuminancePSO.Release();
    m_ToneMapPSO.Release();
    m_LuminanceSRB.Release();
    m_ToneMapSRB.Release();
    m_ToneMappingCB.Release();
    m_LuminanceTexture.Release();
    m_LuminanceRTV.Release();
    m_LuminanceSRV.Release();
    m_FallbackLuminanceTexture.Release();
    m_FallbackLuminanceSRV.Release();
    m_LinearSampler.Release();
    m_PointSampler.Release();
    m_FullscreenVS.Release();
    m_LuminancePS.Release();
    m_Stats                  = {};
    m_Stats.LastAvgLuminance = kDefaultAvgLuminance;
    m_Width                  = 0;
    m_Height                 = 0;
    m_LuminanceFormat        = TEX_FORMAT_UNKNOWN;
    m_ComputeSupported       = false;
}

bool RTXPTToneMappingPass::CreateSamplers(IRenderDevice* pDevice)
{
    if (pDevice == nullptr)
        return false;

    SamplerDesc LinearSampler;
    LinearSampler.Name      = "RTXPT tone mapping linear sampler";
    LinearSampler.MinFilter = FILTER_TYPE_LINEAR;
    LinearSampler.MagFilter = FILTER_TYPE_LINEAR;
    LinearSampler.MipFilter = FILTER_TYPE_LINEAR;
    LinearSampler.AddressU  = TEXTURE_ADDRESS_CLAMP;
    LinearSampler.AddressV  = TEXTURE_ADDRESS_CLAMP;
    LinearSampler.AddressW  = TEXTURE_ADDRESS_CLAMP;
    pDevice->CreateSampler(LinearSampler, &m_LinearSampler);

    SamplerDesc PointSampler = LinearSampler;
    PointSampler.Name        = "RTXPT tone mapping point sampler";
    PointSampler.MinFilter   = FILTER_TYPE_POINT;
    PointSampler.MagFilter   = FILTER_TYPE_POINT;
    PointSampler.MipFilter   = FILTER_TYPE_POINT;
    pDevice->CreateSampler(PointSampler, &m_PointSampler);

    VERIFY(m_LinearSampler && m_PointSampler, "Failed to create RTXPT tone mapping samplers");
    return m_LinearSampler && m_PointSampler;
}

bool RTXPTToneMappingPass::Initialize(IRenderDevice*       pDevice,
                                      IEngineFactory*      pEngineFactory,
                                      IRenderStateCache*   pStateCache,
                                      IPipelineStateCache* pPSOCache,
                                      TEXTURE_FORMAT       LdrFormat,
                                      bool                 ComputeSupported)
{
    Reset();

    if (pDevice == nullptr || pEngineFactory == nullptr)
    {
        DEV_ERROR("RTXPT tone mapping pass requires a render device and engine factory");
        return false;
    }

    if (!ComputeSupported)
    {
        DEV_ERROR("RTXPT tone mapping pass requires compute shader support");
        return false;
    }

    m_ComputeSupported = ComputeSupported;
    m_pStateCache      = pStateCache;
    m_pPSOCache        = pPSOCache;

    if (!CreateSamplers(pDevice))
    {
        DEV_ERROR("RTXPT tone mapping pass failed to create samplers");
        return false;
    }
    if (!CreateFallbackLuminanceTexture(pDevice, m_FallbackLuminanceTexture, m_FallbackLuminanceSRV))
    {
        DEV_ERROR("RTXPT tone mapping pass failed to create fallback luminance");
        return false;
    }

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    CreateRTXPTShaderSourceFactory(pEngineFactory, "shaders;shaders\\PostProcessing\\ToneMapper", &pShaderSourceFactory);

    ShaderCreateInfo ShaderCI;
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

    RefCntAutoPtr<IShader> pToneMapPS;
    const bool             GraphicsShadersReady =
        CreateShader(pStateCache, ShaderCI, SHADER_TYPE_VERTEX, "RTXPT tone mapping VS", "PostProcessing/RTXPTFullscreen.vsh", "main", m_FullscreenVS) &&
        CreateShader(pStateCache, ShaderCI, SHADER_TYPE_PIXEL, "RTXPT tone mapping PS", "PostProcessing/ToneMapper/ToneMapping.hlsl", "main_ps", pToneMapPS);
    if (!GraphicsShadersReady)
    {
        DEV_ERROR("RTXPT tone mapping pass failed to create shaders");
        return false;
    }
    if (m_ComputeSupported)
    {
        const bool AutoExposureShadersReady =
            CreateShader(pStateCache, ShaderCI, SHADER_TYPE_PIXEL, "RTXPT luminance PS", "PostProcessing/ToneMapper/Luminance.psh", "main", m_LuminancePS);
        if (!AutoExposureShadersReady)
        {
            DEV_ERROR("RTXPT tone mapping pass failed to create auto-exposure shaders");
            return false;
        }
    }

    BufferDesc ConstantsDesc;
    ConstantsDesc.Name           = "RTXPT tone mapping constants";
    ConstantsDesc.Size           = sizeof(RTXPTToneMappingConstants);
    ConstantsDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage          = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &m_ToneMappingCB);
    if (!m_ToneMappingCB)
    {
        DEV_ERROR("RTXPT tone mapping pass failed to create constants");
        return false;
    }

    GraphicsPipelineStateCreateInfo ToneMapPSOCreateInfo;
    ToneMapPSOCreateInfo.PSODesc.Name                                  = "RTXPT tone mapping PSO";
    ToneMapPSOCreateInfo.PSODesc.PipelineType                          = PIPELINE_TYPE_GRAPHICS;
    ToneMapPSOCreateInfo.GraphicsPipeline.NumRenderTargets             = 1;
    ToneMapPSOCreateInfo.GraphicsPipeline.RTVFormats[0]                = LdrFormat;
    ToneMapPSOCreateInfo.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    ToneMapPSOCreateInfo.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    ToneMapPSOCreateInfo.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    ToneMapPSOCreateInfo.pVS                                           = m_FullscreenVS;
    ToneMapPSOCreateInfo.pPS                                           = pToneMapPS;

    PipelineResourceLayoutDescX ToneMapLayout;
    ToneMapLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
    ToneMapLayout
        .AddVariable(SHADER_TYPE_PIXEL, "g_ToneMappingConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_PIXEL, "s_ColorSampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_PIXEL, "s_LuminanceSampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
        .AddVariable(SHADER_TYPE_PIXEL, "t_Color", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_PIXEL, "t_Luminance", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
    ToneMapPSOCreateInfo.PSODesc.ResourceLayout = ToneMapLayout;

    ToneMapPSOCreateInfo.pPSOCache = SelectRTXPTPipelineStateCache(pDevice, ToneMapPSOCreateInfo, pPSOCache);
    pStateCache->CreateGraphicsPipelineState(ToneMapPSOCreateInfo, &m_ToneMapPSO);
    if (!m_ToneMapPSO)
    {
        DEV_ERROR("RTXPT tone mapping pass failed to create tone-map PSO");
        return false;
    }

    const bool StaticBound =
        SetStaticVariable(m_ToneMapPSO, SHADER_TYPE_PIXEL, "g_ToneMappingConstants", m_ToneMappingCB, true) &&
        SetStaticVariable(m_ToneMapPSO, SHADER_TYPE_PIXEL, "s_ColorSampler", m_PointSampler, true) &&
        SetStaticVariable(m_ToneMapPSO, SHADER_TYPE_PIXEL, "s_LuminanceSampler", m_LinearSampler, false);
    if (!StaticBound)
    {
        DEV_ERROR("RTXPT tone mapping pass failed to bind static resources");
        return false;
    }

    m_ToneMapPSO->CreateShaderResourceBinding(&m_ToneMapSRB, true);
    if (!m_ToneMapSRB)
    {
        DEV_ERROR("RTXPT tone mapping pass failed to create SRBs");
        return false;
    }

    m_Stats.Ready             = true;
    m_Stats.AutoExposureReady = false;
    return true;
}

bool RTXPTToneMappingPass::CreateLuminanceResources(IRenderDevice* pDevice, Uint32 Width, Uint32 Height, TEXTURE_FORMAT SourceFormat)
{
    m_LuminanceTexture.Release();
    m_LuminanceRTV.Release();
    m_LuminanceSRV.Release();
    m_Stats.LastAvgLuminance  = kDefaultAvgLuminance;
    m_Stats.AutoExposureReady = false;

    TextureDesc Desc;
    Desc.Name      = "RTXPT luminance texture";
    Desc.Type      = RESOURCE_DIM_TEX_2D;
    Desc.Width     = 1u << static_cast<Uint32>(std::floor(std::log2(static_cast<float>(Width))));
    Desc.Height    = 1u << static_cast<Uint32>(std::floor(std::log2(static_cast<float>(Height))));
    Desc.MipLevels = ComputeMipLevelsCount(Desc.Width, Desc.Height);
    Desc.Format    = SourceFormat == TEX_FORMAT_RGBA32_FLOAT ? TEX_FORMAT_R32_FLOAT : TEX_FORMAT_R16_FLOAT;
    Desc.BindFlags = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
    Desc.MiscFlags = MISC_TEXTURE_FLAG_GENERATE_MIPS;

    pDevice->CreateTexture(Desc, nullptr, &m_LuminanceTexture);
    if (!m_LuminanceTexture)
        return false;

    m_LuminanceRTV = m_LuminanceTexture->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    m_LuminanceSRV = m_LuminanceTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    if (!m_LuminanceRTV || !m_LuminanceSRV)
        return false;

    m_Width                   = Width;
    m_Height                  = Height;
    m_LuminanceFormat         = Desc.Format;
    m_Stats.AutoExposureReady = true;
    return true;
}

bool RTXPTToneMappingPass::ResizeResources(IRenderDevice* pDevice, Uint32 Width, Uint32 Height, TEXTURE_FORMAT SourceFormat)
{
    if (!IsReady() || pDevice == nullptr || Width == 0 || Height == 0)
        return false;

    if (!m_ComputeSupported)
    {
        m_Stats.AutoExposureReady = false;
        DEV_ERROR("RTXPT tone mapping pass requires compute shader support");
        return false;
    }

    const TEXTURE_FORMAT LuminanceFormat = SourceFormat == TEX_FORMAT_RGBA32_FLOAT ? TEX_FORMAT_R32_FLOAT : TEX_FORMAT_R16_FLOAT;
    if (m_Stats.AutoExposureReady &&
        m_Width == Width &&
        m_Height == Height &&
        m_LuminanceFormat == LuminanceFormat &&
        m_LuminanceTexture &&
        m_LuminanceRTV &&
        m_LuminanceSRV)
        return true;

    if (!m_LuminancePSO || !m_LuminanceSRB || m_LuminanceFormat != LuminanceFormat)
    {
        GraphicsPipelineStateCreateInfo PSOCreateInfo;
        PSOCreateInfo.PSODesc.Name                                  = "RTXPT luminance PSO";
        PSOCreateInfo.PSODesc.PipelineType                          = PIPELINE_TYPE_GRAPHICS;
        PSOCreateInfo.GraphicsPipeline.NumRenderTargets             = 1;
        PSOCreateInfo.GraphicsPipeline.RTVFormats[0]                = LuminanceFormat;
        PSOCreateInfo.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        PSOCreateInfo.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
        PSOCreateInfo.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
        PSOCreateInfo.pVS                                           = m_FullscreenVS;
        PSOCreateInfo.pPS                                           = m_LuminancePS;

        PipelineResourceLayoutDescX ResourceLayout;
        ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE;
        ResourceLayout
            .AddVariable(SHADER_TYPE_PIXEL, "s_ColorSampler", SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
            .AddVariable(SHADER_TYPE_PIXEL, "t_Color", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC);
        PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

        m_LuminancePSO.Release();
        m_LuminanceSRB.Release();
        PSOCreateInfo.pPSOCache = SelectRTXPTPipelineStateCache(pDevice, PSOCreateInfo, m_pPSOCache);
        m_pStateCache->CreateGraphicsPipelineState(PSOCreateInfo, &m_LuminancePSO);
        if (!m_LuminancePSO ||
            !SetStaticVariable(m_LuminancePSO, SHADER_TYPE_PIXEL, "s_ColorSampler", m_LinearSampler, true))
        {
            m_Stats.AutoExposureReady = false;
            DEV_ERROR("RTXPT tone mapping pass failed to create luminance PSO");
            return false;
        }

        m_LuminancePSO->CreateShaderResourceBinding(&m_LuminanceSRB, true);
        if (!m_LuminanceSRB)
        {
            m_Stats.AutoExposureReady = false;
            DEV_ERROR("RTXPT tone mapping pass failed to create luminance SRB");
            return false;
        }
    }

    if (!CreateLuminanceResources(pDevice, Width, Height, SourceFormat))
    {
        m_Stats.AutoExposureReady = false;
        DEV_ERROR("RTXPT tone mapping pass failed to create luminance resources");
        return false;
    }

    return true;
}

bool RTXPTToneMappingPass::UpdateToneMappingConstants(IDeviceContext* pContext, const RTXPTToneMappingParameters& Params, bool Enabled)
{
    if (pContext == nullptr || !m_ToneMappingCB)
        return false;

    const float3x3 ColorTransform = ComputeToneMappingColorTransform(Params);

    RTXPTToneMappingConstants Constants;
    Constants.WhiteScale              = Params.WhiteScale;
    Constants.WhiteMaxLuminance       = Params.WhiteMaxLuminance;
    Constants.ToneMapOperator         = static_cast<Uint32>(Params.ToneMapOperator);
    Constants.Clamped                 = Params.Clamped ? 1u : 0u;
    Constants.AutoExposure            = Params.AutoExposure ? 1u : 0u;
    Constants.AvgLuminance            = GetValidAverageLuminance(m_Stats.LastAvgLuminance);
    Constants.AutoExposureLumValueMin = std::pow(2.0f, Params.AutoExposure ? Params.ExposureValueMin : -16.0f);
    Constants.AutoExposureLumValueMax = std::pow(2.0f, Params.AutoExposure ? Params.ExposureValueMax : 16.0f);
    Constants.ColorTransform[0]       = float4{ColorTransform._11, ColorTransform._21, ColorTransform._31, 0.0f};
    Constants.ColorTransform[1]       = float4{ColorTransform._12, ColorTransform._22, ColorTransform._32, 0.0f};
    Constants.ColorTransform[2]       = float4{ColorTransform._13, ColorTransform._23, ColorTransform._33, 0.0f};
    Constants.Enabled                 = Enabled ? 1u : 0u;

    MapHelper<RTXPTToneMappingConstants> Mapped{pContext, m_ToneMappingCB, MAP_WRITE, MAP_FLAG_DISCARD};
    if (!Mapped)
        return false;

    *Mapped = Constants;
    return true;
}

float RTXPTToneMappingPass::ComputePreExposedGrayLuminance(const RTXPTToneMappingParameters& Params, bool Enabled) const
{
    if (!Enabled)
        return 1.0f;

    const RTXPTToneMappingParameters PreparedParams = PrepareToneMappingParameters(Params, m_ComputeSupported);
    if (PreparedParams.AutoExposure)
    {
        // Display auto exposure is derived from the GPU luminance mip chain later in Render().
        // G2 has no CPU readback/shared exposure state yet, so keep frame constants neutral.
        return 1.0f;
    }

    const float  Exposure = ComputePreExposedGrayScale(PreparedParams);
    const float3 Gray     = MulRowVector(float3{0.5f, 0.5f, 0.5f} * Exposure,
                                     ComputeWhiteBalanceTransform(PreparedParams));
    return std::max(ComputeRec709Luminance(Gray), 1.0e-6f);
}

bool RTXPTToneMappingPass::Render(IDeviceContext* pContext, const RTXPTToneMappingRenderAttribs& Attribs)
{
    m_Stats.LastRenderExecuted = false;

    if (!IsReady() || pContext == nullptr ||
        Attribs.pSourceSRV == nullptr ||
        Attribs.pLdrRTV == nullptr ||
        Attribs.Width == 0 ||
        Attribs.Height == 0 ||
        Attribs.pParams == nullptr)
        return false;

    RTXPTToneMappingParameters Params          = PrepareToneMappingParameters(*Attribs.pParams, m_ComputeSupported);
    const bool                 UseAutoExposure = Params.AutoExposure;

    if (!UpdateToneMappingConstants(pContext, Params, Attribs.Enabled))
        return false;

    if (UseAutoExposure)
    {
        if (!m_Stats.AutoExposureReady || !m_LuminancePSO || !m_LuminanceSRB ||
            !m_LuminanceRTV || !m_LuminanceSRV)
            return false;

        const bool LuminanceBound =
            SetSRBVariable(m_LuminanceSRB, SHADER_TYPE_PIXEL, "t_Color", Attribs.pSourceSRV, true);
        if (!LuminanceBound)
            return false;

        ITextureView* pLuminanceRTV = m_LuminanceRTV;
        pContext->SetRenderTargets(1, &pLuminanceRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        const TextureDesc& LuminanceDesc = m_LuminanceTexture->GetDesc();
        pContext->SetViewports(1, nullptr, LuminanceDesc.Width, LuminanceDesc.Height);
        pContext->SetPipelineState(m_LuminancePSO);
        pContext->CommitShaderResources(m_LuminanceSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        pContext->Draw(DrawAttribs{3, DRAW_FLAG_VERIFY_ALL});

        pContext->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        pContext->GenerateMips(m_LuminanceSRV);
    }

    ITextureView* pToneMapLuminanceSRV = m_LuminanceSRV ? m_LuminanceSRV.RawPtr() : m_FallbackLuminanceSRV.RawPtr();
    const bool    ToneMapBound =
        SetSRBVariable(m_ToneMapSRB, SHADER_TYPE_PIXEL, "t_Color", Attribs.pSourceSRV, true) &&
        SetSRBVariable(m_ToneMapSRB, SHADER_TYPE_PIXEL, "t_Luminance", pToneMapLuminanceSRV, false);
    if (!ToneMapBound)
        return false;

    ITextureView* pRTV = Attribs.pLdrRTV;
    pContext->SetRenderTargets(1, &pRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->SetViewports(1, nullptr, Attribs.Width, Attribs.Height);
    pContext->SetPipelineState(m_ToneMapPSO);
    pContext->CommitShaderResources(m_ToneMapSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->Draw(DrawAttribs{3, DRAW_FLAG_VERIFY_ALL});

    m_Stats.LastRenderExecuted = true;
    ++m_Stats.RenderCount;
    return true;
}

} // namespace Diligent
