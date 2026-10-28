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

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "Buffer.h"
#include "DeviceObject.h"
#include "GLTFLoader.hpp"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "RTXPTSceneJson.hpp"
#include "Texture.h"
#include "TextureView.h"

namespace Diligent
{

struct RTXPTSceneGraphData;
struct RTXPTModelAsset;
struct RTXPTMaterialExtension;

// GPU material record consumed by the reference path tracer (mirrors MaterialPTData in PathTracer/PathTracerShared.h).
// One entry per GLTF material; the closest-hit / any-hit shaders index it via SubInstanceData::MaterialID.
// All texture indices/slices reference the shared bindless material-texture table (one entry per GLTF texture).
struct MaterialPTData
{
    float4 baseColorFactor = float4{1, 1, 1, 1};

    float3 emissiveFactor = float3{0, 0, 0};
    float  alphaCutoff    = 0.5f;

    Uint32 flags                 = 0;
    Uint32 baseColorTextureIndex = 0;
    Uint32 emissiveTextureIndex  = 0;
    float  metallicFactor        = 1.0f;

    float  roughnessFactor               = 1.0f;
    float  baseColorTextureSlice         = 0.0f;
    float  emissiveTextureSlice          = 0.0f;
    Uint32 metallicRoughnessTextureIndex = 0;

    float  metallicRoughnessTextureSlice = 0.0f;
    Uint32 normalTextureIndex            = 0;
    float  normalTextureSlice            = 0.0f;
    float  normalScale                   = 1.0f;

    float transmissionFactor        = 0.0f; // offset 80
    float diffuseTransmissionFactor = 0.0f; // offset 84
    float ior                       = 1.5f; // offset 88
    float thicknessFactor           = 0.0f; // offset 92

    float3 volumeAttenuationColor    = float3{1, 1, 1};  // offset 96
    float  volumeAttenuationDistance = 3.402823466e+38f; // offset 108

    Uint32 transmissionTextureIndex = 0;    // offset 112
    float  transmissionTextureSlice = 0.0f; // offset 116
    Uint32 thicknessTextureIndex    = 0;    // offset 120
    float  thicknessTextureSlice    = 0.0f; // offset 124

