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

#include "RTXPTMaterials.hpp"
#include "RTXPTSceneGraph.hpp"

#include "DebugUtilities.hpp"
#include "FileSystem.hpp"
#include "TextureUtilities.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_map>
#include <utility>
#include <vector>

#include "AsyncPrefix.hpp"
#include "RTXPTSharedTexture.hpp"

namespace Diligent
{

namespace
{

constexpr Uint32 InvalidTextureIndex = ~Uint32{0};

bool HasNonZeroEmission(const float3& Emission)
{
    return Emission.x > 0.0f || Emission.y > 0.0f || Emission.z > 0.0f;
}

bool RTXPTMaterialHasTransmission(const GLTF::Material&         Material,
                                  const RTXPTMaterialExtension* pExtension)
{
    const bool GltfTransmission = Material.Transmission != nullptr;
    const bool ExtensionTransmission =
        pExtension != nullptr && pExtension->Loaded &&
        (pExtension->EnableTransmission || pExtension->TransmissionFactor > 0.0f || pExtension->DiffuseTransmissionFactor > 0.0f);
    return GltfTransmission || ExtensionTransmission;
}

RefCntAutoPtr<ITextureView> CreateMaterialTextureView(ITexture* pTexture)
{
    if (pTexture == nullptr)
        return {};

    TextureViewDesc ViewDesc;
    ViewDesc.ViewType   = TEXTURE_VIEW_SHADER_RESOURCE;
    ViewDesc.TextureDim = RESOURCE_DIM_TEX_2D_ARRAY;

    RefCntAutoPtr<ITextureView> pSRV;
    pTexture->CreateView(ViewDesc, &pSRV);
    return pSRV;
}

void FillMaterialPTDataFromGLTF(const GLTF::Material& Material, MaterialPTData& Data)
{
    const GLTF::Material::ShaderAttribs& Attribs = Material.Attribs;

    Data.baseColorFactor = Attribs.BaseColorFactor;
    Data.emissiveFactor  = Attribs.EmissiveFactor;
    Data.alphaCutoff     = Attribs.AlphaCutoff;
    Data.metallicFactor  = Attribs.MetallicFactor;
    Data.roughnessFactor = Attribs.RoughnessFactor;
    Data.normalScale     = Attribs.NormalScale;

    const int BaseColorTextureId = Material.GetTextureId(GLTF::DefaultBaseColorTextureAttribId);
    if (BaseColorTextureId >= 0)
    {
        Data.flags |= kMaterialFlag_HasBaseColorTexture;
        Data.baseColorTextureIndex = static_cast<Uint32>(BaseColorTextureId);
        Data.baseColorTextureSlice = Material.GetTextureAttrib(GLTF::DefaultBaseColorTextureAttribId).ShaderAttribs.TextureSlice;
    }

    const int EmissiveTextureId = Material.GetTextureId(GLTF::DefaultEmissiveTextureAttribId);
    if (EmissiveTextureId >= 0)
    {
        Data.flags |= kMaterialFlag_HasEmissiveTexture;
        Data.emissiveTextureIndex = static_cast<Uint32>(EmissiveTextureId);
        Data.emissiveTextureSlice = Material.GetTextureAttrib(GLTF::DefaultEmissiveTextureAttribId).ShaderAttribs.TextureSlice;
    }

    const int MetallicRoughnessTextureId = Material.GetTextureId(GLTF::DefaultMetallicRoughnessTextureAttribId);
    if (MetallicRoughnessTextureId >= 0)
    {
        Data.flags |= kMaterialFlag_HasMetallicRoughnessTexture;
        Data.metallicRoughnessTextureIndex = static_cast<Uint32>(MetallicRoughnessTextureId);
        Data.metallicRoughnessTextureSlice = Material.GetTextureAttrib(GLTF::DefaultMetallicRoughnessTextureAttribId).ShaderAttribs.TextureSlice;
    }

    const int NormalTextureId = Material.GetTextureId(GLTF::DefaultNormalTextureAttribId);
    if (NormalTextureId >= 0)
    {
        Data.flags |= kMaterialFlag_HasNormalTexture;
        Data.normalTextureIndex = static_cast<Uint32>(NormalTextureId);
        Data.normalTextureSlice = Material.GetTextureAttrib(GLTF::DefaultNormalTextureAttribId).ShaderAttribs.TextureSlice;
    }

    Data.ior = 1.5f;

    if (Material.Transmission != nullptr)
    {
        Data.transmissionFactor = std::clamp(Material.Transmission->Factor, 0.0f, 1.0f);
        if (Data.transmissionFactor > 0.0f)
            Data.flags |= kMaterialFlag_HasTransmission;

        const int TransmissionTextureId = Material.GetTextureId(GLTF::DefaultTransmissionTextureAttribId);
        if (TransmissionTextureId >= 0)
        {
            Data.flags |= kMaterialFlag_HasTransmissionTexture;
            Data.transmissionTextureIndex = static_cast<Uint32>(TransmissionTextureId);
            Data.transmissionTextureSlice = Material.GetTextureAttrib(GLTF::DefaultTransmissionTextureAttribId).ShaderAttribs.TextureSlice;
        }
    }

    if (Material.Volume != nullptr)
    {
        Data.flags |= kMaterialFlag_HasVolume;
        Data.thicknessFactor           = std::max(Material.Volume->ThicknessFactor, 0.0f);
        Data.volumeAttenuationColor    = Material.Volume->AttenuationColor;
        Data.volumeAttenuationDistance = std::max(Material.Volume->AttenuationDistance, 0.0f);

        const int ThicknessTextureId = Material.GetTextureId(GLTF::DefaultThicknessTextureAttribId);
        if (ThicknessTextureId >= 0)
        {
            Data.flags |= kMaterialFlag_HasThicknessTexture;
            Data.thicknessTextureIndex = static_cast<Uint32>(ThicknessTextureId);
            Data.thicknessTextureSlice = Material.GetTextureAttrib(GLTF::DefaultThicknessTextureAttribId).ShaderAttribs.TextureSlice;
        }
    }

    if ((Data.flags & kMaterialFlag_HasTransmission) != 0u && Material.Volume == nullptr)
        Data.flags |= kMaterialFlag_ThinSurface;

    if (RTXPTMaterialIsAlphaTested(Material) && (Data.flags & kMaterialFlag_HasBaseColorTexture) != 0u)
        Data.flags |= kMaterialFlag_AlphaTested;

    if (RTXPTMaterialIsEmissiveAreaLight(Material))
        Data.flags |= kMaterialFlag_EmissiveAreaLight;
}

void RemapTextureIndex(Uint32 Flag, Uint32& Flags, Uint32& TextureIndex, const std::vector<Uint32>& TextureRemap)
{
    if ((Flags & Flag) == 0u)
        return;

    if (TextureIndex >= TextureRemap.size() || TextureRemap[TextureIndex] == InvalidTextureIndex)
    {
        Flags &= ~Flag;
        TextureIndex = 0;
        return;
    }

    TextureIndex = TextureRemap[TextureIndex];
}

void RemapMaterialTextureIndices(MaterialPTData& Data, const std::vector<Uint32>& TextureRemap)
{
    RemapTextureIndex(kMaterialFlag_HasBaseColorTexture, Data.flags, Data.baseColorTextureIndex, TextureRemap);
    RemapTextureIndex(kMaterialFlag_HasEmissiveTexture, Data.flags, Data.emissiveTextureIndex, TextureRemap);
    RemapTextureIndex(kMaterialFlag_HasMetallicRoughnessTexture, Data.flags, Data.metallicRoughnessTextureIndex, TextureRemap);
    RemapTextureIndex(kMaterialFlag_HasNormalTexture, Data.flags, Data.normalTextureIndex, TextureRemap);
    RemapTextureIndex(kMaterialFlag_HasTransmissionTexture, Data.flags, Data.transmissionTextureIndex, TextureRemap);
    RemapTextureIndex(kMaterialFlag_HasThicknessTexture, Data.flags, Data.thicknessTextureIndex, TextureRemap);

    if ((Data.flags & kMaterialFlag_HasBaseColorTexture) == 0u)
        Data.flags &= ~kMaterialFlag_AlphaTested;
}

// Resolves an authored material-texture path against the assets root, normalizing slashes and preferring a
// neighboring .dds when a .png is authored and the file source has one (matching RTXPT-fork).
std::string ResolveExternalTexturePath(const std::string& AssetsRoot, const std::string& LocalPath, const RTXPTFileSource& FileSource)
{
    std::string Resolved = (std::filesystem::path{AssetsRoot} / LocalPath).string();
    FileSystem::CorrectSlashes(Resolved);
    Resolved = FileSystem::SimplifyPath(Resolved.c_str());

    std::filesystem::path PathObj{Resolved};
    std::string           Ext = PathObj.extension().string();
    std::transform(Ext.begin(), Ext.end(), Ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (Ext == ".png")
    {
        std::string DdsCandidate = PathObj.replace_extension(".dds").string();
        FileSystem::CorrectSlashes(DdsCandidate);
        DdsCandidate = FileSystem::SimplifyPath(DdsCandidate.c_str());
        if (FileSource.Exists(DdsCandidate))
            return DdsCandidate;
    }
    return Resolved;
}

struct ExternalTextureBinding
{
    Uint32 Index;
    bool   SRGB;
    bool   NormalMap;
};
using ExternalTextureCache = std::unordered_map<std::string, ExternalTextureBinding>;

// Loads (or reuses) an external material texture, read through the file source, and appends its SRV to the
// bindless table. Returns the bindless index, or InvalidTextureIndex on failure. Deduplicates by resolved path;
// conflicting sRGB/NormalMap metadata for the same path warns and keeps the first binding.
Uint32 AppendExternalTexture(IRenderDevice*                            pDevice,
                             const RTXPTMaterialTextureDesc&           Desc,
                             const std::string&                        AssetsRoot,
                             const RTXPTFileSource&                    FileSource,
                             std::vector<RefCntAutoPtr<ITextureView>>& TextureViews,
                             std::vector<IDeviceObject*>&              TextureBindings,
                             ExternalTextureCache&                     Cache)
{
    const std::string ResolvedPath = ResolveExternalTexturePath(AssetsRoot, Desc.LocalPath, FileSource);

    const auto CacheIt = Cache.find(ResolvedPath);
    if (CacheIt != Cache.end())
    {
        if (CacheIt->second.SRGB != Desc.SRGB || CacheIt->second.NormalMap != Desc.NormalMap)
            LOG_WARNING_MESSAGE("RTXPT material texture '", ResolvedPath,
                                "' requested with conflicting sRGB/NormalMap metadata; keeping the first binding");
        return CacheIt->second.Index;
    }

    TextureLoadInfo LoadInfo{"RTXPT material texture"};
    LoadInfo.IsSRGB = Desc.SRGB;

    std::vector<unsigned char> Bytes;
    std::string                Error;
    if (!FileSource.ReadBytes(ResolvedPath, Bytes, Error))
    {
        LOG_WARNING_MESSAGE("Failed to read RTXPT material texture: ", Error);
        return InvalidTextureIndex;
    }

    RefCntAutoPtr<ITexture> pTexture;
    CreateSharedTextureFromMemory(Bytes.data(), Bytes.size(), LoadInfo, pDevice, &pTexture);
    if (!pTexture)
    {
        LOG_WARNING_MESSAGE("Failed to load RTXPT material texture: ", ResolvedPath);
        return InvalidTextureIndex;
    }

    RefCntAutoPtr<ITextureView> pSRV = CreateMaterialTextureView(pTexture.RawPtr());
    if (!pSRV)
    {
        LOG_WARNING_MESSAGE("Failed to create SRV for RTXPT material texture: ", ResolvedPath);
        return InvalidTextureIndex;
    }

    const Uint32 BindingIndex = static_cast<Uint32>(TextureBindings.size());
    TextureViews.emplace_back(std::move(pSRV));
    TextureBindings.push_back(TextureViews.back().RawPtr<IDeviceObject>());
    Cache.emplace(ResolvedPath, ExternalTextureBinding{BindingIndex, Desc.SRGB, Desc.NormalMap});
    return BindingIndex;
}

} // namespace

bool RTXPTMaterialIsAlphaTested(const GLTF::Material& Material)
{
    return Material.Attribs.AlphaMode == GLTF::Material::ALPHA_MODE_MASK &&
        Material.GetTextureId(GLTF::DefaultBaseColorTextureAttribId) >= 0;
}

bool RTXPTMaterialIsEmissiveAreaLight(const GLTF::Material& Material)
{
    return RTXPTMaterialIsEmissiveAreaLight(Material, nullptr);
}

const RTXPTMaterialExtension* RTXPTGetMaterialExtension(const RTXPTSceneGraphData& SceneData,
                                                        const RTXPTModelAsset&     Asset,
                                                        Uint32                     MaterialId)
{
#if RTXPT_ENABLE_MATERIAL_EXTENSION
    if (MaterialId >= Asset.MaterialRemap.size())
        return nullptr;

    const Uint32 ExtensionIdx = Asset.MaterialRemap[MaterialId];
    if (ExtensionIdx >= SceneData.MaterialExtensions.size())
        return nullptr;

    return &SceneData.MaterialExtensions[ExtensionIdx];
#else
    (void)SceneData;
    (void)Asset;
    (void)MaterialId;
    return nullptr;
#endif
}

bool RTXPTMaterialHasBaseColorTexture(const GLTF::Model&            Model,
                                      const GLTF::Material&         Material,
                                      const RTXPTMaterialExtension* pExtension)
{
    if (pExtension != nullptr && pExtension->Loaded && !pExtension->EnableBaseTexture)
        return false;

    // An authored, enabled external base texture provides base color even when the glTF carries none.
    if (pExtension != nullptr && pExtension->Loaded && pExtension->BaseTexture.HasPath)
        return true;

    const int BaseColorTextureId = Material.GetTextureId(GLTF::DefaultBaseColorTextureAttribId);
    if (BaseColorTextureId < 0)
        return false;

    const Uint32 TextureId = static_cast<Uint32>(BaseColorTextureId);
    if (TextureId >= static_cast<Uint32>(Model.GetTextureCount()))
        return false;

    RefCntAutoPtr<ITextureView> pSRV = CreateMaterialTextureView(Model.GetTexture(TextureId));
    return pSRV != nullptr;
}

bool RTXPTMaterialIsAlphaTested(const GLTF::Material&         Material,
                                const RTXPTMaterialExtension* pExtension,
                                bool                          HasBaseColorTexture)
{
    if (!HasBaseColorTexture)
        return false;

    return pExtension != nullptr && pExtension->Loaded ?
        pExtension->EnableAlphaTesting :
        Material.Attribs.AlphaMode == GLTF::Material::ALPHA_MODE_MASK;
}

bool RTXPTMaterialIsExcludedFromNEE(const RTXPTMaterialExtension* pExtension)
{
    return pExtension != nullptr && pExtension->Loaded && pExtension->ExcludeFromNEE;
}

bool RTXPTMaterialNeedsAnyHit(const GLTF::Material&         Material,
                              const RTXPTMaterialExtension* pExtension,
                              bool                          HasBaseColorTexture)
{
    return RTXPTMaterialIsAlphaTested(Material, pExtension, HasBaseColorTexture) ||
        RTXPTMaterialIsExcludedFromNEE(pExtension);
}

bool RTXPTMaterialIsEmissiveAreaLight(const GLTF::Material&         Material,
                                      const RTXPTMaterialExtension* pExtension)
{
    // A material qualifies as an emissive area light on its authored emissive factor/strength. Bindless is
    // mandatory, so emissive-textured materials are sampled on the GPU by the emissive-triangle build shader
    // (emission = emissiveFactor * texture); the non-zero factor is the glTF-correct gate (a zero factor
    // yields zero emission regardless of the texture).
    const bool    ExtensionLoaded = pExtension != nullptr && pExtension->Loaded;
    const float3& Emission        = ExtensionLoaded ? pExtension->EmissiveFactor : Material.Attribs.EmissiveFactor;
    return HasNonZeroEmission(Emission);
}

void RTXPTMaterials::Reset()
{
    m_MaterialBuffer.Release();
    m_TextureBindings.clear();
    m_TextureViews.clear();
    m_Stats = {};
}

void RTXPTMaterials::AppendTextureViews(const GLTF::Model& Model, std::vector<Uint32>& TextureRemap)
{
    const Uint32 ModelTextureCount = static_cast<Uint32>(Model.GetTextureCount());
    TextureRemap.assign(ModelTextureCount, InvalidTextureIndex);
    m_TextureViews.reserve(m_TextureViews.size() + ModelTextureCount);
    m_TextureBindings.reserve(m_TextureBindings.size() + ModelTextureCount);

    for (Uint32 i = 0; i < ModelTextureCount; ++i)
    {
        RefCntAutoPtr<ITextureView> pSRV = CreateMaterialTextureView(Model.GetTexture(i));
        if (!pSRV)
            continue;

        TextureRemap[i] = static_cast<Uint32>(m_TextureBindings.size());
        m_TextureViews.emplace_back(std::move(pSRV));
        m_TextureBindings.push_back(m_TextureViews.back().RawPtr<IDeviceObject>());
    }
}

bool RTXPTMaterials::CreateMaterialBuffer(IRenderDevice* pDevice, const std::vector<MaterialPTData>& MaterialData)
{
    BufferDesc Desc;
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    Desc.Name                 = "RTXPT material buffer";
    Desc.Usage                = USAGE_IMMUTABLE;
    Desc.BindFlags            = BIND_SHADER_RESOURCE;
    Desc.Mode                 = BUFFER_MODE_STRUCTURED;
    Desc.ElementByteStride    = sizeof(MaterialPTData);
    Desc.Size                 = sizeof(MaterialPTData) * MaterialData.size();

    BufferData Data{MaterialData.data(), Desc.Size};
    pDevice->CreateBuffer(Desc, &Data, &m_MaterialBuffer);

    VERIFY(m_MaterialBuffer, "Failed to create RTXPT material buffer");
    return m_MaterialBuffer != nullptr;
}

bool RTXPTMaterials::Upload(IRenderDevice*             pDevice,
                            const RTXPTSceneGraphData& SceneData,
                            const std::string&         AssetsRoot,
                            const RTXPTFileSource&     FileSource)
{
    Reset();

    std::vector<std::vector<Uint32>> TextureRemaps(SceneData.ModelAssets.size());
    for (Uint32 AssetIdx = 0; AssetIdx < SceneData.ModelAssets.size(); ++AssetIdx)
    {
        const RTXPTModelAsset& Asset = SceneData.ModelAssets[AssetIdx];
        if (Asset.Model)
            AppendTextureViews(*Asset.Model, TextureRemaps[AssetIdx]);
    }

    ExternalTextureCache ExternalCache;

    std::vector<MaterialPTData> MaterialData;
    for (Uint32 AssetIdx = 0; AssetIdx < SceneData.ModelAssets.size(); ++AssetIdx)
    {
        const RTXPTModelAsset& Asset = SceneData.ModelAssets[AssetIdx];
        if (!Asset.Model)
            continue;

        for (Uint32 MatIdx = 0; MatIdx < Asset.Model->Materials.size(); ++MatIdx)
        {
            const GLTF::Material& Material = Asset.Model->Materials[MatIdx];

            MaterialPTData Data;
            FillMaterialPTDataFromGLTF(Material, Data);
            RemapMaterialTextureIndices(Data, TextureRemaps[AssetIdx]);

            const RTXPTMaterialExtension* pExtension = RTXPTGetMaterialExtension(SceneData, Asset, MatIdx);
            if (pExtension != nullptr && pExtension->Loaded)
            {
                const RTXPTMaterialExtension& Ext = *pExtension;
                Data.baseColorFactor              = Ext.BaseColorFactor;
                Data.emissiveFactor               = Ext.EmissiveFactor;
                Data.alphaCutoff                  = Ext.AlphaCutoff;
                Data.metallicFactor               = Ext.MetallicFactor;
                Data.roughnessFactor              = Ext.RoughnessFactor;
                Data.transmissionFactor           = std::clamp(Ext.TransmissionFactor, 0.0f, 1.0f);
                Data.diffuseTransmissionFactor    = std::clamp(Ext.DiffuseTransmissionFactor, 0.0f, 1.0f);
                Data.ior                          = std::max(Ext.IoR, 1.0f);
                Data.thicknessFactor              = std::max(Ext.ThicknessFactor, 0.0f);
                Data.volumeAttenuationColor       = Ext.VolumeAttenuationColor;
                Data.volumeAttenuationDistance    = std::max(Ext.VolumeAttenuationDistance, 0.0f);
                Data.nestedPriority               = static_cast<Uint32>(std::clamp(Ext.NestedPriority, 0, 14));
                Data.shadowNoLFadeout             = std::clamp(Ext.ShadowNoLFadeout, 0.0f, 0.25f);
                Data.pathDecompositionFlags       = 0;

                if (Ext.PSDExclude)
                    Data.pathDecompositionFlags |= kMaterialPathDecompositionFlag_PSDExclude;
                if (Ext.IgnoreMeshTangentSpace)
                    Data.pathDecompositionFlags |= kMaterialPathDecompositionFlag_IgnoreMeshTangentSpace;

                Data.pathDecompositionFlags |=
                    (static_cast<Uint32>(std::clamp(Ext.PSDBlockMotionVectorsAtSurfaceType, 0, 3)) << kMaterialPathDecompositionFlag_PSDBlockMotionVectorsAtSurfaceShift) &
                    kMaterialPathDecompositionFlag_PSDBlockMotionVectorsAtSurfaceMask;
                Data.pathDecompositionFlags |=
                    (static_cast<Uint32>(std::clamp(Ext.PSDDominantDeltaLobe + 1, 0, 7)) << kMaterialPathDecompositionFlag_PSDDominantDeltaLobeP1Shift) &
                    kMaterialPathDecompositionFlag_PSDDominantDeltaLobeP1Mask;

                if (Ext.EnableTransmission || Data.transmissionFactor > 0.0f || Data.diffuseTransmissionFactor > 0.0f)
                    Data.flags |= kMaterialFlag_HasTransmission;
                else
                {
                    Data.flags &= ~kMaterialFlag_HasTransmission;
                    Data.flags &= ~kMaterialFlag_HasTransmissionTexture;
                }

                if (Ext.ThinSurface)
                    Data.flags |= kMaterialFlag_ThinSurface;
                else
                    Data.flags &= ~kMaterialFlag_ThinSurface;

                if (Data.thicknessFactor > 0.0f ||
                    Data.volumeAttenuationDistance < 3.402823466e+38f ||
                    Data.volumeAttenuationColor.x != 1.0f ||
                    Data.volumeAttenuationColor.y != 1.0f ||
                    Data.volumeAttenuationColor.z != 1.0f)
                    Data.flags |= kMaterialFlag_HasVolume;
                else
                {
                    Data.flags &= ~kMaterialFlag_HasVolume;
                    Data.flags &= ~kMaterialFlag_HasThicknessTexture;
                }
                if (!Ext.EnableBaseTexture)
                    Data.flags &= ~kMaterialFlag_HasBaseColorTexture;
                if (!Ext.EnableEmissiveTexture)
                    Data.flags &= ~kMaterialFlag_HasEmissiveTexture;
                if (!Ext.EnableNormalTexture)
                    Data.flags &= ~kMaterialFlag_HasNormalTexture;
                if (!Ext.EnableOcclusionRoughnessMetallicTexture)
                    Data.flags &= ~kMaterialFlag_HasMetallicRoughnessTexture;

                // Apply external .material.json textures after scalar overrides and enable-switch clears, so
                // MaterialPTData reflects the final texture state before flag recomputation. An external texture
                // overrides the glTF binding for its slot; if the external load fails but a glTF binding exists,
                // the glTF binding is kept (fail-safe for shipped scenes), otherwise the slot falls back to factors.
                const auto ApplyExternalTexture = [&](const RTXPTMaterialTextureDesc& Desc, bool Enable,
                                                      Uint32 Flag, Uint32& TexIndex, float& TexSlice) {
                    if (!Enable || !Desc.HasPath)
                        return;

                    const Uint32 ExtIndex = AppendExternalTexture(pDevice, Desc, AssetsRoot, FileSource,
                                                                  m_TextureViews, m_TextureBindings, ExternalCache);
                    if (ExtIndex != InvalidTextureIndex)
                    {
                        Data.flags |= Flag;
                        TexIndex = ExtIndex;
                        TexSlice = 0.0f;
                    }
                    else if ((Data.flags & Flag) == 0u)
                    {
                        Data.flags &= ~Flag;
                        TexIndex = 0;
                    }
                };

                ApplyExternalTexture(Ext.BaseTexture, Ext.EnableBaseTexture,
                                     kMaterialFlag_HasBaseColorTexture,
                                     Data.baseColorTextureIndex, Data.baseColorTextureSlice);
                ApplyExternalTexture(Ext.OcclusionRoughnessMetallicTexture, Ext.EnableOcclusionRoughnessMetallicTexture,
                                     kMaterialFlag_HasMetallicRoughnessTexture,
                                     Data.metallicRoughnessTextureIndex, Data.metallicRoughnessTextureSlice);
                ApplyExternalTexture(Ext.NormalTexture, Ext.EnableNormalTexture,
                                     kMaterialFlag_HasNormalTexture,
                                     Data.normalTextureIndex, Data.normalTextureSlice);
                ApplyExternalTexture(Ext.EmissiveTexture, Ext.EnableEmissiveTexture,
                                     kMaterialFlag_HasEmissiveTexture,
                                     Data.emissiveTextureIndex, Data.emissiveTextureSlice);

                const bool TransmissionEnabled = (Data.flags & kMaterialFlag_HasTransmission) != 0u;
                ApplyExternalTexture(Ext.TransmissionTexture, Ext.EnableTransmissionTexture && TransmissionEnabled,
                                     kMaterialFlag_HasTransmissionTexture,
                                     Data.transmissionTextureIndex, Data.transmissionTextureSlice);

                if (Ext.HasNormalTextureScale)
                    Data.normalScale = Ext.NormalTextureScale;
            }

            if (RTXPTMaterialIsExcludedFromNEE(pExtension))
                Data.flags |= kMaterialFlag_ExcludeFromNEE;

            Data.flags &= ~kMaterialFlag_AlphaTested;
            if (RTXPTMaterialIsAlphaTested(Material, pExtension, (Data.flags & kMaterialFlag_HasBaseColorTexture) != 0u))
                Data.flags |= kMaterialFlag_AlphaTested;

            Data.flags &= ~kMaterialFlag_EmissiveAreaLight;
            if (RTXPTMaterialIsEmissiveAreaLight(Material, pExtension))
                Data.flags |= kMaterialFlag_EmissiveAreaLight;

            // Match MaterialsBaker::UpdateSubInstanceData's packed UNORM8 cutoff.
            Data.alphaCutoff = RTXPTQuantizeAlphaCutoff(Data.alphaCutoff);
            MaterialData.emplace_back(Data);
        }
    }

    if (MaterialData.empty())
        MaterialData.emplace_back();

    m_Stats.TextureCount  = static_cast<Uint32>(m_TextureBindings.size());
    m_Stats.MaterialCount = static_cast<Uint32>(MaterialData.size());
    return CreateMaterialBuffer(pDevice, MaterialData);
}

} // namespace Diligent
