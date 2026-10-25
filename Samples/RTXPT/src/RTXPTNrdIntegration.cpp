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

#include "RTXPTNrdIntegration.hpp"

#include "DebugUtilities.hpp"

#if RTXPT_HAS_NRD
#    include "GraphicsTypesX.hpp"
#    include "MapHelper.hpp"
#    include "RenderStateCache.h"
#    include "Shader.h"
#    include "ShaderMacroHelper.hpp"
#    if RTXPT_HAS_D3D_SHADER_REFLECTION
#        include "ShaderD3D.h"
#    endif

#    include <algorithm>
#    include <array>
#    include <cstddef>
#    include <cstring>
#    include <limits>
#    include <sstream>
#    include <string>
#    include <utility>
#    include <vector>
#endif

namespace Diligent
{

namespace
{

void ResetNrdStats(RTXPTNrdIntegrationStats& Stats)
{
    Stats.Ready                = false;
    Stats.LastDispatchExecuted = false;
    Stats.DispatchCount        = 0;
    Stats.LastPlaneIndex       = 0;
    Stats.LastDispatches       = 0;
    Stats.Width                = 0;
    Stats.Height               = 0;
    Stats.Method               = RTXPTNrdMethod::REBLUR;
    Stats.LastFailureReason.clear();
}

} // namespace

RTXPTNrdIntegration::~RTXPTNrdIntegration()
{
    Reset();
}

bool RTXPTNrdIntegration::Fail(const char* Reason)
{
    m_Stats.LastFailureReason = Reason != nullptr ? Reason : "NRD integration failed";
    DEV_ERROR(m_Stats.LastFailureReason.c_str());
    return false;
}

#if !RTXPT_HAS_NRD

void RTXPTNrdIntegration::Reset()
{
    ResetNrdStats(m_Stats);
}

bool RTXPTNrdIntegration::Initialize(IRenderDevice*,
                                     IEngineFactory*,
                                     IRenderStateCache*,
                                     RTXPTNrdMethod Method,
                                     Uint32         Width,
                                     Uint32         Height,
                                     bool)
{
    m_Stats.Method = Method;
    m_Stats.Width  = Width;
    m_Stats.Height = Height;
    return Fail(RTXPTGetNrdUnavailableReason());
}

bool RTXPTNrdIntegration::Dispatch(IDeviceContext*, const RTXPTNrdFrameAttribs&)
{
    m_Stats.LastDispatchExecuted = false;
    return Fail(RTXPTGetNrdUnavailableReason());
}

#endif

#if RTXPT_HAS_NRD

namespace
{

struct ParsedShaderIdentifier
{
    std::string                                      FilePath;
    std::vector<std::pair<std::string, std::string>> Macros;
};

void AppendShaderSearchDir(std::string& SearchDirs, const char* Path)
{
    if (Path == nullptr || Path[0] == '\0')
        return;

    if (!SearchDirs.empty())
        SearchDirs += ';';
    SearchDirs += Path;
}

std::string BuildNrdShaderSearchDirs()
{
    std::string SearchDirs;
    AppendShaderSearchDir(SearchDirs, RTXPT_NRD_SHADER_SOURCE_DIR);
    AppendShaderSearchDir(SearchDirs, RTXPT_NRD_SHADER_INCLUDE_DIR);
    AppendShaderSearchDir(SearchDirs, RTXPT_NRD_SHADER_CONFIG_DIR);
    AppendShaderSearchDir(SearchDirs, RTXPT_NRD_MATHLIB_INCLUDE_DIR);
    return SearchDirs;
}

ParsedShaderIdentifier ParseShaderIdentifier(const char* ShaderIdentifier)
{
    ParsedShaderIdentifier Parsed;
    if (ShaderIdentifier == nullptr)
        return Parsed;

    std::istringstream Stream{ShaderIdentifier};
    std::string        Token;
    if (!std::getline(Stream, Parsed.FilePath, '|'))
        return Parsed;

    while (std::getline(Stream, Token, '|'))
    {
        const std::string::size_type EqualPos = Token.find('=');
        if (EqualPos == std::string::npos || EqualPos == 0)
            continue;

        Parsed.Macros.emplace_back(Token.substr(0, EqualPos), Token.substr(EqualPos + 1));
    }

    return Parsed;
}

void AddNrdRequiredMacros(ShaderMacroHelper& Macros, const nrd::InstanceDesc& InstanceDesc)
{
    Macros.Add("NRD_INTERNAL", 1);
    Macros.Add("NRD_COMPILER_DXC", 1);
    Macros.Add("NRD_NORMAL_ENCODING", 2);
    Macros.Add("NRD_ROUGHNESS_ENCODING", 1);
    Macros.Add("NRD_CONSTANT_BUFFER_REGISTER_INDEX", static_cast<int>(InstanceDesc.constantBufferRegisterIndex));
    Macros.Add("NRD_SAMPLERS_BASE_REGISTER_INDEX", static_cast<int>(InstanceDesc.samplersBaseRegisterIndex));
    Macros.Add("NRD_RESOURCES_BASE_REGISTER_INDEX", static_cast<int>(InstanceDesc.resourcesBaseRegisterIndex));
    Macros.Add("NRD_CONSTANT_BUFFER_AND_SAMPLERS_SPACE_INDEX", static_cast<int>(InstanceDesc.constantBufferAndSamplersSpaceIndex));
    Macros.Add("NRD_RESOURCES_SPACE_INDEX", static_cast<int>(InstanceDesc.resourcesSpaceIndex));
}

void SortReflectedResources(std::vector<std::pair<Uint32, std::string>>& Resources)
{
    std::sort(Resources.begin(), Resources.end(),
              [](const auto& Lhs, const auto& Rhs) {
                  if (Lhs.first != Rhs.first)
                      return Lhs.first < Rhs.first;
                  return Lhs.second < Rhs.second;
              });
}

TEXTURE_FORMAT ToDiligentFormat(nrd::Format Format)
{
    static constexpr std::array<TEXTURE_FORMAT, static_cast<std::size_t>(nrd::Format::MAX_NUM)> FormatMap =
        {
            TEX_FORMAT_R8_UNORM,
            TEX_FORMAT_R8_SNORM,
            TEX_FORMAT_R8_UINT,
            TEX_FORMAT_R8_SINT,
            TEX_FORMAT_RG8_UNORM,
            TEX_FORMAT_RG8_SNORM,
            TEX_FORMAT_RG8_UINT,
            TEX_FORMAT_RG8_SINT,
            TEX_FORMAT_RGBA8_UNORM,
            TEX_FORMAT_RGBA8_SNORM,
            TEX_FORMAT_RGBA8_UINT,
            TEX_FORMAT_RGBA8_SINT,
            TEX_FORMAT_RGBA8_UNORM_SRGB,
            TEX_FORMAT_R16_UNORM,
            TEX_FORMAT_R16_SNORM,
            TEX_FORMAT_R16_UINT,
            TEX_FORMAT_R16_SINT,
            TEX_FORMAT_R16_FLOAT,
            TEX_FORMAT_RG16_UNORM,
            TEX_FORMAT_RG16_SNORM,
            TEX_FORMAT_RG16_UINT,
            TEX_FORMAT_RG16_SINT,
            TEX_FORMAT_RG16_FLOAT,
            TEX_FORMAT_RGBA16_UNORM,
            TEX_FORMAT_RGBA16_SNORM,
            TEX_FORMAT_RGBA16_UINT,
            TEX_FORMAT_RGBA16_SINT,
            TEX_FORMAT_RGBA16_FLOAT,
            TEX_FORMAT_R32_UINT,
            TEX_FORMAT_R32_SINT,
            TEX_FORMAT_R32_FLOAT,
            TEX_FORMAT_RG32_UINT,
            TEX_FORMAT_RG32_SINT,
            TEX_FORMAT_RG32_FLOAT,
            TEX_FORMAT_RGB32_UINT,
            TEX_FORMAT_RGB32_SINT,
            TEX_FORMAT_RGB32_FLOAT,
            TEX_FORMAT_RGBA32_UINT,
            TEX_FORMAT_RGBA32_SINT,
            TEX_FORMAT_RGBA32_FLOAT,
            TEX_FORMAT_RGB10A2_UNORM,
            TEX_FORMAT_RGB10A2_UINT,
            TEX_FORMAT_R11G11B10_FLOAT,
            TEX_FORMAT_RGB9E5_SHAREDEXP,
        };

    const auto Index = static_cast<std::size_t>(Format);
    return Index < FormatMap.size() ? FormatMap[Index] : TEX_FORMAT_UNKNOWN;
}

Uint32 DivideUp(Uint32 Value, Uint32 Divisor)
{
    return Value / Divisor + (Value % Divisor != 0 ? 1u : 0u);
}

void CopyMatrixToNrd(float* pDst, const float4x4& Matrix)
{
    static_assert(sizeof(float4x4) == sizeof(float) * 16, "Unexpected matrix layout");
    std::memcpy(pDst, &Matrix, sizeof(Matrix));
}

Uint16 ToNrdDimension(Uint32 Value)
{
    return static_cast<Uint16>(std::min<Uint32>(Value, std::numeric_limits<Uint16>::max()));
}

const char* GetNrdResourceTypeName(nrd::ResourceType Type)
{
    switch (Type)
    {
        case nrd::ResourceType::IN_MV:
            return "IN_MV";
        case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
            return "IN_NORMAL_ROUGHNESS";
        case nrd::ResourceType::IN_VIEWZ:
            return "IN_VIEWZ";
        case nrd::ResourceType::IN_DISOCCLUSION_THRESHOLD_MIX:
            return "IN_DISOCCLUSION_THRESHOLD_MIX";
        case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
            return "IN_DIFF_RADIANCE_HITDIST";
        case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
            return "IN_SPEC_RADIANCE_HITDIST";
        case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
            return "OUT_DIFF_RADIANCE_HITDIST";
        case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
            return "OUT_SPEC_RADIANCE_HITDIST";
        case nrd::ResourceType::OUT_VALIDATION:
            return "OUT_VALIDATION";
        case nrd::ResourceType::TRANSIENT_POOL:
            return "TRANSIENT_POOL";
        case nrd::ResourceType::PERMANENT_POOL:
            return "PERMANENT_POOL";
        default:
            return "unsupported";
    }
}

TEXTURE_VIEW_TYPE GetDiligentViewType(nrd::DescriptorType DescriptorType)
{
    return DescriptorType == nrd::DescriptorType::TEXTURE ?
        TEXTURE_VIEW_SHADER_RESOURCE :
        TEXTURE_VIEW_UNORDERED_ACCESS;
}

} // namespace

void RTXPTNrdIntegration::Reset()
{
    if (m_Instance != nullptr)
    {
        nrd::DestroyInstance(*m_Instance);
        m_Instance = nullptr;
    }

    m_Device.Release();
    m_ConstantBuffer.Release();
    m_Pipelines.clear();
    m_Samplers.clear();
    m_PermanentTextures.clear();
    m_TransientTextures.clear();
    ResetNrdStats(m_Stats);
}

bool RTXPTNrdIntegration::Initialize(IRenderDevice*     pDevice,
                                     IEngineFactory*    pEngineFactory,
                                     IRenderStateCache* pStateCache,
                                     RTXPTNrdMethod     Method,
                                     Uint32             Width,
                                     Uint32             Height,
                                     bool               ComputeSupported)
{
    Reset();
    m_Stats.Method = Method;
    m_Stats.Width  = Width;
    m_Stats.Height = Height;

    if (!ComputeSupported)
        return Fail("RTXPT NRD requires compute shader support");
    if (pDevice == nullptr || pEngineFactory == nullptr)
        return Fail("RTXPT NRD requires a device and engine factory");
    if (Width == 0 || Height == 0)
        return Fail("RTXPT NRD requires a non-zero render size");

    m_Device                    = pDevice;
    const auto ReleaseResources = [this]() {
        if (m_Instance != nullptr)
        {
            nrd::DestroyInstance(*m_Instance);
            m_Instance = nullptr;
        }

        m_Device.Release();
        m_ConstantBuffer.Release();
        m_Pipelines.clear();
        m_Samplers.clear();
        m_PermanentTextures.clear();
        m_TransientTextures.clear();
    };

    if (!CreateInstance(Method) ||
        !CreateConstantBuffer(pDevice) ||
        !CreateSamplers(pDevice) ||
        !CreatePipelines(pDevice, pEngineFactory, pStateCache) ||
        !CreatePoolTextures(pDevice, Width, Height))
    {
        ReleaseResources();
        return false;
    }

    m_Stats.Ready = true;
    return true;
}

bool RTXPTNrdIntegration::CreateInstance(RTXPTNrdMethod Method)
{
    const nrd::DenoiserDesc DenoiserDescs[] =
        {
            {m_Identifier, RTXPTToNrdDenoiser(Method)},
        };

    nrd::InstanceCreationDesc Desc = {};
    Desc.denoisers                 = DenoiserDescs;
    Desc.denoisersNum              = 1;

    const nrd::Result Result = nrd::CreateInstance(Desc, m_Instance);
    return Result == nrd::Result::SUCCESS && m_Instance != nullptr ?
        true :
        Fail("Failed to create NRD instance");
}

bool RTXPTNrdIntegration::CreateConstantBuffer(IRenderDevice* pDevice)
{
    const nrd::InstanceDesc* pInstanceDesc = nrd::GetInstanceDesc(*m_Instance);
    if (pInstanceDesc == nullptr)
        return Fail("Failed to query NRD instance description");

    BufferDesc Desc;
    Desc.Name           = "RTXPT NRD constants";
    Desc.Size           = pInstanceDesc->constantBufferMaxDataSize;
    Desc.BindFlags      = BIND_UNIFORM_BUFFER;
    Desc.Usage          = USAGE_DYNAMIC;
    Desc.CPUAccessFlags = CPU_ACCESS_WRITE;

    pDevice->CreateBuffer(Desc, nullptr, &m_ConstantBuffer);
    return m_ConstantBuffer ? true : Fail("Failed to create NRD constant buffer");
}

bool RTXPTNrdIntegration::CreateSamplers(IRenderDevice* pDevice)
{
    const nrd::InstanceDesc* pInstanceDesc = nrd::GetInstanceDesc(*m_Instance);
    if (pInstanceDesc == nullptr)
        return Fail("Failed to query NRD instance description");

    m_Samplers.resize(pInstanceDesc->samplersNum);
    for (Uint32 SamplerIndex = 0; SamplerIndex < pInstanceDesc->samplersNum; ++SamplerIndex)
    {
        const nrd::Sampler NrdSampler = pInstanceDesc->samplers[SamplerIndex];
        if (NrdSampler != nrd::Sampler::NEAREST_CLAMP && NrdSampler != nrd::Sampler::LINEAR_CLAMP)
            return Fail("Unsupported NRD sampler");

        const bool  Linear = NrdSampler == nrd::Sampler::LINEAR_CLAMP;
        SamplerDesc Desc;
        Desc.Name      = Linear ? "RTXPT NRD linear clamp sampler" : "RTXPT NRD nearest clamp sampler";
        Desc.MinFilter = Linear ? FILTER_TYPE_LINEAR : FILTER_TYPE_POINT;
        Desc.MagFilter = Desc.MinFilter;
        Desc.MipFilter = Desc.MinFilter;
        Desc.AddressU  = TEXTURE_ADDRESS_CLAMP;
        Desc.AddressV  = TEXTURE_ADDRESS_CLAMP;
        Desc.AddressW  = TEXTURE_ADDRESS_CLAMP;

        pDevice->CreateSampler(Desc, &m_Samplers[SamplerIndex]);
        if (!m_Samplers[SamplerIndex])
            return Fail("Failed to create NRD sampler");
    }

    return true;
}

bool RTXPTNrdIntegration::CreatePipelines(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache)
{
    const nrd::InstanceDesc* pInstanceDesc = nrd::GetInstanceDesc(*m_Instance);
    if (pInstanceDesc == nullptr)
        return Fail("Failed to query NRD instance description");

    const std::string                              SearchDirs = BuildNrdShaderSearchDirs();
    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    pEngineFactory->CreateDefaultShaderSourceStreamFactory(SearchDirs.c_str(), &pShaderSourceFactory);
    if (!pShaderSourceFactory)
        return Fail("Failed to create NRD shader source factory");

    m_Pipelines.resize(pInstanceDesc->pipelinesNum);
    for (Uint32 PipelineIndex = 0; PipelineIndex < pInstanceDesc->pipelinesNum; ++PipelineIndex)
    {
        const nrd::PipelineDesc& NrdPipeline = pInstanceDesc->pipelines[PipelineIndex];
        ParsedShaderIdentifier   ShaderInfo  = ParseShaderIdentifier(NrdPipeline.shaderIdentifier);
        if (ShaderInfo.FilePath.empty())
            return Fail("NRD pipeline shader identifier is empty");

        ShaderMacroHelper Macros;
        AddNrdRequiredMacros(Macros, *pInstanceDesc);
        for (const auto& Macro : ShaderInfo.Macros)
            Macros.Update(Macro.first.c_str(), Macro.second.c_str());

        ShaderCreateInfo ShaderCI;
        ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
        ShaderCI.Desc.Name                  = ShaderInfo.FilePath.c_str();
        ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
        ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
        ShaderCI.CompileFlags               = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
        ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
        ShaderCI.FilePath                   = ShaderInfo.FilePath.c_str();
        ShaderCI.EntryPoint                 = pInstanceDesc->shaderEntryPoint != nullptr && pInstanceDesc->shaderEntryPoint[0] != '\0' ? pInstanceDesc->shaderEntryPoint : "NRD_CS_MAIN";
        ShaderCI.Macros                     = Macros;
        ShaderCI.pShaderSourceStreamFactory = pShaderSourceFactory;

        RefCntAutoPtr<IShader> pCS;
        pStateCache->CreateShader(ShaderCI, &pCS);
        if (!pCS)
            return Fail("Failed to create NRD compute shader");

        PipelineState& Pipeline = m_Pipelines[PipelineIndex];
#    if RTXPT_HAS_D3D_SHADER_REFLECTION
        RefCntAutoPtr<IShaderD3D> pShaderD3D{pCS, IID_ShaderD3D};
#    endif
        for (Uint32 ResourceIndex = 0; ResourceIndex < pCS->GetResourceCount(); ++ResourceIndex)
        {
            ShaderResourceDesc ResourceDesc;
            pCS->GetResourceDesc(ResourceIndex, ResourceDesc);
            if (ResourceDesc.Name == nullptr || ResourceDesc.Name[0] == '\0')
                return Fail("NRD shader reflected an unnamed resource");

            Uint32 SortKey = ResourceIndex;
#    if RTXPT_HAS_D3D_SHADER_REFLECTION
            if (pShaderD3D)
            {
                HLSLShaderResourceDesc HLSLDesc;
                pShaderD3D->GetHLSLResource(ResourceIndex, HLSLDesc);
                SortKey = HLSLDesc.ShaderRegister;
            }
#    endif

            switch (ResourceDesc.Type)
            {
                case SHADER_RESOURCE_TYPE_CONSTANT_BUFFER:
                    Pipeline.ConstantBufferNames.emplace_back(ResourceDesc.Name);
                    break;
                case SHADER_RESOURCE_TYPE_SAMPLER:
                    Pipeline.SamplerNames.emplace_back(SortKey, ResourceDesc.Name);
                    break;
                case SHADER_RESOURCE_TYPE_TEXTURE_SRV:
                    Pipeline.TextureSRVNames.emplace_back(SortKey, ResourceDesc.Name);
                    break;
                case SHADER_RESOURCE_TYPE_TEXTURE_UAV:
                    Pipeline.TextureUAVNames.emplace_back(SortKey, ResourceDesc.Name);
                    break;
                default:
                    break;
            }
        }
        SortReflectedResources(Pipeline.SamplerNames);
        SortReflectedResources(Pipeline.TextureSRVNames);
        SortReflectedResources(Pipeline.TextureUAVNames);

        ComputePipelineStateCreateInfo PSOCreateInfo;
        PSOCreateInfo.PSODesc.Name         = ShaderInfo.FilePath.c_str();
        PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
        PSOCreateInfo.pCS                  = pCS;

        PipelineResourceLayoutDescX ResourceLayout;
        ResourceLayout.DefaultVariableType   = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

        pStateCache->CreateComputePipelineState(PSOCreateInfo, &Pipeline.PSO);
        if (!Pipeline.PSO)
            return Fail("Failed to create NRD compute PSO");

        Pipeline.PSO->CreateShaderResourceBinding(&Pipeline.SRB, true);
        if (!Pipeline.SRB)
            return Fail("Failed to create NRD shader resource binding");
    }

    return true;
}

bool RTXPTNrdIntegration::CreatePoolTextures(IRenderDevice* pDevice, Uint32 Width, Uint32 Height)
{
    const nrd::InstanceDesc* pInstanceDesc = nrd::GetInstanceDesc(*m_Instance);
    if (pInstanceDesc == nullptr)
        return Fail("Failed to query NRD instance description");

    auto CreatePoolTexture = [&](const nrd::TextureDesc& NrdDesc, const char* PoolName, Uint32 Index, RefCntAutoPtr<ITexture>& Texture) {
        const TEXTURE_FORMAT Format = ToDiligentFormat(NrdDesc.format);
        if (Format == TEX_FORMAT_UNKNOWN)
            return Fail("Unsupported NRD pool texture format");

        std::ostringstream Name;
        Name << "RTXPT NRD " << PoolName << " texture " << Index;
        const std::string TextureName = Name.str();

        const Uint32 DownsampleFactor = std::max(static_cast<Uint32>(NrdDesc.downsampleFactor), 1u);
        TextureDesc  Desc;
        Desc.Name      = TextureName.c_str();
        Desc.Type      = RESOURCE_DIM_TEX_2D;
        Desc.Width     = std::max(DivideUp(Width, DownsampleFactor), 1u);
        Desc.Height    = std::max(DivideUp(Height, DownsampleFactor), 1u);
        Desc.Format    = Format;
        Desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;

        pDevice->CreateTexture(Desc, nullptr, &Texture);
        if (!Texture)
            return Fail("Failed to create NRD pool texture");

        if (Texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) == nullptr ||
            Texture->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS) == nullptr)
        {
            Texture.Release();
            return Fail("Failed to create NRD pool texture views");
        }

