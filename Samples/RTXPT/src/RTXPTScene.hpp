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

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Buffer.h"
#include "GLTFLoader.hpp"
#include "RefCntAutoPtr.hpp"
#include "RTXPTSceneGraph.hpp"
#include "RTXPTSceneJson.hpp"

namespace Diligent
{

struct RTXPTSceneCamera
{
    std::string          Name;
    float3               Position              = float3{0.0f, 0.0f, 0.0f};
    QuaternionF          Rotation              = QuaternionF{};
    float                VerticalFov           = PI_F / 4.0f;
    float                NearPlane             = 0.1f;
    float                FarPlane              = 10000.0f;
    bool                 HasExplicitClipPlanes = false;
    std::optional<bool>  EnableAutoExposure;
    std::optional<float> ExposureCompensation;
    std::optional<float> ExposureValue;
    std::optional<float> ExposureValueMin;
    std::optional<float> ExposureValueMax;
};

struct RTXPTSceneGeometryStats
{
    bool   HasSkinnedGeometry    = false;
    bool   HasAnimations         = false;
    Uint32 SkinnedNodeCount      = 0;
    Uint32 SkinnedPrimitiveCount = 0;
    Uint32 SkinnedVertexCount    = 0;
};

struct RTXPTSceneLoadOptions
{
    // Where the scene, material and model files are read from; the disk by default.
    RTXPTFileSource FileSource;

    // Creates the model and static geometry buffers with BIND_RAY_TRACING, so that acceleration structures can be
    // built from them. Devices without ray tracing need false.
    bool RayTracing = true;
};

class RTXPTScene
{
public:
    bool LoadScene(IRenderDevice*               pDevice,
                   IDeviceContext*              pContext,
                   const std::string&           AssetsRoot,
                   const std::string&           SceneName,
                   const RTXPTSceneLoadOptions& Options = {});
    bool LoadDefaultScene(IRenderDevice* pDevice, IDeviceContext* pContext, const std::string& AssetsRoot);
    void Update(double CurrTime, double ElapsedTime);
    bool HasValidContent() const;

    const std::string&             GetLoadedSceneName() const { return m_LoadedSceneName; }
    const std::string&             GetAssetsRoot() const { return m_AssetsRoot; }
    const RTXPTFileSource&         GetFileSource() const { return m_FileSource; }
    const std::string&             GetModelPath() const { return m_ModelPath; }
    const GLTF::Model*             GetModel() const;
    const GLTF::ModelTransforms&   GetTransforms() const;
    Uint32                         GetSceneIndex() const { return m_SceneIndex; }
    VALUE_TYPE                     GetIndexType() const { return m_IndexType; }
    Uint32                         GetMeshNodeCount() const { return m_MeshNodeCount; }
    Uint32                         GetPrimitiveCount() const { return m_PrimitiveCount; }
    Uint32                         GetMaterialCount() const { return m_MaterialCount; }
    Uint32                         GetLightCount() const { return m_LightCount; }
    Uint32                         GetCameraCount() const { return static_cast<Uint32>(m_Cameras.size()); }
    const RTXPTSceneCamera*        GetCamera(Uint32 CameraIndex) const;
    const RTXPTSceneGeometryStats& GetGeometryStats() const { return m_GeometryStats; }
    const RTXPTSceneGraphData&     GetSceneGraphData() const { return m_SceneGraph; }
    const RTXPTSceneAdapterStats&  GetAdapterStats() const { return m_SceneGraph.Stats; }
    Uint32                         GetModelAssetCount() const { return static_cast<Uint32>(m_SceneGraph.ModelAssets.size()); }
    Uint32                         GetModelInstanceCount() const { return static_cast<Uint32>(m_SceneGraph.ModelInstances.size()); }
    bool                           HasSkinnedGeometry() const { return m_GeometryStats.HasSkinnedGeometry; }
    bool                           HasAnimation() const { return m_GeometryStats.HasAnimations; }
    bool                           IsGeometryDirty() const { return m_GeometryDirty; }
    void                           ClearGeometryDirty() { m_GeometryDirty = false; }

    // Buffer 0 packs POSITION + NORMAL + TEXCOORD_0 (the Diligent GLTF default layout).
    // VertexStride0 is the per-vertex stride for buffer 0 and must equal sizeof(GeometryVertexData) on the shader side.
    IBuffer* GetVertexBuffer0(IRenderDevice* pDevice = nullptr, IDeviceContext* pContext = nullptr) const;
    IBuffer* GetSkinningBuffer(IRenderDevice* pDevice = nullptr, IDeviceContext* pContext = nullptr) const;
    IBuffer* GetIndexBuffer(IRenderDevice* pDevice = nullptr, IDeviceContext* pContext = nullptr) const;
    Uint32   GetVertexStride0() const { return m_VertexStride0; }

private:
    void                      ResetLoadedData();
    void                      CacheSceneData();
    bool                      LoadSceneCameras(const std::string& ScenePath);
    const RTXPTModelAsset*    GetCompatibilityModelAsset() const;
    const RTXPTModelInstance* GetCompatibilityModelInstance() const;

    RTXPTSceneGraphData           m_SceneGraph;
    RTXPTFileSource               m_FileSource;
    RefCntAutoPtr<IBuffer>        m_StaticVertexBuffer;
    RefCntAutoPtr<IBuffer>        m_StaticIndexBuffer;
    GLTF::ModelTransforms         m_Transforms;
    std::vector<RTXPTSceneCamera> m_Cameras;
    std::string                   m_LoadedSceneName;
    std::string                   m_AssetsRoot;
    std::string                   m_ModelPath;
    VALUE_TYPE                    m_IndexType      = VT_UINT32;
    Uint32                        m_SceneIndex     = 0;
    Uint32                        m_MeshNodeCount  = 0;
    Uint32                        m_PrimitiveCount = 0;
    Uint32                        m_MaterialCount  = 0;
    Uint32                        m_LightCount     = 0;
    Uint32                        m_VertexStride0  = 0;
    RTXPTSceneGeometryStats       m_GeometryStats;
    float                         m_AnimationTime  = 0.0f;
    Int32                         m_AnimationIndex = -1;
    bool                          m_GeometryDirty  = false;
};

} // namespace Diligent