    // RTXPT-fork authored priority: 0 is the special highest-priority value; 14 is the default/max authored value.
    Uint32 nestedPriority         = 14;   // offset 128
    Uint32 pathDecompositionFlags = 0;    // offset 132
    float  shadowNoLFadeout       = 0.0f; // offset 136
    float  _paddingR7_1           = 0.0f; // offset 140
};
static_assert(sizeof(MaterialPTData) == 144, "MaterialPTData layout must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, baseColorFactor) == 0,
              "MaterialPTData baseColorFactor offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, emissiveFactor) == 16,
              "MaterialPTData emissiveFactor offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, alphaCutoff) == 28,
              "MaterialPTData alphaCutoff offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, flags) == 32,
              "MaterialPTData flags offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, roughnessFactor) == 48,
              "MaterialPTData roughnessFactor offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, metallicRoughnessTextureIndex) == 60,
              "MaterialPTData metallicRoughnessTextureIndex offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, metallicRoughnessTextureSlice) == 64,
              "MaterialPTData metallicRoughnessTextureSlice offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, normalTextureIndex) == 68,
              "MaterialPTData normalTextureIndex offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, normalScale) == 76,
              "MaterialPTData normalScale offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, transmissionFactor) == 80,
              "MaterialPTData transmissionFactor offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, diffuseTransmissionFactor) == 84,
              "MaterialPTData diffuseTransmissionFactor offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, ior) == 88,
              "MaterialPTData ior offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, thicknessFactor) == 92,
              "MaterialPTData thicknessFactor offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, volumeAttenuationColor) == 96,
              "MaterialPTData volumeAttenuationColor offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, volumeAttenuationDistance) == 108,
              "MaterialPTData volumeAttenuationDistance offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, transmissionTextureIndex) == 112,
              "MaterialPTData transmissionTextureIndex offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, transmissionTextureSlice) == 116,
              "MaterialPTData transmissionTextureSlice offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, thicknessTextureIndex) == 120,
              "MaterialPTData thicknessTextureIndex offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, thicknessTextureSlice) == 124,
              "MaterialPTData thicknessTextureSlice offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, nestedPriority) == 128,
              "MaterialPTData nestedPriority offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, pathDecompositionFlags) == 132,
              "MaterialPTData pathDecompositionFlags offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, shadowNoLFadeout) == 136,
              "MaterialPTData shadowNoLFadeout offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(MaterialPTData, _paddingR7_1) == 140,
              "MaterialPTData final padding offset must match PathTracer/PathTracerShared.h");

// Flag bits for MaterialPTData::flags. Keep in sync with kMaterialFlag* in PathTracer/PathTracerShared.h.
constexpr Uint32 kMaterialFlag_HasBaseColorTexture         = 0x1u;
constexpr Uint32 kMaterialFlag_AlphaTested                 = 0x2u;
constexpr Uint32 kMaterialFlag_HasEmissiveTexture          = 0x4u;
constexpr Uint32 kMaterialFlag_HasMetallicRoughnessTexture = 0x8u;
constexpr Uint32 kMaterialFlag_HasNormalTexture            = 0x10u;
constexpr Uint32 kMaterialFlag_EmissiveAreaLight           = 0x20u;
constexpr Uint32 kMaterialFlag_HasTransmission             = 0x40u;
constexpr Uint32 kMaterialFlag_HasTransmissionTexture      = 0x80u;
constexpr Uint32 kMaterialFlag_HasVolume                   = 0x100u;
constexpr Uint32 kMaterialFlag_HasThicknessTexture         = 0x200u;
constexpr Uint32 kMaterialFlag_ThinSurface                 = 0x400u;
constexpr Uint32 kMaterialFlag_ExcludeFromNEE              = 0x1000u;

// Flag bits for MaterialPTData::pathDecompositionFlags. Keep in sync with PathTracer/PathTracerShared.h.
constexpr Uint32 kMaterialPathDecompositionFlag_PSDExclude                          = 0x1u;
constexpr Uint32 kMaterialPathDecompositionFlag_PSDBlockMotionVectorsAtSurfaceMask  = 0x6u;
constexpr Uint32 kMaterialPathDecompositionFlag_PSDBlockMotionVectorsAtSurfaceShift = 1u;
constexpr Uint32 kMaterialPathDecompositionFlag_IgnoreMeshTangentSpace              = 0x8u;
constexpr Uint32 kMaterialPathDecompositionFlag_PSDDominantDeltaLobeP1Mask          = 0xF0u;
constexpr Uint32 kMaterialPathDecompositionFlag_PSDDominantDeltaLobeP1Shift         = 4u;

// A material is alpha tested only when it uses ALPHA_MODE_MASK and actually has a base-color texture to
// sample the alpha from. This compatibility overload preserves the single-GLTF path behavior.
bool RTXPTMaterialIsAlphaTested(const GLTF::Material& Material);
// Emissive-textured materials are promoted to area lights on the same non-zero emissive-factor test as
// constant emitters; bindless is mandatory, so the emissive-triangle build shader can always sample the
// texture (emission = emissiveFactor * texture, so a zero factor still yields no emission).
bool                          RTXPTMaterialIsEmissiveAreaLight(const GLTF::Material& Material);
const RTXPTMaterialExtension* RTXPTGetMaterialExtension(const RTXPTSceneGraphData& SceneData,
                                                        const RTXPTModelAsset&     Asset,
                                                        Uint32                     MaterialId);
bool                          RTXPTMaterialHasBaseColorTexture(const GLTF::Model&            Model,
                                                               const GLTF::Material&         Material,
                                                               const RTXPTMaterialExtension* pExtension);
bool                          RTXPTMaterialIsAlphaTested(const GLTF::Material&         Material,
                                                         const RTXPTMaterialExtension* pExtension,
                                                         bool                          HasBaseColorTexture);
bool                          RTXPTMaterialIsEmissiveAreaLight(const GLTF::Material&         Material,
                                                               const RTXPTMaterialExtension* pExtension);
bool                          RTXPTMaterialIsExcludedFromNEE(const RTXPTMaterialExtension* pExtension);
inline float                  RTXPTQuantizeAlphaCutoff(float AlphaCutoff)
{
    return static_cast<float>(static_cast<Uint32>(std::clamp(AlphaCutoff, 0.0f, 1.0f) * 255.0f + 0.5f)) / 255.0f;
}
bool RTXPTMaterialNeedsAnyHit(const GLTF::Material&         Material,
                              const RTXPTMaterialExtension* pExtension,
                              bool                          HasBaseColorTexture);

struct RTXPTMaterialStats
{
    Uint32 MaterialCount = 0;
    Uint32 TextureCount  = 0;
};

class RTXPTMaterials
{
public:
    void Reset();
    // Uploads the scene's materials and builds the bindless material-texture table. Emissive-textured
    // materials are promoted to NEE area lights (bindless is mandatory, so their textures are GPU-sampleable).
    // The external textures of the .material.json files are read through FileSource, which reads the disk
    // when it has no callbacks.
    bool Upload(IRenderDevice*             pDevice,
                const RTXPTSceneGraphData& SceneData,
                const std::string&         AssetsRoot,
                const RTXPTFileSource&     FileSource = {});

    const RTXPTMaterialStats& GetStats() const { return m_Stats; }
    IBuffer*                  GetMaterialBuffer() const { return m_MaterialBuffer; }

    // Bindless material-texture table holding one SRV per glTF texture and per external .material.json
    // texture. Indices are referenced by MaterialPTData texture-index fields. The SRV views are owned here
    // and (being non-default views) keep the underlying texture resources alive on every backend.
    Uint32                GetTextureCount() const { return static_cast<Uint32>(m_TextureBindings.size()); }
    IDeviceObject* const* GetTextureBindings() const { return m_TextureBindings.empty() ? nullptr : m_TextureBindings.data(); }

private:
    void AppendTextureViews(const GLTF::Model& Model, std::vector<Uint32>& TextureRemap);
    bool CreateMaterialBuffer(IRenderDevice* pDevice, const std::vector<MaterialPTData>& MaterialData);

    RefCntAutoPtr<IBuffer>                   m_MaterialBuffer;
    std::vector<RefCntAutoPtr<ITextureView>> m_TextureViews;
    std::vector<IDeviceObject*>              m_TextureBindings;
    RTXPTMaterialStats                       m_Stats;
};

} // namespace Diligent