        return true;
    };

    m_PermanentTextures.resize(pInstanceDesc->permanentPoolSize);
    for (Uint32 Index = 0; Index < pInstanceDesc->permanentPoolSize; ++Index)
    {
        if (!CreatePoolTexture(pInstanceDesc->permanentPool[Index], "permanent", Index, m_PermanentTextures[Index]))
            return false;
    }

    m_TransientTextures.resize(pInstanceDesc->transientPoolSize);
    for (Uint32 Index = 0; Index < pInstanceDesc->transientPoolSize; ++Index)
    {
        if (!CreatePoolTexture(pInstanceDesc->transientPool[Index], "transient", Index, m_TransientTextures[Index]))
            return false;
    }

    return true;
}

bool RTXPTNrdIntegration::BindDispatchResources(IDeviceContext*,
                                                const nrd::DispatchDesc&    DispatchDesc,
                                                const nrd::PipelineDesc&    PipelineDesc,
                                                PipelineState&              Pipeline,
                                                const RTXPTNrdFrameAttribs& Attribs)
{
    if (!Pipeline.SRB)
        return Fail("NRD dispatch pipeline SRB is not initialized");
    if (DispatchDesc.resourcesNum != 0 && DispatchDesc.resources == nullptr)
        return Fail("NRD dispatch resource list is null");
    if (PipelineDesc.resourceRangesNum != 0 && PipelineDesc.resourceRanges == nullptr)
        return Fail("NRD dispatch resource range list is null");

    const RTXPTRenderTargets& RenderTargets = *Attribs.pRenderTargets;

    auto ResolveExternalView = [&](ITextureView*       pSRV,
                                   ITextureView*       pUAV,
                                   nrd::DescriptorType DescriptorType,
                                   const char*         ResourceName,
                                   ITextureView*&      pView) {
        pView = DescriptorType == nrd::DescriptorType::TEXTURE ? pSRV : pUAV;
        if (pView != nullptr)
            return true;

        std::ostringstream Message;
        Message << "Unavailable NRD " << ResourceName << ' '
                << (DescriptorType == nrd::DescriptorType::TEXTURE ? "SRV" : "UAV");
        return Fail(Message.str().c_str());
    };

    auto ResolvePoolView = [&](const std::vector<RefCntAutoPtr<ITexture>>& Pool,
                               Uint32                                      Index,
                               nrd::DescriptorType                         DescriptorType,
                               const char*                                 PoolName,
                               ITextureView*&                              pView) {
        if (Index >= Pool.size())
        {
            std::ostringstream Message;
            Message << "NRD " << PoolName << " pool resource index is out of range";
            return Fail(Message.str().c_str());
        }

        ITexture* pTexture = Pool[Index];
        if (pTexture == nullptr)
        {
            std::ostringstream Message;
            Message << "NRD " << PoolName << " pool texture is not initialized";
            return Fail(Message.str().c_str());
        }

        pView = pTexture->GetDefaultView(GetDiligentViewType(DescriptorType));
        if (pView != nullptr)
            return true;

        std::ostringstream Message;
        Message << "Unavailable NRD " << PoolName << " pool "
                << (DescriptorType == nrd::DescriptorType::TEXTURE ? "SRV" : "UAV");
        return Fail(Message.str().c_str());
    };

    auto ResolveResourceView = [&](const nrd::ResourceDesc& Resource,
                                   nrd::DescriptorType      DescriptorType,
                                   ITextureView*&           pView) {
        switch (Resource.type)
        {
            case nrd::ResourceType::IN_MV:
                return ResolveExternalView(RenderTargets.GetDenoiserMotionVectorsSRV(), RenderTargets.GetDenoiserMotionVectorsUAV(), DescriptorType, GetNrdResourceTypeName(Resource.type), pView);
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
                return ResolveExternalView(RenderTargets.GetDenoiserNormalRoughnessSRV(), RenderTargets.GetDenoiserNormalRoughnessUAV(), DescriptorType, GetNrdResourceTypeName(Resource.type), pView);
            case nrd::ResourceType::IN_VIEWZ:
                return ResolveExternalView(RenderTargets.GetDenoiserViewspaceZSRV(), RenderTargets.GetDenoiserViewspaceZUAV(), DescriptorType, GetNrdResourceTypeName(Resource.type), pView);
            case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
                return ResolveExternalView(RenderTargets.GetDenoiserSpecRadianceHitDistSRV(), RenderTargets.GetDenoiserSpecRadianceHitDistUAV(), DescriptorType, GetNrdResourceTypeName(Resource.type), pView);
            case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
                return ResolveExternalView(RenderTargets.GetDenoiserDiffRadianceHitDistSRV(), RenderTargets.GetDenoiserDiffRadianceHitDistUAV(), DescriptorType, GetNrdResourceTypeName(Resource.type), pView);
            case nrd::ResourceType::IN_DISOCCLUSION_THRESHOLD_MIX:
                return ResolveExternalView(RenderTargets.GetDenoiserDisocclusionThresholdMixSRV(), RenderTargets.GetDenoiserDisocclusionThresholdMixUAV(), DescriptorType, GetNrdResourceTypeName(Resource.type), pView);
            case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
                return ResolveExternalView(RenderTargets.GetDenoiserOutSpecRadianceHitDistSRV(Attribs.PlaneIndex), RenderTargets.GetDenoiserOutSpecRadianceHitDistUAV(Attribs.PlaneIndex), DescriptorType, GetNrdResourceTypeName(Resource.type), pView);
            case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
                return ResolveExternalView(RenderTargets.GetDenoiserOutDiffRadianceHitDistSRV(Attribs.PlaneIndex), RenderTargets.GetDenoiserOutDiffRadianceHitDistUAV(Attribs.PlaneIndex), DescriptorType, GetNrdResourceTypeName(Resource.type), pView);
            case nrd::ResourceType::OUT_VALIDATION:
                return ResolveExternalView(RenderTargets.GetDenoiserOutValidationSRV(), RenderTargets.GetDenoiserOutValidationUAV(), DescriptorType, GetNrdResourceTypeName(Resource.type), pView);
            case nrd::ResourceType::TRANSIENT_POOL:
                return ResolvePoolView(m_TransientTextures, Resource.indexInPool, DescriptorType, "transient", pView);
            case nrd::ResourceType::PERMANENT_POOL:
                return ResolvePoolView(m_PermanentTextures, Resource.indexInPool, DescriptorType, "permanent", pView);
            default:
            {
                std::ostringstream Message;
                Message << "Unsupported NRD dispatch resource type: " << GetNrdResourceTypeName(Resource.type);
                return Fail(Message.str().c_str());
            }
        }
    };

    auto BindTexture = [&](const std::string& Name, ITextureView* pView) {
        IShaderResourceVariable* pVar = Pipeline.SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name.c_str());
        if (pVar == nullptr)
        {
            std::ostringstream Message;
            Message << "Missing reflected NRD texture variable: " << Name;
            return Fail(Message.str().c_str());
        }
        if (pView == nullptr)
        {
            std::ostringstream Message;
            Message << "Unavailable view for reflected NRD texture variable: " << Name;
            return Fail(Message.str().c_str());
        }

        pVar->Set(pView);
        return true;
    };

    Uint32 ResourceIndex = 0;
    Uint32 SRVNameIndex  = 0;
    Uint32 UAVNameIndex  = 0;

    for (Uint32 RangeIndex = 0; RangeIndex < PipelineDesc.resourceRangesNum; ++RangeIndex)
    {
        const nrd::ResourceRangeDesc& Range = PipelineDesc.resourceRanges[RangeIndex];
        if (Range.descriptorType != nrd::DescriptorType::TEXTURE &&
            Range.descriptorType != nrd::DescriptorType::STORAGE_TEXTURE)
        {
            return Fail("Unsupported NRD dispatch descriptor type");
        }

        std::vector<std::pair<Uint32, std::string>>& Names =
            Range.descriptorType == nrd::DescriptorType::TEXTURE ? Pipeline.TextureSRVNames : Pipeline.TextureUAVNames;
        Uint32& NameIndex = Range.descriptorType == nrd::DescriptorType::TEXTURE ? SRVNameIndex : UAVNameIndex;

        for (Uint32 DescriptorIndex = 0; DescriptorIndex < Range.descriptorsNum; ++DescriptorIndex)
        {
            if (ResourceIndex >= DispatchDesc.resourcesNum)
                return Fail("NRD dispatch resource range exceeds resource list");
            if (NameIndex >= Names.size())
                return Fail("NRD reflected texture resource count does not match dispatch resources");

            const nrd::ResourceDesc& Resource = DispatchDesc.resources[ResourceIndex];
            ITextureView*            pView    = nullptr;
            if (!ResolveResourceView(Resource, Range.descriptorType, pView) ||
                !BindTexture(Names[NameIndex].second, pView))
            {
                return false;
            }

            ++ResourceIndex;
            ++NameIndex;
        }
    }

    if (ResourceIndex != DispatchDesc.resourcesNum)
        return Fail("NRD dispatch resource list has unbound entries");
    if (SRVNameIndex != Pipeline.TextureSRVNames.size() ||
        UAVNameIndex != Pipeline.TextureUAVNames.size())
    {
        return Fail("NRD reflected texture resources have no dispatch mapping");
    }

    return true;
}

