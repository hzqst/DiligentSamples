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

#include "Buffer.h"
#include "GLTFLoader.hpp"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "RTXPTLightSourceHistory.hpp"
#include "RTXPTLightPacking.hpp"

namespace Diligent
{

struct RTXPTSceneGraphData;

struct RTXPTLightStats
{
    Uint32      LightCount            = 0;
    Uint32      EmissiveTriangleCount = 0;
    std::string LastError;
};

class RTXPTLights
{
public:
    void Reset();
    bool Upload(IRenderDevice* pDevice, const GLTF::Scene& Scene, const GLTF::ModelTransforms& Transforms);
    bool Upload(IRenderDevice* pDevice, const RTXPTSceneGraphData& SceneData);
    // Counts the scene's emissive-area-light triangles. Emitter classification (here, the material flag, and
    // the per-sub-instance EmissiveTriangleOffset allocation) is unconditional now that bindless is mandatory.
    bool UploadEmissiveTriangles(IRenderDevice* pDevice, const RTXPTSceneGraphData& SceneData);

    const RTXPTLightStats&                       GetStats() const { return m_Stats; }
    IBuffer*                                     GetLightBuffer() const { return m_LightBuffer; }
    IBuffer*                                     GetLightExBuffer() const { return m_LightExBuffer; }
    IBuffer*                                     GetEmissiveTriangleBuffer() const { return m_EmissiveTriangleBuffer; }
    Uint32                                       GetEmissiveTriangleCount() const { return m_Stats.EmissiveTriangleCount; }
    const std::vector<PolymorphicLightInfoFull>& GetAnalyticLights() const { return m_AnalyticLights; }
    float                                        GetEmissiveProxyWeight() const { return m_EmissiveProxyWeight; }
    const std::vector<RTXPTLightSourceRange>&    GetAnalyticSources() const { return m_AnalyticSources; }
    const std::vector<RTXPTLightSourceRange>&    GetTriangleSources() const { return m_TriangleSources; }

private:
    bool UploadLightBuffer(IRenderDevice* pDevice, std::vector<PolymorphicLightInfoFull>& Lights);
    bool UploadEmissiveTriangleBuffer(IRenderDevice* pDevice, Uint32 EmissiveTriangleCount);

    RefCntAutoPtr<IBuffer>                m_LightBuffer;
    RefCntAutoPtr<IBuffer>                m_LightExBuffer;
    RefCntAutoPtr<IBuffer>                m_EmissiveTriangleBuffer;
    std::vector<PolymorphicLightInfoFull> m_AnalyticLights;
    std::vector<RTXPTLightSourceRange>    m_AnalyticSources, m_TriangleSources;
    float                                 m_EmissiveProxyWeight = 0.0f;
    RTXPTLightStats                       m_Stats;
};

} // namespace Diligent
