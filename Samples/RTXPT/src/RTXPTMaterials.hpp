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
#include "RTXPTMaterialData.hpp"
#include "RTXPTSceneJson.hpp"
#include "Texture.h"
#include "TextureView.h"

namespace Diligent
{

struct RTXPTSceneGraphData;
struct RTXPTModelAsset;
struct RTXPTMaterialExtension;

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
    // pContext must be the context used to prepare the scene's textures; color mip generation is recorded here.
    bool Upload(IRenderDevice*             pDevice,
                IDeviceContext*            pContext,
                const RTXPTSceneGraphData& SceneData,
                const std::string&         AssetsRoot,
                const RTXPTFileSource&     FileSource = {});

    const RTXPTMaterialStats& GetStats() const { return m_Stats; }
    IBuffer*                  GetMaterialBuffer() const { return m_MaterialBuffer; }

    // Bindless material-texture table holding one SRV per glTF texture usage (color/data) and per external .material.json
    // texture. Indices are referenced by MaterialPTData texture-index fields. The SRV views are owned here
    // and (being non-default views) keep the underlying texture resources alive on every backend.
    Uint32                GetTextureCount() const { return static_cast<Uint32>(m_TextureBindings.size()); }
    IDeviceObject* const* GetTextureBindings() const { return m_TextureBindings.empty() ? nullptr : m_TextureBindings.data(); }
    ITextureView*         GetTextureView(Uint32 Index) const { return Index < m_TextureViews.size() ? m_TextureViews[Index].RawPtr() : nullptr; }

    // The uploaded material records, one per scene material (RTXPTModelAsset::MaterialRemap), indexing the
    // bindless table above.
    const std::vector<MaterialPTData>& GetMaterialData() const { return m_MaterialData; }

private:
    bool AppendTextureViews(IRenderDevice* pDevice, IDeviceContext* pContext, const GLTF::Model& Model, std::vector<Uint32>& TextureRemap, std::vector<Uint32>& ColorTextureRemap);
    bool CreateMaterialBuffer(IRenderDevice* pDevice, const std::vector<MaterialPTData>& MaterialData);

    RefCntAutoPtr<IBuffer>                   m_MaterialBuffer;
    std::vector<MaterialPTData>              m_MaterialData;
    std::vector<RefCntAutoPtr<ITextureView>> m_TextureViews;
    std::vector<IDeviceObject*>              m_TextureBindings;
    RTXPTMaterialStats                       m_Stats;
};

} // namespace Diligent