void RTXPTNrdIntegration::PopulateCommonSettings(nrd::CommonSettings& Settings, const RTXPTNrdFrameAttribs& Attribs) const
{
    Settings = {};

    const SampleConstants&       FrameConstants = *Attribs.pFrameConstants;
    const RTXPTRenderTargets&    RenderTargets  = *Attribs.pRenderTargets;
    const RTXPTRealtimeSettings& Realtime       = *Attribs.pRealtime;
    const Uint32                 RenderWidth    = RenderTargets.GetRenderWidth();
    const Uint32                 RenderHeight   = RenderTargets.GetRenderHeight();

    CopyMatrixToNrd(Settings.worldToViewMatrix, FrameConstants.view.MatWorldToView);
    CopyMatrixToNrd(Settings.worldToViewMatrixPrev, FrameConstants.previousView.MatWorldToView);
    CopyMatrixToNrd(Settings.viewToClipMatrix, FrameConstants.view.MatViewToClip);
    CopyMatrixToNrd(Settings.viewToClipMatrixPrev, FrameConstants.previousView.MatViewToClip);

    Settings.isMotionVectorInWorldSpace          = false;
    Settings.motionVectorScale[0]                = RenderWidth != 0 ? 1.0f / static_cast<float>(RenderWidth) : 0.0f;
    Settings.motionVectorScale[1]                = RenderHeight != 0 ? 1.0f / static_cast<float>(RenderHeight) : 0.0f;
    Settings.motionVectorScale[2]                = 1.0f;
    Settings.cameraJitter[0]                     = FrameConstants.view.PixelOffset.x;
    Settings.cameraJitter[1]                     = FrameConstants.view.PixelOffset.y;
    Settings.cameraJitterPrev[0]                 = FrameConstants.previousView.PixelOffset.x;
    Settings.cameraJitterPrev[1]                 = FrameConstants.previousView.PixelOffset.y;
    Settings.frameIndex                          = Attribs.FrameIndex;
    Settings.denoisingRange                      = 20000.0f;
    Settings.enableValidation                    = Attribs.EnableValidation && RenderTargets.GetDenoiserOutValidationUAV() != nullptr;
    Settings.disocclusionThreshold               = Realtime.NRDDisocclusionThreshold;
    Settings.disocclusionThresholdAlternate      = Realtime.NRDDisocclusionThresholdAlternate;
    Settings.isDisocclusionThresholdMixAvailable = Realtime.NRDUseAlternateDisocclusionThresholdMix;
    Settings.timeDeltaBetweenFrames              = Attribs.TimeDeltaSeconds;
    Settings.accumulationMode                    = Attribs.ResetHistory ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
    Settings.resourceSize[0]                     = ToNrdDimension(RenderWidth);
    Settings.resourceSize[1]                     = ToNrdDimension(RenderHeight);
    Settings.resourceSizePrev[0]                 = ToNrdDimension(RenderWidth);
    Settings.resourceSizePrev[1]                 = ToNrdDimension(RenderHeight);
    Settings.rectSize[0]                         = ToNrdDimension(RenderWidth);
    Settings.rectSize[1]                         = ToNrdDimension(RenderHeight);
    Settings.rectSizePrev[0]                     = ToNrdDimension(RenderWidth);
    Settings.rectSizePrev[1]                     = ToNrdDimension(RenderHeight);
}

