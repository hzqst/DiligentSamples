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

#include "RTXPTEnvMapBaker.hpp"
#include "RTXPTSceneJson.hpp"

#include "DebugUtilities.hpp"
#include "FileSystem.hpp"
#include "GraphicsAccessories.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"
#include "ShaderMacroHelper.hpp"
#include "TextureUtilities.h"
#include "PBR_Renderer.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <utility>
#include <vector>

#include "AsyncPrefix.hpp"
#include "RTXPTSharedTexture.hpp"

namespace Diligent
{

namespace
{

constexpr const char* kProceduralSkyPath          = "==PROCEDURAL_SKY==";
constexpr Uint32      kFallbackCubeSize           = 4;
constexpr Uint32      kMinImportanceMapResolution = 16;
constexpr Uint32      kImportanceBakerThreads     = 16;
constexpr Uint32      kImportanceSamplesPerAxis   = 4;

struct EnvMapImportanceBakerConstantsCPU
{
    Uint32 SourceCubeDim                = 0;
    Uint32 SourceCubeMipCount           = 0;
    Uint32 ImportanceMapDim             = 0;
    Uint32 ImportanceMapBaseMip         = 0;
    Uint32 ImportanceMapDimInSamples[2] = {};
    Uint32 ImportanceMapNumSamples[2]   = {};
    float  ImportanceMapInvSamples      = 1.0f;
    Uint32 ReduceSrcMip                 = 0;
    Uint32 ReduceDstMip                 = 0;
    Uint32 _padding0                    = 0;
};
static_assert(sizeof(EnvMapImportanceBakerConstantsCPU) == 48, "EnvMapImportanceBakerConstantsCPU must match EnvMapImportanceBaker.hlsl");

Uint32 MipCountForPowerOfTwo(Uint32 Resolution)
{
    Uint32 Mips = 1;
    while ((Resolution >> Mips) != 0)
        ++Mips;
    return Mips;
}

Uint32 RoundUpPowerOfTwo(Uint32 Value)
{
    Uint32 Result = 1;
    while (Result < Value && Result <= (1u << 30))
        Result <<= 1;
    return Result;
}

Uint32 NormalizeImportanceResolution(Uint32 Resolution)
{
    return RoundUpPowerOfTwo(std::max(kMinImportanceMapResolution, Resolution));
}

Uint32 DispatchGroupsForDim(Uint32 Dim)
{
    return (Dim + kImportanceBakerThreads - 1u) / kImportanceBakerThreads;
}

RefCntAutoPtr<ITextureView> CreateMipView(ITexture* pTexture, TEXTURE_VIEW_TYPE ViewType, Uint32 Mip)
{
    RefCntAutoPtr<ITextureView> View;
    if (pTexture == nullptr)
        return View;

    TextureViewDesc ViewDesc;
    ViewDesc.ViewType        = ViewType;
    ViewDesc.TextureDim      = RESOURCE_DIM_TEX_2D;
    ViewDesc.MostDetailedMip = Mip;
    ViewDesc.NumMipLevels    = 1;
    if (ViewType == TEXTURE_VIEW_UNORDERED_ACCESS)
        ViewDesc.AccessFlags = UAV_ACCESS_FLAG_READ_WRITE;

    pTexture->CreateView(ViewDesc, &View);
    return View;
}

EnvMapImportanceBakerConstantsCPU MakeImportanceConstants(const TextureDesc& SourceDesc,
                                                          Uint32             Resolution,
                                                          Uint32             MipLevels,
                                                          Uint32             ReduceSrcMip,
                                                          Uint32             ReduceDstMip)
{
    EnvMapImportanceBakerConstantsCPU Constants;
    Constants.SourceCubeDim                = SourceDesc.Width;
    Constants.SourceCubeMipCount           = SourceDesc.MipLevels;
    Constants.ImportanceMapDim             = Resolution;
    Constants.ImportanceMapBaseMip         = MipLevels > 0 ? MipLevels - 1u : 0u;
    Constants.ImportanceMapDimInSamples[0] = Resolution * kImportanceSamplesPerAxis;
    Constants.ImportanceMapDimInSamples[1] = Resolution * kImportanceSamplesPerAxis;
    Constants.ImportanceMapNumSamples[0]   = kImportanceSamplesPerAxis;
    Constants.ImportanceMapNumSamples[1]   = kImportanceSamplesPerAxis;
    Constants.ImportanceMapInvSamples      = 1.0f / static_cast<float>(kImportanceSamplesPerAxis * kImportanceSamplesPerAxis);
    Constants.ReduceSrcMip                 = ReduceSrcMip;
    Constants.ReduceDstMip                 = ReduceDstMip;
    return Constants;
}

bool UploadImportanceConstants(IDeviceContext* pContext, IBuffer* pBuffer, const EnvMapImportanceBakerConstantsCPU& Constants)
{
    MapHelper<EnvMapImportanceBakerConstantsCPU> MappedConstants{pContext, pBuffer, MAP_WRITE, MAP_FLAG_DISCARD};
    if (MappedConstants == nullptr)
        return false;

    *MappedConstants = Constants;
    return true;
}

void TransitionTextureRange(IDeviceContext*        pContext,
                            ITexture*              pTexture,
                            RESOURCE_STATE         OldState,
                            RESOURCE_STATE         NewState,
                            Uint32                 FirstMip,
                            Uint32                 MipCount,
                            STATE_TRANSITION_FLAGS Flags = STATE_TRANSITION_FLAG_NONE)
{
    if (pContext == nullptr || pTexture == nullptr)
        return;

    StateTransitionDesc Barrier{pTexture,
                                OldState,
                                NewState,
                                FirstMip,
                                MipCount,
                                0,
                                REMAINING_ARRAY_SLICES,
                                STATE_TRANSITION_TYPE_IMMEDIATE,
                                Flags};
    pContext->TransitionResourceState(Barrier);
}

void TransitionImportanceMaps(IDeviceContext*        pContext,
                              ITexture*              pImportanceMap,
                              ITexture*              pRadianceMap,
                              RESOURCE_STATE         OldState,
                              RESOURCE_STATE         NewState,
                              Uint32                 FirstMip,
                              Uint32                 MipCount,
                              STATE_TRANSITION_FLAGS Flags = STATE_TRANSITION_FLAG_NONE)
{
    TransitionTextureRange(pContext, pImportanceMap, OldState, NewState, FirstMip, MipCount, Flags);
    TransitionTextureRange(pContext, pRadianceMap, OldState, NewState, FirstMip, MipCount, Flags);
}

std::string ToLower(std::string Value)
{
    std::transform(Value.begin(), Value.end(), Value.begin(),
                   [](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
    return Value;
}

bool IsProceduralSkyPath(const std::string& Path)
{
    return Path.empty() || Path == kProceduralSkyPath;
}

bool IsEnvironmentFile(const std::filesystem::path& Path)
{
    const std::string Ext = ToLower(Path.extension().string());
    return Ext == ".hdr" || Ext == ".exr" || Ext == ".dds" || Ext == ".ktx" || Ext == ".ktx2";
}

bool IsEnvironmentSourceLoadError(const std::string& Error)
{
    return Error.rfind("Failed to load RTXPT environment source:", 0) == 0;
}

bool EnvMapSourceChanged(const RTXPTEnvMapSettings& Lhs, const RTXPTEnvMapSettings& Rhs)
{
    return Lhs.SourceRelativePath != Rhs.SourceRelativePath;
}

bool CreateCubeTexture(IRenderDevice* pDevice, const char* Name, Uint32 Size, Uint32 Color, RefCntAutoPtr<ITexture>& Texture, RefCntAutoPtr<ITextureView>& SRV)
{
    Texture.Release();
    SRV.Release();

    TextureDesc Desc;
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    Desc.Name                 = Name;
    Desc.Type                 = RESOURCE_DIM_TEX_CUBE;
    Desc.Usage                = USAGE_IMMUTABLE;
    Desc.BindFlags            = BIND_SHADER_RESOURCE;
    Desc.Format               = TEX_FORMAT_RGBA8_UNORM;
    Desc.Width                = Size;
    Desc.Height               = Size;
    Desc.MipLevels            = 1;
    Desc.ArraySize            = 6;

    std::vector<Uint32>            Data(static_cast<size_t>(Desc.ArraySize) * Size * Size, Color);
    std::vector<TextureSubResData> SubResData(Desc.ArraySize);
    for (Uint32 Face = 0; Face < Desc.ArraySize; ++Face)
    {
        SubResData[Face].pData  = Data.data() + static_cast<size_t>(Face) * Size * Size;
        SubResData[Face].Stride = Uint64{Size} * sizeof(Uint32);
    }
    TextureData InitData{SubResData.data(), static_cast<Uint32>(SubResData.size())};
    pDevice->CreateTexture(Desc, &InitData, &Texture);

    SRV = Texture ? Texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    return SRV != nullptr;
}

bool CreateRGBA8Texture(IRenderDevice* pDevice, const char* Name, Uint32 Color, RefCntAutoPtr<ITexture>& Texture, RefCntAutoPtr<ITextureView>& SRV)
{
    Texture.Release();
    SRV.Release();

    TextureDesc Desc;
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    Desc.Name                 = Name;
    Desc.Type                 = RESOURCE_DIM_TEX_2D;
    Desc.Usage                = USAGE_IMMUTABLE;
    Desc.BindFlags            = BIND_SHADER_RESOURCE;
    Desc.Format               = TEX_FORMAT_RGBA8_UNORM;
    Desc.Width                = 1;
    Desc.Height               = 1;
    Desc.MipLevels            = 1;

    TextureSubResData Subres{&Color, sizeof(Color)};
    TextureData       InitData{&Subres, 1};
    pDevice->CreateTexture(Desc, &InitData, &Texture);

    SRV = Texture ? Texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    return SRV != nullptr;
}

bool CreateR32FloatTexture(IRenderDevice* pDevice, const char* Name, float Value, RefCntAutoPtr<ITexture>& Texture, RefCntAutoPtr<ITextureView>& SRV)
{
    Texture.Release();
    SRV.Release();

    TextureDesc Desc;
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    Desc.Name                 = Name;
    Desc.Type                 = RESOURCE_DIM_TEX_2D;
    Desc.Usage                = USAGE_IMMUTABLE;
    Desc.BindFlags            = BIND_SHADER_RESOURCE;
    Desc.Format               = TEX_FORMAT_R32_FLOAT;
    Desc.Width                = 1;
    Desc.Height               = 1;
    Desc.MipLevels            = 1;

    TextureSubResData Subres{&Value, sizeof(Value)};
    TextureData       InitData{&Subres, 1};
    pDevice->CreateTexture(Desc, &InitData, &Texture);

    SRV = Texture ? Texture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    return SRV != nullptr;
}

} // namespace

RTXPTEnvMapBaker::~RTXPTEnvMapBaker() = default;

void RTXPTEnvMapBaker::Reset()
{
    m_SourceTexture.Release();
    m_SourceSRV.Release();
    m_FallbackEnvironmentMap.Release();
    m_FallbackDiffuseIrradiance.Release();
    m_FallbackImportanceMap.Release();
    m_FallbackRadianceMap.Release();
    m_FallbackBRDFLUT.Release();
    m_ImportanceConstants.Release();
    m_ImportanceMap.Release();
    m_RadianceMap.Release();
    m_EnvironmentMapSRV.Release();
    m_DiffuseIrradianceSRV.Release();
    m_ImportanceMapSRV.Release();
    m_RadianceMapSRV.Release();
    m_BRDFLUTSRV.Release();
    m_EnvironmentSampler.Release();
    m_ImportanceSampler.Release();
    m_IBLPrecompute.reset();
    m_EnvironmentCube.Release();
    m_EnvCubeBakeConstants.Release();
    m_EnvCubeBakePSOCube.Release();
    m_EnvCubeBakeSRBCube.Release();
    m_EnvCubeBakePSOSphere.Release();
    m_EnvCubeBakeSRBSphere.Release();
    m_EnvironmentCubeResolution = 0;
    m_BuildImportanceBasePass.Reset();
    m_ReduceImportanceMipPass.Reset();
    m_Constants         = {};
    m_LightsBakerParams = {};
    m_LastSettings      = {};
    m_Stats             = {};
}

void RTXPTEnvMapBaker::SceneReloaded()
{
    m_LastSettings.SourceRelativePath.clear();
    m_Stats.SourceLoaded    = false;
    m_Stats.ImportanceReady = false;
    m_Stats.SourceName.clear();
    ++m_Stats.Version;
}

bool RTXPTEnvMapBaker::CreateResources(IRenderDevice* pDevice, IDeviceContext*, IEngineFactory*, IRenderStateCache* pStateCache, bool)
{
    if (pDevice == nullptr)
    {
        m_Stats.Ready     = false;
        m_Stats.LastError = "RTXPT EnvMapBaker requires a render device";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    m_pStateCache = pStateCache;

    return CreateSamplers(pDevice) && CreateFallbackTextures(pDevice);
}

bool RTXPTEnvMapBaker::Update(IRenderDevice* pDevice, IDeviceContext* pContext, IEngineFactory* pEngineFactory, const std::string& AssetsRoot, const RTXPTEnvMapSettings& Settings, bool ForceRebuild, bool ComputeSupported)
{
    if (pDevice == nullptr || pContext == nullptr || pEngineFactory == nullptr)
    {
        m_Stats.Ready     = false;
        m_Stats.LastError = "RTXPT EnvMapBaker update requires a render device, device context, and engine factory";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    if (!m_EnvironmentSampler || !m_ImportanceSampler || !m_FallbackEnvironmentMap || !m_FallbackDiffuseIrradiance ||
        !m_FallbackImportanceMap || !m_FallbackRadianceMap || !m_FallbackBRDFLUT)
    {
        if (!CreateResources(pDevice, pContext, pEngineFactory, m_pStateCache, ComputeSupported))
            return false;
    }

    const Uint32 ImportanceResolution = NormalizeImportanceResolution(Settings.ImportanceMapResolution);
    const Uint32 ImportanceMipLevels  = MipCountForPowerOfTwo(ImportanceResolution);
    const bool   SourceChanged        = ForceRebuild || !m_SourceTexture || !m_SourceSRV ||
        !m_EnvironmentMapSRV || !m_DiffuseIrradianceSRV || !m_BRDFLUTSRV ||
        EnvMapSourceChanged(m_LastSettings, Settings);
    bool        UpdateOk = true;
    std::string SourceLoadError;
    if (SourceChanged)
    {
        UpdateOk = LoadSourceTexture(pDevice, AssetsRoot, Settings) &&
            PrecomputeCubemap(pDevice, pContext, pEngineFactory, Settings);
        if (UpdateOk && !IsProceduralSkyPath(Settings.SourceRelativePath) &&
            m_Stats.Procedural && !m_Stats.LastError.empty())
        {
            SourceLoadError = m_Stats.LastError;
        }
    }
    if (SourceLoadError.empty() && !IsProceduralSkyPath(Settings.SourceRelativePath) &&
        m_Stats.Procedural && IsEnvironmentSourceLoadError(m_Stats.LastError))
    {
        SourceLoadError = m_Stats.LastError;
    }

    const bool ImportanceResourceMissing = m_Stats.ImportanceReady && (!m_ImportanceMap || !m_RadianceMap);
    const bool ImportanceChanged         = ForceRebuild || SourceChanged ||
        !m_ImportanceMapSRV || !m_RadianceMapSRV ||
        ImportanceResourceMissing ||
        m_Stats.ImportanceResolution != ImportanceResolution ||
        m_Stats.ImportanceMipLevels != ImportanceMipLevels;
    bool ImportanceOk = true;
    if (UpdateOk && ImportanceChanged)
    {
        ImportanceOk = CreateImportanceMaps(pDevice, pContext, pEngineFactory, Settings, ComputeSupported);
        if (!ImportanceOk)
        {
            const std::string ImportanceError = m_Stats.LastError;
            UseFallbackImportanceMaps(ImportanceResolution);
            if (!SourceLoadError.empty() && !ImportanceError.empty())
                m_Stats.LastError = SourceLoadError + " | " + ImportanceError;
            else if (!ImportanceError.empty())
                m_Stats.LastError = ImportanceError;
            else
                m_Stats.LastError = SourceLoadError;
        }
    }

    if (UpdateOk)
    {
        m_LastSettings = Settings;
        if (SourceChanged || ImportanceChanged)
            ++m_Stats.Version;

        if (ImportanceOk)
        {
            if (!SourceLoadError.empty())
                m_Stats.LastError = SourceLoadError;
            else if (m_Stats.ImportanceReady)
                m_Stats.LastError.clear();
        }
    }

    m_Stats.CompressedOutput = false;
    m_Stats.Ready            = UpdateOk &&
        m_EnvironmentSampler && m_ImportanceSampler &&
        m_EnvironmentMapSRV && m_DiffuseIrradianceSRV && m_BRDFLUTSRV &&
        m_FallbackImportanceMap && m_FallbackRadianceMap &&
        m_ImportanceMapSRV && m_RadianceMapSRV;
    UpdateConstants(Settings);
    return m_Stats.Ready;
}

bool RTXPTEnvMapBaker::InfoGUI(float Indent)
{
    ImGui::Indent(Indent);
    ImGui::Text("Ready: %s", m_Stats.Ready ? "yes" : "no");
    ImGui::Text("Source: %s", m_Stats.SourceName.empty() ? "none" : m_Stats.SourceName.c_str());
    ImGui::Text("Procedural: %s", m_Stats.Procedural ? "yes" : "no");
    ImGui::Text("Cubemap: %u (%u mips)", m_Stats.CubeResolution, m_Stats.CubeMipLevels);
    ImGui::Text("Diffuse irradiance: %s", m_DiffuseIrradianceSRV ? "ready" : "missing");
    ImGui::Text("Importance: %s %u (%u mips)", m_Stats.ImportanceReady ? "ready" : "missing",
                m_Stats.ImportanceResolution, m_Stats.ImportanceMipLevels);
    ImGui::Text("BRDF LUT: %s", m_Stats.BRDFLUTReady ? "ready" : "missing");
    if (!m_Stats.LastError.empty())
        ImGui::TextWrapped("EnvMapBaker error: %s", m_Stats.LastError.c_str());
    ImGui::Unindent(Indent);
    return false;
}

bool RTXPTEnvMapBaker::DebugGUI(float Indent)
{
    ImGui::Indent(Indent);
    ImGui::Text("Version: %llu", static_cast<unsigned long long>(m_Stats.Version));
    ImGui::Text("Compressed output: %s", m_Stats.CompressedOutput ? "yes" : "no");
    ImGui::Text("Environment SRV: %s", m_EnvironmentMapSRV ? "bound" : "missing");
    ImGui::Text("Diffuse irradiance SRV: %s", m_DiffuseIrradianceSRV ? "bound" : "missing");
    ImGui::Text("Importance SRV: %s", m_ImportanceMapSRV ? "bound" : "missing");
    ImGui::Text("Radiance SRV: %s", m_RadianceMapSRV ? "bound" : "missing");
    ImGui::Unindent(Indent);
    return false;
}

std::vector<RTXPTEnvMapSource> RTXPTEnvMapBaker::EnumerateEnvironmentSources(const std::string& AssetsRoot)
{
    std::vector<RTXPTEnvMapSource> Sources;
    Sources.push_back(RTXPTEnvMapSource{RTXPTEnvMapSourceKind::ProceduralSky, "Procedural Sky", kProceduralSkyPath, ""});

    const std::filesystem::path EnvRoot = std::filesystem::path{AssetsRoot} / "EnvironmentMaps";
    std::error_code             Error;
    if (!std::filesystem::is_directory(EnvRoot, Error))
        return Sources;

    for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator{EnvRoot, Error})
    {
        if (Error || !Entry.is_regular_file() || !IsEnvironmentFile(Entry.path()))
            continue;

        RTXPTEnvMapSource Source;
        Source.Kind         = RTXPTEnvMapSourceKind::TextureFile;
        Source.DisplayName  = Entry.path().filename().string();
        Source.RelativePath = std::string{"EnvironmentMaps/"} + Source.DisplayName;
        Source.ResolvedPath = Entry.path().string();
        FileSystem::CorrectSlashes(Source.ResolvedPath);
        Sources.push_back(std::move(Source));
    }

    std::sort(Sources.begin() + 1, Sources.end(),
              [](const RTXPTEnvMapSource& Lhs, const RTXPTEnvMapSource& Rhs) { return Lhs.DisplayName < Rhs.DisplayName; });
    return Sources;
}

RTXPTEnvMapSettings RTXPTEnvMapBaker::MakeSceneDefaultSettings(const RTXPTSceneGraphData& SceneData)
{
    RTXPTEnvMapSettings Settings;
    Settings.SourceRelativePath = kProceduralSkyPath;
    Settings.Enabled            = true;

    for (const RTXPTSceneLightMetadata& Light : SceneData.Lights)
    {
        if (Light.Type != "EnvironmentLight")
            continue;

        Settings.SourceRelativePath = ReadRTXPTOptionalString(Light.RawJson, "path", kProceduralSkyPath);
        float Scale[3]              = {1.0f, 1.0f, 1.0f};
        if (ReadRTXPTFloatArray(Light.RawJson, "radianceScale", Scale, 3))
            Settings.RadianceScale = float3{Scale[0], Scale[1], Scale[2]};

        float Rotation[1] = {0.0f};
        if (ReadRTXPTFloatArray(Light.RawJson, "rotation", Rotation, 1))
            Settings.RotationRadians = Rotation[0];

        return Settings;
    }

    return Settings;
}

bool RTXPTEnvMapBaker::LoadSourceTexture(IRenderDevice* pDevice, const std::string& AssetsRoot, const RTXPTEnvMapSettings& Settings)
{
    if (pDevice == nullptr)
    {
        m_Stats.Ready     = false;
        m_Stats.LastError = "RTXPT EnvMapBaker source loading requires a render device";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    m_SourceTexture.Release();
    m_SourceSRV.Release();
    m_Stats.SourceLoaded = false;
    m_Stats.Procedural   = IsProceduralSkyPath(Settings.SourceRelativePath);
    m_Stats.SourceName   = m_Stats.Procedural ? kProceduralSkyPath : Settings.SourceRelativePath;

    if (IsProceduralSkyPath(Settings.SourceRelativePath))
    {
        const bool Loaded = CreateProceduralSourceTexture(pDevice, Settings);
        if (Loaded)
            m_Stats.LastError.clear();
        return Loaded;
    }

    std::string ResolvedPath = (std::filesystem::path{AssetsRoot} / Settings.SourceRelativePath).string();
    FileSystem::CorrectSlashes(ResolvedPath);
    ResolvedPath = FileSystem::SimplifyPath(ResolvedPath.c_str());

    CreateSharedTextureFromFile(ResolvedPath.c_str(), TextureLoadInfo{"RTXPT environment source"}, pDevice, &m_SourceTexture);
    m_SourceSRV = m_SourceTexture ? m_SourceTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    if (m_SourceTexture && m_SourceSRV)
    {
        m_Stats.SourceLoaded = true;
        m_Stats.Procedural   = false;
        m_Stats.SourceName   = Settings.SourceRelativePath;
        m_Stats.LastError.clear();
        return true;
    }

    const std::string LoadError = "Failed to load RTXPT environment source: " + ResolvedPath;
    m_Stats.LastError           = LoadError;
    LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
    m_Stats.Procedural = true;
    m_Stats.SourceName = kProceduralSkyPath;

    if (!CreateProceduralSourceTexture(pDevice, Settings))
    {
        if (!m_Stats.LastError.empty())
            m_Stats.LastError = LoadError + " | " + m_Stats.LastError;
        else
            m_Stats.LastError = LoadError;
        return false;
    }

    m_Stats.LastError = LoadError;
    return true;
}

bool RTXPTEnvMapBaker::CreateProceduralSourceTexture(IRenderDevice* pDevice, const RTXPTEnvMapSettings&)
{
    if (pDevice == nullptr)
    {
        m_Stats.Ready     = false;
        m_Stats.LastError = "RTXPT EnvMapBaker procedural source creation requires a render device";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    m_SourceTexture.Release();
    m_SourceSRV.Release();
    m_Stats.SourceLoaded      = false;
    constexpr Uint32    Width = 512, Height = 256;
    const float3        HorizonColor{0.48f, 0.58f, 0.68f};
    const float3        ZenithColor{0.05f, 0.08f, 0.14f};
    std::vector<float4> Texels(static_cast<size_t>(Width) * Height);

    for (Uint32 Y = 0; Y < Height; ++Y)
    {
        const float  T = static_cast<float>(Y) / static_cast<float>(Height - 1);
        const float4 RowColor{
            HorizonColor.x + (ZenithColor.x - HorizonColor.x) * T,
            HorizonColor.y + (ZenithColor.y - HorizonColor.y) * T,
            HorizonColor.z + (ZenithColor.z - HorizonColor.z) * T,
            1.0f};
        std::fill_n(Texels.data() + static_cast<size_t>(Y) * Width, Width, RowColor);
    }
    TextureDesc Desc{"RTXPT EnvMapBaker procedural source", RESOURCE_DIM_TEX_2D, Width, Height, 1, TEX_FORMAT_RGBA32_FLOAT,
                     1, 1, USAGE_IMMUTABLE, BIND_SHADER_RESOURCE};
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    TextureSubResData SubRes{Texels.data(), Uint64{Width} * sizeof(float4)};
    TextureData       InitData{&SubRes, 1};
    pDevice->CreateTexture(Desc, &InitData, &m_SourceTexture);
    m_SourceSRV = m_SourceTexture ? m_SourceTexture->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;
    if (!m_SourceTexture || !m_SourceSRV)
    {
        m_Stats.Ready     = false;
        m_Stats.LastError = "Failed to create RTXPT EnvMapBaker procedural source texture";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        m_SourceTexture.Release();
        m_SourceSRV.Release();
        return false;
    }

    m_Stats.SourceLoaded = true;
    m_Stats.Procedural   = true;
    m_Stats.SourceName   = kProceduralSkyPath;
    m_Stats.LastError.clear();
    return true;
}

bool RTXPTEnvMapBaker::PrecomputeCubemap(IRenderDevice* pDevice, IDeviceContext* pContext, IEngineFactory* pEngineFactory, const RTXPTEnvMapSettings&)
{
    if (pDevice == nullptr || pContext == nullptr || m_SourceSRV == nullptr)
    {
        m_Stats.Ready     = false;
        m_Stats.LastError = "RTXPT EnvMapBaker precompute requires a render device, device context, and source texture";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    if (!m_IBLPrecompute)
        m_IBLPrecompute = std::make_unique<PBR_Renderer>(pDevice, nullptr, pContext, PBR_Renderer::CreateInfo{});

    auto pIrradianceCube    = m_IBLPrecompute->CreateIrradianceCube(pContext, "RTXPT irradiance cube map");
    auto pPrefilteredEnvMap = m_IBLPrecompute->CreatePrefilteredEnvMap(pContext, "RTXPT prefiltered environment map");
    if (!pIrradianceCube || !pPrefilteredEnvMap)
    {
        m_Stats.Ready     = false;
        m_Stats.LastError = "Failed to create RTXPT IBL cubemaps";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    PBR_Renderer::PrecomputeCubemapsAttribs Attribs;
    Attribs.pEnvironmentMapSRV = m_SourceSRV;
    Attribs.pIrradianceCube    = pIrradianceCube;
    Attribs.pPrefilteredEnvMap = pPrefilteredEnvMap;
    m_IBLPrecompute->PrecomputeCubemaps(pContext, Attribs);
    m_EnvironmentMapSRV    = pPrefilteredEnvMap->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    m_DiffuseIrradianceSRV = pIrradianceCube->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    m_BRDFLUTSRV           = m_IBLPrecompute->GetPreintegratedGGX_SRV();

    if (!m_EnvironmentMapSRV || !m_DiffuseIrradianceSRV || !m_BRDFLUTSRV)
    {
        m_Stats.Ready     = false;
        m_Stats.LastError = "RTXPT EnvMapBaker precompute did not produce all IBL textures";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    // Replace the low-resolution GGX-prefiltered IBL cube with a high-resolution environment cube baked
    // straight from the source. The prefiltered cube (kept above only for the IBL irradiance/BRDF
    // products) is PrefilteredEnvMapDim (256) and washes out fine detail seen through small apertures
    // such as windows; upstream RTXPT bakes a high-res environment cube instead. On failure the
    // prefiltered cube remains bound so rendering still works (just lower resolution).
    if (BakeHighResEnvironmentCube(pDevice, pContext, pEngineFactory))
    {
        m_EnvironmentMapSRV = m_EnvironmentCube->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    }
    else
    {
        LOG_WARNING_MESSAGE("RTXPT EnvMapBaker high-resolution environment cube bake failed; "
                            "falling back to the low-resolution prefiltered environment cube");
    }

    const ITexture* pEnvTexture = m_EnvironmentMapSRV->GetTexture();
    if (pEnvTexture == nullptr)
    {
        m_Stats.Ready     = false;
        m_Stats.LastError = "RTXPT EnvMapBaker precompute produced an invalid environment texture";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    const TextureDesc& EnvDesc = pEnvTexture->GetDesc();
    m_Stats.CubeResolution     = EnvDesc.Width;
    m_Stats.CubeMipLevels      = EnvDesc.MipLevels;
    m_Stats.BRDFLUTReady       = true;
    m_Stats.ImportanceReady    = false;
    m_Stats.CompressedOutput   = false;
    return true;
}

bool RTXPTEnvMapBaker::EnsureEnvCubeBakePipeline(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, bool SourceIsCube)
{
    RefCntAutoPtr<IPipelineState>&         PSO = SourceIsCube ? m_EnvCubeBakePSOCube : m_EnvCubeBakePSOSphere;
    RefCntAutoPtr<IShaderResourceBinding>& SRB = SourceIsCube ? m_EnvCubeBakeSRBCube : m_EnvCubeBakeSRBSphere;
    if (PSO && SRB)
        return true;
    if (pDevice == nullptr || pEngineFactory == nullptr)
        return false;

    RefCntAutoPtr<IShaderSourceInputStreamFactory> pShaderSourceFactory;
    pEngineFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders\\PathTracer;shaders\\PathTracer\\Lighting", &pShaderSourceFactory);
    if (!pShaderSourceFactory)
        return false;

    ShaderMacroHelper Macros;
    if (SourceIsCube)
        Macros.Add("ENV_BAKE_SOURCE_CUBE", 1);

    ShaderCreateInfo ShaderCI;
    ShaderCI.SourceLanguage                  = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler                  = SHADER_COMPILER_DXC;
    ShaderCI.ShaderOptimizationLevel         = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.CompileFlags                    = SHADER_COMPILE_FLAG_PACK_MATRIX_ROW_MAJOR;
    ShaderCI.Desc.UseCombinedTextureSamplers = false;
    ShaderCI.pShaderSourceStreamFactory      = pShaderSourceFactory;
    ShaderCI.FilePath                        = "PathTracer/Lighting/EnvMapImportanceBaker.hlsl";
    ShaderCI.Macros                          = Macros;

    RefCntAutoPtr<IShader> pVS;
    ShaderCI.Desc.ShaderType = SHADER_TYPE_VERTEX;
    ShaderCI.Desc.Name       = "RTXPT EnvCube bake VS";
    ShaderCI.EntryPoint      = "EnvCubeBakeVS";
    pDevice->CreateShader(ShaderCI, &pVS);

    RefCntAutoPtr<IShader> pPS;
    ShaderCI.Desc.ShaderType = SHADER_TYPE_PIXEL;
    ShaderCI.Desc.Name       = "RTXPT EnvCube bake PS";
    ShaderCI.EntryPoint      = "SampleEnvToCubePS";
    pDevice->CreateShader(ShaderCI, &pPS);

    if (!pVS || !pPS)
        return false;

    GraphicsPipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name                                  = SourceIsCube ? "RTXPT EnvCube bake PSO (cube)" : "RTXPT EnvCube bake PSO (sphere)";
    PSOCreateInfo.PSODesc.PipelineType                          = PIPELINE_TYPE_GRAPHICS;
    PSOCreateInfo.GraphicsPipeline.NumRenderTargets             = 1;
    PSOCreateInfo.GraphicsPipeline.RTVFormats[0]                = TEX_FORMAT_RGBA16_FLOAT;
    PSOCreateInfo.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    PSOCreateInfo.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    PSOCreateInfo.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;
    PSOCreateInfo.pVS                                           = pVS;
    PSOCreateInfo.pPS                                           = pPS;

    SamplerDesc LinearClamp;
    LinearClamp.MinFilter = FILTER_TYPE_LINEAR;
    LinearClamp.MagFilter = FILTER_TYPE_LINEAR;
    LinearClamp.MipFilter = FILTER_TYPE_LINEAR;
    LinearClamp.AddressU  = TEXTURE_ADDRESS_CLAMP;
    LinearClamp.AddressV  = TEXTURE_ADDRESS_CLAMP;
    LinearClamp.AddressW  = TEXTURE_ADDRESS_CLAMP;

    PipelineResourceLayoutDescX ResourceLayout;
    ResourceLayout
        .AddVariable(SHADER_TYPE_VERTEX, "cbEnvCubeBake", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddVariable(SHADER_TYPE_PIXEL, "g_EnvBakeSource", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC)
        .AddImmutableSampler(SHADER_TYPE_PIXEL, "g_EnvBakeSourceSampler", LinearClamp);
    PSOCreateInfo.PSODesc.ResourceLayout = ResourceLayout;

    pDevice->CreateGraphicsPipelineState(PSOCreateInfo, &PSO);
    if (!PSO)
        return false;
    PSO->CreateShaderResourceBinding(&SRB, true);
    return SRB != nullptr;
}

bool RTXPTEnvMapBaker::BakeHighResEnvironmentCube(IRenderDevice* pDevice, IDeviceContext* pContext, IEngineFactory* pEngineFactory)
{
    if (pDevice == nullptr || pContext == nullptr || pEngineFactory == nullptr || m_SourceSRV == nullptr || m_SourceTexture == nullptr)
        return false;

    const bool SourceIsCube = m_SourceTexture->GetDesc().IsCube();
    if (!EnsureEnvCubeBakePipeline(pDevice, pEngineFactory, SourceIsCube))
        return false;

    IPipelineState*         pPSO = SourceIsCube ? m_EnvCubeBakePSOCube : m_EnvCubeBakePSOSphere;
    IShaderResourceBinding* pSRB = SourceIsCube ? m_EnvCubeBakeSRBCube : m_EnvCubeBakeSRBSphere;
    if (pPSO == nullptr || pSRB == nullptr)
        return false;

    // Upstream RTXPT EnvMapBaker bakes a 2048-texel environment cube (1024 for the procedural sky); we
    // use a fixed 2048 for every source so the cube texture -- and therefore the SRV the path tracer
    // binds to t_EnvironmentMap as a STATIC resource -- is created once and reused across re-bakes
    // (e.g. runtime environment swaps), only its contents are re-rendered. This is still 8x the
    // resolution of the 256-texel prefiltered IBL cube it replaces, which washed out detail seen
    // through small apertures such as windows.
    const Uint32 TargetRes = 2048u;

    if (!m_EnvironmentCube || m_EnvironmentCubeResolution != TargetRes)
    {
        m_EnvironmentCube.Release();

        TextureDesc CubeDesc;
        CubeDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
        CubeDesc.Name                 = "RTXPT high-resolution environment cube";
        CubeDesc.Type                 = RESOURCE_DIM_TEX_CUBE;
        CubeDesc.Width                = TargetRes;
        CubeDesc.Height               = TargetRes;
        CubeDesc.ArraySize            = 6;
        CubeDesc.Format               = TEX_FORMAT_RGBA16_FLOAT;
        CubeDesc.MipLevels            = ComputeMipLevelsCount(TargetRes, TargetRes);
        CubeDesc.BindFlags            = BIND_SHADER_RESOURCE | BIND_RENDER_TARGET;
        CubeDesc.MiscFlags            = MISC_TEXTURE_FLAG_GENERATE_MIPS;
        CubeDesc.Usage                = USAGE_DEFAULT;
        pDevice->CreateTexture(CubeDesc, nullptr, &m_EnvironmentCube);
        if (!m_EnvironmentCube)
        {
            m_EnvironmentCubeResolution = 0;
            return false;
        }
        m_EnvironmentCubeResolution = TargetRes;
    }

    if (!m_EnvCubeBakeConstants)
    {
        BufferDesc CBDesc;
        CBDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
        CBDesc.Name                 = "RTXPT EnvCube bake constants";
        CBDesc.Size                 = sizeof(float4x4);
        CBDesc.BindFlags            = BIND_UNIFORM_BUFFER;
        CBDesc.Usage                = USAGE_DYNAMIC;
        CBDesc.CPUAccessFlags       = CPU_ACCESS_WRITE;
        pDevice->CreateBuffer(CBDesc, nullptr, &m_EnvCubeBakeConstants);
        if (!m_EnvCubeBakeConstants)
            return false;
    }

    // Same per-face rotations as DiligentFX PBR_Renderer::PrecomputeCubemaps so the baked cube keeps
    // the orientation the path tracer already expects.
    const std::array<float4x4, 6> FaceRotations =
        {
            float4x4::RotationY(-PI_F / 2.f), // +X
            float4x4::RotationY(+PI_F / 2.f), // -X
            float4x4::RotationX(+PI_F / 2.f), // +Y
            float4x4::RotationX(-PI_F / 2.f), // -Y
            float4x4::Identity(),             // +Z
            float4x4::RotationY(-PI_F)        // -Z
        };

    if (IShaderResourceVariable* pSourceVar = pSRB->GetVariableByName(SHADER_TYPE_PIXEL, "g_EnvBakeSource"))
        pSourceVar->Set(m_SourceSRV);
    if (IShaderResourceVariable* pRotationVar = pSRB->GetVariableByName(SHADER_TYPE_VERTEX, "cbEnvCubeBake"))
        pRotationVar->Set(m_EnvCubeBakeConstants);

    pContext->SetPipelineState(pPSO);

    for (Uint32 Face = 0; Face < 6; ++Face)
    {
        {
            MapHelper<float4x4> MappedRotation{pContext, m_EnvCubeBakeConstants, MAP_WRITE, MAP_FLAG_DISCARD};
            if (MappedRotation == nullptr)
                return false;
            *MappedRotation = FaceRotations[Face];
        }

        TextureViewDesc RTVDesc;
        RTVDesc.ViewType        = TEXTURE_VIEW_RENDER_TARGET;
        RTVDesc.TextureDim      = RESOURCE_DIM_TEX_2D_ARRAY;
        RTVDesc.MostDetailedMip = 0;
        RTVDesc.FirstArraySlice = Face;
        RTVDesc.NumArraySlices  = 1;
        RefCntAutoPtr<ITextureView> pRTV;
        m_EnvironmentCube->CreateView(RTVDesc, &pRTV);
        if (!pRTV)
            return false;

        ITextureView* ppRTVs[] = {pRTV};
        pContext->SetRenderTargets(1, ppRTVs, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        pContext->CommitShaderResources(pSRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        pContext->Draw(DrawAttribs{4, DRAW_FLAG_VERIFY_ALL});
    }

    pContext->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    pContext->GenerateMips(m_EnvironmentCube->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    return true;
}

bool RTXPTEnvMapBaker::CreateImportanceMaps(IRenderDevice* pDevice, IDeviceContext* pContext, IEngineFactory* pEngineFactory, const RTXPTEnvMapSettings& Settings, bool ComputeSupported)
{
    auto Fail = [this](const char* Error) {
        m_Stats.Ready           = false;
        m_Stats.ImportanceReady = false;
        m_Stats.LastError       = Error;
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    };

    m_Stats.ImportanceReady = false;

    if (!ComputeSupported)
        return Fail("RTXPT EnvMapBaker importance generation requires compute shader support");
    if (pDevice == nullptr || pContext == nullptr || pEngineFactory == nullptr)
        return Fail("RTXPT EnvMapBaker importance generation requires a render device, device context, and engine factory");
    if (!m_Stats.BRDFLUTReady || !m_EnvironmentMapSRV || m_EnvironmentMapSRV->GetTexture() == nullptr)
        return Fail("RTXPT EnvMapBaker importance generation requires a baked environment cubemap");

    const TextureDesc& EnvDesc = m_EnvironmentMapSRV->GetTexture()->GetDesc();
    if (EnvDesc.Width == 0 || EnvDesc.Height == 0 || EnvDesc.MipLevels == 0)
        return Fail("RTXPT EnvMapBaker baked environment cubemap is invalid");

    const Uint32 Resolution = NormalizeImportanceResolution(Settings.ImportanceMapResolution);

    RefCntAutoPtr<IBuffer> NewImportanceConstants;
    BufferDesc             ConstantsDesc;
    ConstantsDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    ConstantsDesc.Name                 = "RTXPT EnvMapBaker importance constants";
    ConstantsDesc.Size                 = sizeof(EnvMapImportanceBakerConstantsCPU);
    ConstantsDesc.BindFlags            = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage                = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags       = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(ConstantsDesc, nullptr, &NewImportanceConstants);
    if (!NewImportanceConstants)
        return Fail("Failed to create RTXPT EnvMapBaker importance constants buffer");
    m_ImportanceConstants = NewImportanceConstants;

    if (!m_BuildImportanceBasePass.Initialize(pDevice, pEngineFactory, m_pStateCache,
                                              "RTXPT EnvMapBaker build importance base", "BuildImportanceBaseCS"))
        return Fail("Failed to initialize RTXPT EnvMapBaker base importance pass");
    if (!m_ReduceImportanceMipPass.Initialize(pDevice, pEngineFactory, m_pStateCache,
                                              "RTXPT EnvMapBaker reduce importance mip", "ReduceImportanceMipCS"))
        return Fail("Failed to initialize RTXPT EnvMapBaker importance mip reduction pass");

    if (!CreateImportanceTextures(pDevice, Resolution))
        return false;
    TransitionImportanceMaps(pContext,
                             m_ImportanceMap,
                             m_RadianceMap,
                             RESOURCE_STATE_UNDEFINED,
                             RESOURCE_STATE_UNORDERED_ACCESS,
                             0,
                             REMAINING_MIP_LEVELS,
                             STATE_TRANSITION_FLAG_UPDATE_STATE | STATE_TRANSITION_FLAG_DISCARD_CONTENT);
    if (!DispatchImportanceBuild(pContext, Resolution))
        return false;
    if (!DispatchImportanceReduce(pContext, Resolution, m_Stats.ImportanceMipLevels))
        return false;

    m_Stats.ImportanceReady = true;
    return true;
}

bool RTXPTEnvMapBaker::CreateImportanceTextures(IRenderDevice* pDevice, Uint32 Resolution)
{
    if (pDevice == nullptr)
    {
        m_Stats.Ready           = false;
        m_Stats.ImportanceReady = false;
        m_Stats.LastError       = "RTXPT EnvMapBaker importance texture creation requires a render device";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    const Uint32 SafeResolution = NormalizeImportanceResolution(Resolution);
    const Uint32 MipLevels      = MipCountForPowerOfTwo(SafeResolution);

    TextureDesc ImportanceDesc;
    ImportanceDesc.ImmediateContextMask = kRTXPTImmediateContextMask;
    ImportanceDesc.Name                 = "RTXPT EnvMapBaker importance map";
    ImportanceDesc.Type                 = RESOURCE_DIM_TEX_2D;
    ImportanceDesc.Usage                = USAGE_DEFAULT;
    ImportanceDesc.BindFlags            = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    ImportanceDesc.Format               = TEX_FORMAT_R32_FLOAT;
    ImportanceDesc.Width                = SafeResolution;
    ImportanceDesc.Height               = SafeResolution;
    ImportanceDesc.MipLevels            = MipLevels;

    RefCntAutoPtr<ITexture>     ImportanceMap;
    RefCntAutoPtr<ITextureView> ImportanceSRV;
    pDevice->CreateTexture(ImportanceDesc, nullptr, &ImportanceMap);
    ImportanceSRV = ImportanceMap ? ImportanceMap->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;

    TextureDesc RadianceDesc = ImportanceDesc;
    RadianceDesc.Name        = "RTXPT EnvMapBaker radiance map";
    RadianceDesc.Format      = TEX_FORMAT_RGBA16_FLOAT;

    RefCntAutoPtr<ITexture>     RadianceMap;
    RefCntAutoPtr<ITextureView> RadianceSRV;
    pDevice->CreateTexture(RadianceDesc, nullptr, &RadianceMap);
    RadianceSRV = RadianceMap ? RadianceMap->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) : nullptr;

    if (!ImportanceMap || !ImportanceSRV || !RadianceMap || !RadianceSRV)
    {
        m_Stats.Ready           = false;
        m_Stats.ImportanceReady = false;
        m_Stats.LastError       = "Failed to create RTXPT EnvMapBaker importance and radiance maps";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    m_ImportanceMapSRV = ImportanceSRV;
    m_RadianceMapSRV   = RadianceSRV;
    m_ImportanceMap    = ImportanceMap;
    m_RadianceMap      = RadianceMap;

    m_Stats.ImportanceResolution = SafeResolution;
    m_Stats.ImportanceMipLevels  = MipLevels;
    return true;
}

bool RTXPTEnvMapBaker::DispatchImportanceBuild(IDeviceContext* pContext, Uint32 Resolution)
{
    if (pContext == nullptr || !m_ImportanceConstants || !m_EnvironmentMapSRV || !m_EnvironmentSampler ||
        !m_ImportanceMap || !m_RadianceMap || m_Stats.ImportanceMipLevels == 0 || Resolution == 0)
    {
        m_Stats.Ready           = false;
        m_Stats.ImportanceReady = false;
        m_Stats.LastError       = "RTXPT EnvMapBaker base importance dispatch is missing required resources";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    ITexture* const pSourceCube = m_EnvironmentMapSRV->GetTexture();
    if (pSourceCube == nullptr)
    {
        m_Stats.Ready           = false;
        m_Stats.ImportanceReady = false;
        m_Stats.LastError       = "RTXPT EnvMapBaker base importance dispatch requires a baked environment cubemap";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    RefCntAutoPtr<ITextureView> ImportanceMip0UAV = CreateMipView(m_ImportanceMap, TEXTURE_VIEW_UNORDERED_ACCESS, 0);
    RefCntAutoPtr<ITextureView> RadianceMip0UAV   = CreateMipView(m_RadianceMap, TEXTURE_VIEW_UNORDERED_ACCESS, 0);
    if (!ImportanceMip0UAV || !RadianceMip0UAV)
    {
        m_Stats.Ready           = false;
        m_Stats.ImportanceReady = false;
        m_Stats.LastError       = "Failed to create RTXPT EnvMapBaker base importance UAV views";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    const EnvMapImportanceBakerConstantsCPU Constants =
        MakeImportanceConstants(pSourceCube->GetDesc(), Resolution, m_Stats.ImportanceMipLevels, 0, 0);
    if (!UploadImportanceConstants(pContext, m_ImportanceConstants, Constants))
    {
        m_Stats.Ready           = false;
        m_Stats.ImportanceReady = false;
        m_Stats.LastError       = "Failed to upload RTXPT EnvMapBaker base importance constants";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    if (!m_BuildImportanceBasePass.Bind(m_ImportanceConstants, m_EnvironmentMapSRV, nullptr, nullptr,
                                        ImportanceMip0UAV, RadianceMip0UAV, m_EnvironmentSampler) ||
        !m_BuildImportanceBasePass.Dispatch(pContext, DispatchGroupsForDim(Resolution), DispatchGroupsForDim(Resolution)))
    {
        m_Stats.Ready           = false;
        m_Stats.ImportanceReady = false;
        m_Stats.LastError       = "Failed to dispatch RTXPT EnvMapBaker base importance pass";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    return true;
}

bool RTXPTEnvMapBaker::DispatchImportanceReduce(IDeviceContext* pContext, Uint32 Resolution, Uint32 MipLevels)
{
    if (pContext == nullptr || !m_ImportanceConstants || !m_EnvironmentMapSRV || !m_EnvironmentSampler ||
        !m_ImportanceMap || !m_RadianceMap || MipLevels == 0 || Resolution == 0)
    {
        m_Stats.Ready           = false;
        m_Stats.ImportanceReady = false;
        m_Stats.LastError       = "RTXPT EnvMapBaker importance reduction is missing required resources";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    ITexture* const pSourceCube = m_EnvironmentMapSRV->GetTexture();
    if (pSourceCube == nullptr)
    {
        m_Stats.Ready           = false;
        m_Stats.ImportanceReady = false;
        m_Stats.LastError       = "RTXPT EnvMapBaker importance reduction requires a baked environment cubemap";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    for (Uint32 DstMip = 1; DstMip < MipLevels; ++DstMip)
    {
        TransitionImportanceMaps(pContext,
                                 m_ImportanceMap,
                                 m_RadianceMap,
                                 RESOURCE_STATE_UNORDERED_ACCESS,
                                 RESOURCE_STATE_SHADER_RESOURCE,
                                 DstMip - 1u,
                                 1);

        RefCntAutoPtr<ITextureView> SrcImportanceSRV = CreateMipView(m_ImportanceMap, TEXTURE_VIEW_SHADER_RESOURCE, DstMip - 1u);
        RefCntAutoPtr<ITextureView> SrcRadianceSRV   = CreateMipView(m_RadianceMap, TEXTURE_VIEW_SHADER_RESOURCE, DstMip - 1u);
        RefCntAutoPtr<ITextureView> ImportanceUAV    = CreateMipView(m_ImportanceMap, TEXTURE_VIEW_UNORDERED_ACCESS, DstMip);
        RefCntAutoPtr<ITextureView> RadianceUAV      = CreateMipView(m_RadianceMap, TEXTURE_VIEW_UNORDERED_ACCESS, DstMip);
        if (!SrcImportanceSRV || !SrcRadianceSRV || !ImportanceUAV || !RadianceUAV)
        {
            m_Stats.Ready           = false;
            m_Stats.ImportanceReady = false;
            m_Stats.LastError       = "Failed to create RTXPT EnvMapBaker importance reduction views";
            LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
            return false;
        }

        const EnvMapImportanceBakerConstantsCPU Constants =
            MakeImportanceConstants(pSourceCube->GetDesc(), Resolution, MipLevels, DstMip - 1u, DstMip);
        if (!UploadImportanceConstants(pContext, m_ImportanceConstants, Constants))
        {
            m_Stats.Ready           = false;
            m_Stats.ImportanceReady = false;
            m_Stats.LastError       = "Failed to upload RTXPT EnvMapBaker importance reduction constants";
            LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
            return false;
        }

        const Uint32 MipDim = std::max(1u, Resolution >> DstMip);
        if (!m_ReduceImportanceMipPass.Bind(m_ImportanceConstants, m_EnvironmentMapSRV, SrcImportanceSRV, SrcRadianceSRV,
                                            ImportanceUAV, RadianceUAV, m_EnvironmentSampler) ||
            !m_ReduceImportanceMipPass.Dispatch(pContext, DispatchGroupsForDim(MipDim), DispatchGroupsForDim(MipDim), RESOURCE_STATE_TRANSITION_MODE_NONE))
        {
            m_Stats.Ready           = false;
            m_Stats.ImportanceReady = false;
            m_Stats.LastError       = "Failed to dispatch RTXPT EnvMapBaker importance reduction pass";
            LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
            return false;
        }
    }

    TransitionImportanceMaps(pContext,
                             m_ImportanceMap,
                             m_RadianceMap,
                             RESOURCE_STATE_UNORDERED_ACCESS,
                             RESOURCE_STATE_SHADER_RESOURCE,
                             MipLevels - 1u,
                             1,
                             STATE_TRANSITION_FLAG_UPDATE_STATE);

    return true;
}

void RTXPTEnvMapBaker::UseFallbackImportanceMaps(Uint32 RequestedResolution)
{
    m_ImportanceMap.Release();
    m_RadianceMap.Release();

    m_ImportanceMapSRV = m_FallbackImportanceMap ?
        m_FallbackImportanceMap->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) :
        nullptr;
    m_RadianceMapSRV = m_FallbackRadianceMap ?
        m_FallbackRadianceMap->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE) :
        nullptr;

    const Uint32 NormalizedResolution = NormalizeImportanceResolution(RequestedResolution);
    m_Stats.ImportanceReady           = false;
    m_Stats.ImportanceResolution      = NormalizedResolution;
    m_Stats.ImportanceMipLevels       = MipCountForPowerOfTwo(NormalizedResolution);
}

bool RTXPTEnvMapBaker::CreateFallbackTextures(IRenderDevice* pDevice)
{
    if (pDevice == nullptr)
    {
        m_Stats.Ready     = false;
        m_Stats.LastError = "RTXPT EnvMapBaker fallback texture creation requires a render device";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    constexpr Uint32 BlackRGBA = 0xff000000u;
    constexpr Uint32 WhiteRGBA = 0xffffffffu;

    m_SourceTexture.Release();
    m_SourceSRV.Release();

    const bool EnvOk        = CreateCubeTexture(pDevice, "RTXPT EnvMapBaker fallback environment cube",
                                         kFallbackCubeSize, BlackRGBA, m_FallbackEnvironmentMap, m_EnvironmentMapSRV);
    const bool DiffuseOk    = CreateCubeTexture(pDevice, "RTXPT EnvMapBaker fallback diffuse irradiance cube",
                                             kFallbackCubeSize, BlackRGBA, m_FallbackDiffuseIrradiance, m_DiffuseIrradianceSRV);
    const bool ImportanceOk = CreateR32FloatTexture(pDevice, "RTXPT EnvMapBaker fallback importance map",
                                                    1.0f, m_FallbackImportanceMap, m_ImportanceMapSRV);
    const bool RadianceOk   = CreateRGBA8Texture(pDevice, "RTXPT EnvMapBaker fallback radiance map",
                                               BlackRGBA, m_FallbackRadianceMap, m_RadianceMapSRV);
    const bool BRDFOk       = CreateRGBA8Texture(pDevice, "RTXPT EnvMapBaker fallback BRDF LUT",
                                           WhiteRGBA, m_FallbackBRDFLUT, m_BRDFLUTSRV);

    m_SourceSRV   = m_EnvironmentMapSRV;
    m_Stats.Ready = m_EnvironmentSampler && m_ImportanceSampler &&
        EnvOk && DiffuseOk && ImportanceOk && RadianceOk && BRDFOk;
    m_Stats.SourceLoaded         = false;
    m_Stats.Procedural           = true;
    m_Stats.ImportanceReady      = false;
    m_Stats.BRDFLUTReady         = false;
    m_Stats.CompressedOutput     = false;
    m_Stats.CubeResolution       = kFallbackCubeSize;
    m_Stats.CubeMipLevels        = 1;
    m_Stats.ImportanceResolution = 1;
    m_Stats.ImportanceMipLevels  = 1;
    m_Stats.SourceName           = kProceduralSkyPath;

    if (!m_Stats.Ready)
    {
        m_Stats.LastError = "Failed to create RTXPT EnvMapBaker fallback textures";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    m_Stats.LastError.clear();
    UpdateConstants(m_LastSettings);
    return true;
}

bool RTXPTEnvMapBaker::CreateSamplers(IRenderDevice* pDevice)
{
    if (pDevice == nullptr)
    {
        m_Stats.LastError = "RTXPT EnvMapBaker sampler creation requires a render device";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    m_EnvironmentSampler.Release();
    m_ImportanceSampler.Release();

    SamplerDesc LinearWrap;
    LinearWrap.Name      = "RTXPT environment map sampler";
    LinearWrap.MinFilter = FILTER_TYPE_LINEAR;
    LinearWrap.MagFilter = FILTER_TYPE_LINEAR;
    LinearWrap.MipFilter = FILTER_TYPE_LINEAR;
    LinearWrap.AddressU  = TEXTURE_ADDRESS_WRAP;
    LinearWrap.AddressV  = TEXTURE_ADDRESS_WRAP;
    LinearWrap.AddressW  = TEXTURE_ADDRESS_WRAP;
    pDevice->CreateSampler(LinearWrap, &m_EnvironmentSampler);

    SamplerDesc PointClamp;
    PointClamp.Name      = "RTXPT environment importance sampler";
    PointClamp.MinFilter = FILTER_TYPE_POINT;
    PointClamp.MagFilter = FILTER_TYPE_POINT;
    PointClamp.MipFilter = FILTER_TYPE_POINT;
    PointClamp.AddressU  = TEXTURE_ADDRESS_CLAMP;
    PointClamp.AddressV  = TEXTURE_ADDRESS_CLAMP;
    PointClamp.AddressW  = TEXTURE_ADDRESS_CLAMP;
    pDevice->CreateSampler(PointClamp, &m_ImportanceSampler);

    if (!m_EnvironmentSampler || !m_ImportanceSampler)
    {
        m_Stats.LastError = "Failed to create RTXPT EnvMapBaker samplers";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    return true;
}

void RTXPTEnvMapBaker::UpdateConstants(const RTXPTEnvMapSettings& Settings)
{
    const float Rotation      = Settings.RotationRadians;
    const float CosA          = std::cos(Rotation);
    const float SinA          = std::sin(Rotation);
    const float Enabled       = Settings.Enabled ? 1.0f : 0.0f;
    const float Intensity     = std::max(Settings.Intensity, 0.0f);
    m_Constants.LocalToWorld0 = float4{CosA, 0.0f, -SinA, 0.0f};
    m_Constants.LocalToWorld1 = float4{0.0f, 1.0f, 0.0f, 0.0f};
    m_Constants.LocalToWorld2 = float4{SinA, 0.0f, CosA, 0.0f};
    m_Constants.WorldToLocal0 = float4{CosA, 0.0f, SinA, 0.0f};
    m_Constants.WorldToLocal1 = float4{0.0f, 1.0f, 0.0f, 0.0f};
    m_Constants.WorldToLocal2 = float4{-SinA, 0.0f, CosA, 0.0f};
    m_Constants.ColorEnabled  = float4{
        Settings.RadianceScale.x * Intensity,
        Settings.RadianceScale.y * Intensity,
        Settings.RadianceScale.z * Intensity,
        Enabled};
    m_Constants.ImportanceMetadata =
        m_Stats.ImportanceReady ?
        float4{1.0f / static_cast<float>(m_Stats.ImportanceResolution),
               1.0f / static_cast<float>(m_Stats.ImportanceResolution),
               static_cast<float>(m_Stats.ImportanceMipLevels - 1u),
               1.0f} :
        float4{1.0f, 1.0f, 0.0f, 0.0f};

    m_LightsBakerParams.Transform.Row0    = m_Constants.LocalToWorld0;
    m_LightsBakerParams.Transform.Row1    = m_Constants.LocalToWorld1;
    m_LightsBakerParams.Transform.Row2    = m_Constants.LocalToWorld2;
    m_LightsBakerParams.InvTransform.Row0 = m_Constants.WorldToLocal0;
    m_LightsBakerParams.InvTransform.Row1 = m_Constants.WorldToLocal1;
    m_LightsBakerParams.InvTransform.Row2 = m_Constants.WorldToLocal2;
    m_LightsBakerParams.ColorMultiplier   = float3{
        m_Constants.ColorEnabled.x,
        m_Constants.ColorEnabled.y,
        m_Constants.ColorEnabled.z};
    m_LightsBakerParams.Enabled = Enabled;
}

} // namespace Diligent
