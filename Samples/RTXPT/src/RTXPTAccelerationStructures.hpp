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

#include <cstddef>
#include <string>
#include <vector>

#include "BottomLevelAS.h"
#include "Buffer.h"
#include "DeviceContext.h"
#include "GLTFLoader.hpp"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "RTXPTSkinnedGeometry.hpp"
#include "TopLevelAS.h"

namespace Diligent
{

struct SubInstanceData
{
    Uint32 MaterialID             = 0;
    Uint32 Flags                  = 0;
    Uint32 IndexOffset            = 0;
    Uint32 IndexCount             = 0;
    Uint32 VertexOffset           = 0;
    Uint32 VertexCount            = 0;
    Uint32 EmissiveTriangleOffset = 0;
    Uint32 _padding1              = 0;
};
static_assert(sizeof(SubInstanceData) == 32, "SubInstanceData layout must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SubInstanceData, IndexOffset) == 8, "SubInstanceData IndexOffset offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SubInstanceData, VertexOffset) == 16, "SubInstanceData VertexOffset offset must match PathTracer/PathTracerShared.h");
static_assert(offsetof(SubInstanceData, EmissiveTriangleOffset) == 24, "SubInstanceData EmissiveTriangleOffset offset must match PathTracer/PathTracerShared.h");

// Flag bits for SubInstanceData::Flags.
constexpr Uint32 kSubInstanceFlag_Indexed = 0x1u;
constexpr Uint32 kSubInstanceFlag_Skinned = 0x2u;

struct RTXPTAccelerationStructureStats
{
    bool   RayTracingSupported       = false;
    bool   Built                     = false;
    Uint32 GeometryCount             = 0;
    Uint32 InstanceCount             = 0;
    Uint32 BLASCount                 = 0;
    Uint32 SubInstanceCount          = 0;
    Uint32 AlphaTestedGeometryCount  = 0;
    Uint32 ExcludedFromNEEGeometryCount = 0;
    Uint64 BLASScratchSize           = 0;
    Uint64 TLASScratchSize           = 0;
};

class RTXPTAccelerationStructures
{
public:
    void Reset();

    // The per-sub-instance EmissiveTriangleOffset allocation classifies emitters unconditionally (bindless is
    // mandatory), staying consistent with the material flag and RTXPTLights::UploadEmissiveTriangles.
    bool BuildScene(IRenderDevice*                   pDevice,
                    IDeviceContext*                  pContext,
                    const RTXPTSceneGraphData&       SceneData,
                    VALUE_TYPE                       IndexType,
                    const RTXPTSkinnedSceneGeometry* pSkinnedGeometry,
                    bool                             RayTracingSupported);

    bool BuildStaticScene(IRenderDevice*             pDevice,
                          IDeviceContext*            pContext,
                          const RTXPTSceneGraphData& SceneData,
                          VALUE_TYPE                 IndexType,
                          bool                       RayTracingSupported)
    {
        return BuildScene(pDevice, pContext, SceneData, IndexType, nullptr, RayTracingSupported);
    }

    bool UpdateDynamicBLAS(IDeviceContext*                  pContext,
                           const RTXPTSceneGraphData&       SceneData,
                           const RTXPTSkinnedSceneGeometry& SkinnedGeometry);

    bool IsBuilt() const { return m_Stats.Built && m_TLAS; }

    ITopLevelAS* GetTLAS() const { return m_TLAS; }
    IBuffer*     GetSubInstanceBuffer() const { return m_SubInstanceBuffer; }
    IBuffer*     GetSubInstanceTransformBuffer() const { return m_SubInstanceTransformBuffer; }

    const RTXPTAccelerationStructureStats& GetStats() const { return m_Stats; }

private:
    struct BLASRecord
    {
        std::string                        Name;
        RefCntAutoPtr<IBottomLevelAS>      BLAS;
        RefCntAutoPtr<IBuffer>             VertexBuffer;
        RefCntAutoPtr<IBuffer>             IndexBuffer;
        std::vector<std::string>           GeometryNames;
        std::vector<BLASBuildTriangleData> TriangleData;
        RTXPTSceneId                       ModelAssetId          = InvalidRTXPTSceneId;
        RTXPTSceneId                       ModelInstanceId       = InvalidRTXPTSceneId;
        const GLTF::Node*                  pNode                 = nullptr;
        Uint32                             GeometryCount         = 0;
        bool                               Dynamic               = false;
        Uint32                             SkinningDispatchCount = 0;
        Uint32                             InstanceIndex         = 0;
        Uint32                             SubInstanceBase       = 0;
    };

    bool UpdateTLAS(IDeviceContext* pContext, const RTXPTSceneGraphData& SceneData);

    std::vector<BLASRecord>            m_BLASRecords;
    std::vector<std::string>           m_InstanceNames;
    std::vector<TLASBuildInstanceData> m_TLASInstances;
    RefCntAutoPtr<ITopLevelAS>         m_TLAS;
    RefCntAutoPtr<IBuffer>             m_BLASScratch;
    RefCntAutoPtr<IBuffer>             m_TLASScratch;
    RefCntAutoPtr<IBuffer>             m_InstanceBuffer;
    RefCntAutoPtr<IBuffer>             m_SubInstanceBuffer;
    RefCntAutoPtr<IBuffer>             m_SubInstanceTransformBuffer;
    std::vector<float4x4>              m_SubInstanceTransforms;
    RTXPTAccelerationStructureStats    m_Stats;
};

} // namespace Diligent