bool RTXPTNrdIntegration::Dispatch(IDeviceContext* pContext, const RTXPTNrdFrameAttribs& Attribs)
{
    m_Stats.LastDispatchExecuted = false;
    m_Stats.LastDispatches       = 0;

    if (!IsReady() || m_Instance == nullptr)
        return Fail("RTXPT NRD integration is not ready");
    if (pContext == nullptr)
        return Fail("RTXPT NRD dispatch requires a device context");
    if (Attribs.pRenderTargets == nullptr || Attribs.pFrameConstants == nullptr || Attribs.pRealtime == nullptr)
        return Fail("RTXPT NRD dispatch requires render targets, frame constants, and realtime settings");
    if (Attribs.PlaneIndex >= kRTXPTDenoisingLayerCount)
        return Fail("RTXPT NRD dispatch plane index is out of range");
    if (Attribs.pRealtime->NRDMethod != m_Stats.Method)
        return Fail("RTXPT NRD dispatch method does not match the initialized denoiser");

    const Uint32 RenderWidth  = Attribs.pRenderTargets->GetRenderWidth();
    const Uint32 RenderHeight = Attribs.pRenderTargets->GetRenderHeight();
    if (RenderWidth == 0 || RenderHeight == 0)
        return Fail("RTXPT NRD dispatch requires a non-zero render size");
    if (RenderWidth != m_Stats.Width || RenderHeight != m_Stats.Height)
        return Fail("RTXPT NRD dispatch render size does not match the initialized denoiser");

    const nrd::InstanceDesc* pInstanceDesc = nrd::GetInstanceDesc(*m_Instance);
    if (pInstanceDesc == nullptr)
        return Fail("Failed to query NRD instance description");

    nrd::Result Result = nrd::Result::SUCCESS;
    if (m_Stats.Method == RTXPTNrdMethod::RELAX)
    {
        nrd::RelaxSettings Settings = RTXPTMakeRelaxSettings(Attribs.pRealtime->RelaxSettings);
        Result                      = nrd::SetDenoiserSettings(*m_Instance, m_Identifier, &Settings);
    }
    else
    {
        nrd::ReblurSettings Settings = RTXPTMakeReblurSettings(Attribs.pRealtime->ReblurSettings);
        Result                       = nrd::SetDenoiserSettings(*m_Instance, m_Identifier, &Settings);
    }
    if (Result != nrd::Result::SUCCESS)
        return Fail("Failed to set NRD denoiser settings");

    nrd::CommonSettings CommonSettings;
    PopulateCommonSettings(CommonSettings, Attribs);
    Result = nrd::SetCommonSettings(*m_Instance, CommonSettings);
    if (Result != nrd::Result::SUCCESS)
        return Fail("Failed to set NRD common settings");

    const nrd::DispatchDesc* pDispatchDescs = nullptr;
    Uint32                   DispatchNum    = 0;
    Result                                  = nrd::GetComputeDispatches(*m_Instance, &m_Identifier, 1, pDispatchDescs, DispatchNum);
    if (Result != nrd::Result::SUCCESS)
        return Fail("Failed to get NRD compute dispatches");
    if (DispatchNum != 0 && pDispatchDescs == nullptr)
        return Fail("NRD returned an empty dispatch list");

    auto BindDeviceObject = [&](PipelineState& Pipeline, const std::string& Name, IDeviceObject* pObject, const char* Kind) {
        IShaderResourceVariable* pVar = Pipeline.SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name.c_str());
        if (pVar == nullptr)
        {
            std::ostringstream Message;
            Message << "Missing reflected NRD " << Kind << " variable: " << Name;
            return Fail(Message.str().c_str());
        }
        if (pObject == nullptr)
        {
            std::ostringstream Message;
            Message << "Unavailable NRD " << Kind << " resource: " << Name;
            return Fail(Message.str().c_str());
        }

        pVar->Set(pObject);
        return true;
    };

    for (Uint32 DispatchIndex = 0; DispatchIndex < DispatchNum; ++DispatchIndex)
    {
        const nrd::DispatchDesc& DispatchDesc = pDispatchDescs[DispatchIndex];
        if (DispatchDesc.pipelineIndex >= m_Pipelines.size() ||
            DispatchDesc.pipelineIndex >= pInstanceDesc->pipelinesNum)
        {
            return Fail("NRD dispatch pipeline index is out of range");
        }

        PipelineState&           Pipeline     = m_Pipelines[DispatchDesc.pipelineIndex];
        const nrd::PipelineDesc& PipelineDesc = pInstanceDesc->pipelines[DispatchDesc.pipelineIndex];
        if (!Pipeline.PSO || !Pipeline.SRB)
            return Fail("NRD dispatch pipeline is not initialized");

        if (DispatchDesc.constantBufferDataSize != 0)
        {
            if (DispatchDesc.constantBufferData == nullptr)
                return Fail("NRD dispatch constant data is null");
            if (!m_ConstantBuffer)
                return Fail("NRD constant buffer is not initialized");
            if (DispatchDesc.constantBufferDataSize > pInstanceDesc->constantBufferMaxDataSize)
                return Fail("NRD dispatch constant data exceeds the constant buffer size");

            MapHelper<Uint8> MappedConstants{pContext, m_ConstantBuffer, MAP_WRITE, MAP_FLAG_DISCARD};
            if (static_cast<Uint8*>(MappedConstants) == nullptr)
                return Fail("Failed to map NRD constant buffer");
            std::memcpy(static_cast<Uint8*>(MappedConstants), DispatchDesc.constantBufferData, DispatchDesc.constantBufferDataSize);
        }

        for (const std::string& ConstantBufferName : Pipeline.ConstantBufferNames)
        {
            if (!BindDeviceObject(Pipeline, ConstantBufferName, m_ConstantBuffer, "constant buffer"))
                return false;
        }

        auto GetSamplerForReflectedName = [&](const std::string& Name, Uint32 FallbackIndex) -> ISampler* {
            nrd::Sampler DesiredSampler = nrd::Sampler::MAX_NUM;
            if (Name == "gNearestClamp")
                DesiredSampler = nrd::Sampler::NEAREST_CLAMP;
            else if (Name == "gLinearClamp")
                DesiredSampler = nrd::Sampler::LINEAR_CLAMP;

            if (DesiredSampler != nrd::Sampler::MAX_NUM)
            {
                for (Uint32 SamplerIndex = 0; SamplerIndex < pInstanceDesc->samplersNum && SamplerIndex < m_Samplers.size(); ++SamplerIndex)
                {
                    if (pInstanceDesc->samplers[SamplerIndex] == DesiredSampler)
                        return m_Samplers[SamplerIndex];
                }
            }

            return FallbackIndex < m_Samplers.size() ? m_Samplers[FallbackIndex] : nullptr;
        };

        for (Uint32 SamplerIndex = 0; SamplerIndex < Pipeline.SamplerNames.size(); ++SamplerIndex)
        {
            const std::string& SamplerName = Pipeline.SamplerNames[SamplerIndex].second;
            ISampler*          pSampler    = GetSamplerForReflectedName(SamplerName, SamplerIndex);
            if (pSampler == nullptr)
                return Fail("NRD reflected sampler count exceeds created sampler count");
            if (!BindDeviceObject(Pipeline, SamplerName, pSampler, "sampler"))
                return false;
        }

        if (!BindDispatchResources(pContext, DispatchDesc, PipelineDesc, Pipeline, Attribs))
            return false;

        pContext->SetPipelineState(Pipeline.PSO);
        pContext->CommitShaderResources(Pipeline.SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        pContext->DispatchCompute(DispatchComputeAttribs{DispatchDesc.gridWidth, DispatchDesc.gridHeight, 1});

        ++m_Stats.DispatchCount;
        ++m_Stats.LastDispatches;
    }

    m_Stats.LastDispatchExecuted = m_Stats.LastDispatches != 0;
    m_Stats.LastPlaneIndex       = Attribs.PlaneIndex;
    return true;
}

#endif

} // namespace Diligent
