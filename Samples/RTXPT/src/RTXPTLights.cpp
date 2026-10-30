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

#include "RTXPTLights.hpp"
#include "RTXPTMaterials.hpp"
#include "RTXPTSceneGraph.hpp"
#include "RTXPTSceneJson.hpp"

#include "DebugUtilities.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "AsyncPrefix.hpp"

namespace Diligent
{

namespace
{

constexpr float kPolymorphicLightTypeSphere      = 0.0f;
constexpr float kPolymorphicLightTypeDirectional = 2.0f;
constexpr float kPolymorphicLightTypePoint       = 4.0f;
constexpr float kDefaultPunctualLightRadius      = 0.01f;
constexpr float kDefaultDirectionalAngularSize   = PI_F * 0.53f / 180.0f;

float DegreesToRadians(float AngleInDegrees)
{
    return AngleInDegrees * (PI_F / 180.0f);
}

float ReadRTXPTSpotAngleRadians(const nlohmann::json& Json, const char* DegreesKey, const char* LegacyRadiansKey, float DefaultDegrees = 0.0f)
{
    const auto DegreesIt = Json.find(DegreesKey);
    if (DegreesIt != Json.end() && DegreesIt->is_number())
        return DegreesToRadians(DegreesIt->get<float>());

    // Legacy fallback keeps the old glTF-style cone field names in their original units.
    const auto LegacyIt = Json.find(LegacyRadiansKey);
    if (LegacyIt != Json.end() && LegacyIt->is_number())
        return LegacyIt->get<float>();

    return DegreesToRadians(DefaultDegrees);
}

PolymorphicLightInfoFull MakeSphereLightData(const float3& Color, float Intensity, const float4x4& Transform, float Range, float Radius, float InnerCone, float OuterCone)
{
    const float  ClampedRadius = std::max(Radius, 1.0e-4f);
    const float3 Radiance      = Color * std::max(Intensity, 0.0f) / (PI_F * ClampedRadius * ClampedRadius);

    AnalyticLightData Data;
    Data.colorType      = float4{Radiance.x, Radiance.y, Radiance.z, kPolymorphicLightTypeSphere};
    Data.positionRadius = float4{Transform._41, Transform._42, Transform._43, ClampedRadius};
    Data.directionRange = float4{-Transform._31, -Transform._32, -Transform._33, Range};
    if (OuterCone > 0.0f)
    {
        const float OuterCos = std::cos(OuterCone);
        const float InnerCos = std::cos(InnerCone);
        Data.shaping         = float4{OuterCos, std::max(0.0f, InnerCos - OuterCos), 0.0f, 0.0f};
    }
    return PackAnalyticLight(Data);
}

PolymorphicLightInfoFull MakeDirectionalLightData(const float3& Color, float Intensity, const float4x4& Transform, float AngularSizeRadians)
{
    const float  HalfAngle  = std::max(AngularSizeRadians * 0.5f, 0.00001f);
    const float  SolidAngle = std::max(2.0f * PI_F * (1.0f - std::cos(HalfAngle)), 1.0e-8f);
    const float3 Radiance   = Color * std::max(Intensity, 0.0f) / SolidAngle;

    AnalyticLightData Data;
    Data.colorType      = float4{Radiance.x, Radiance.y, Radiance.z, kPolymorphicLightTypeDirectional};
    Data.directionRange = float4{-Transform._31, -Transform._32, -Transform._33, 0.0f};
    Data.shaping        = float4{-1.0f, 0.0f, 0.0f, SolidAngle};
    return PackAnalyticLight(Data);
}

PolymorphicLightInfoFull MakeLightData(const GLTF::Light& Light, const float4x4& NodeTransform)
{
    switch (Light.Type)
    {
        case GLTF::Light::TYPE::DIRECTIONAL:
            return MakeDirectionalLightData(Light.Color, Light.Intensity, NodeTransform, kDefaultDirectionalAngularSize);
        case GLTF::Light::TYPE::POINT:
            return MakeSphereLightData(Light.Color, Light.Intensity, NodeTransform, Light.Range,
                                       kDefaultPunctualLightRadius, 0.0f, 0.0f);
        case GLTF::Light::TYPE::SPOT:
            return MakeSphereLightData(Light.Color, Light.Intensity, NodeTransform, Light.Range,
                                       kDefaultPunctualLightRadius, Light.InnerConeAngle, Light.OuterConeAngle);
        default:
            return {};
    }
}

float ReadRTXPTLightIntensity(const nlohmann::json& Json)
{
    return ReadRTXPTOptionalFloat(Json, "intensity",
                                  ReadRTXPTOptionalFloat(Json, "irradiance", 1.0f));
}

float ReadRTXPTDirectionalAngularSizeRadians(const nlohmann::json& Json)
{
    const auto DegreesIt = Json.find("angularSize");
    if (DegreesIt != Json.end() && DegreesIt->is_number())
        return DegreesToRadians(DegreesIt->get<float>());

    return ReadRTXPTOptionalFloat(Json, "angularSizeRadians", kDefaultDirectionalAngularSize);
}

Uint32 GetPrimitiveTriangleCount(const GLTF::Primitive& Primitive)
{
    return Primitive.HasIndices() ? Primitive.IndexCount / 3u : Primitive.VertexCount / 3u;
}

float MaxRGB(const float3& V)
{
    return std::max(V.x, std::max(V.y, V.z));
}

float GetMaterialEmissionMagnitude(const GLTF::Material& Material, const RTXPTMaterialExtension* pExtension)
{
    if (pExtension != nullptr && pExtension->Loaded)
        return MaxRGB(pExtension->EmissiveFactor);

    return MaxRGB(Material.Attribs.EmissiveFactor);
}

} // namespace

void RTXPTLights::Reset()
{
    m_LightBuffer.Release();
    m_LightExBuffer.Release();
    m_EmissiveTriangleBuffer.Release();
    m_AnalyticLights.clear();
    m_AnalyticSources.clear();
    m_TriangleSources.clear();
    m_EmissiveProxyWeight = 0.0f;
    m_Stats               = {};
}

bool RTXPTLights::UploadLightBuffer(IRenderDevice* pDevice, std::vector<PolymorphicLightInfoFull>& Lights)
{
    for (const auto& Source : m_AnalyticSources)
        Lights[Source.Base].Extended.ClusterIndexAndCount = ~0u;
    m_AnalyticLights   = Lights;
    m_Stats.LightCount = static_cast<Uint32>(m_AnalyticLights.size());
    if (Lights.empty())
    {
        // Always upload at least one default (disabled) light so the shader-side bridge SRV is never null.
        PolymorphicLightInfoFull Default = {};
        Lights.emplace_back(Default);
    }

    std::vector<PolymorphicLightInfo>   Base(Lights.size());
    std::vector<PolymorphicLightInfoEx> Extended(Lights.size());
    for (size_t I = 0; I < Lights.size(); ++I)
    {
        Base[I]     = Lights[I].Base;
        Extended[I] = Lights[I].Extended;
    }

    BufferDesc Desc;
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    Desc.Name                 = "RTXPT light buffer";
    Desc.Usage                = USAGE_IMMUTABLE;
    Desc.BindFlags            = BIND_SHADER_RESOURCE;
    Desc.Mode                 = BUFFER_MODE_STRUCTURED;
    Desc.ElementByteStride    = sizeof(PolymorphicLightInfo);
    Desc.Size                 = sizeof(PolymorphicLightInfo) * Base.size();

    BufferData Data{Base.data(), Desc.Size};
    pDevice->CreateBuffer(Desc, &Data, &m_LightBuffer);

    Desc.Name              = "RTXPT light extensions";
    Desc.ElementByteStride = sizeof(PolymorphicLightInfoEx);
    Desc.Size              = sizeof(PolymorphicLightInfoEx) * Extended.size();
    Data                   = BufferData{Extended.data(), Desc.Size};
    pDevice->CreateBuffer(Desc, &Data, &m_LightExBuffer);

    VERIFY(m_LightBuffer, "Failed to create RTXPT light buffer");
    return m_LightBuffer != nullptr && m_LightExBuffer != nullptr;
}

bool RTXPTLights::UploadEmissiveTriangleBuffer(IRenderDevice* pDevice, Uint32 EmissiveTriangleCount)
{
    m_EmissiveTriangleBuffer.Release();

    if (pDevice == nullptr)
    {
        m_Stats.LastError = "RTXPT emissive triangle buffer requires a render device";
        LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
        return false;
    }

    const Uint32 ElementCount = std::max<Uint32>(EmissiveTriangleCount, 1u);

    BufferDesc Desc;
    Desc.ImmediateContextMask = kRTXPTImmediateContextMask;
    Desc.Name                 = "RTXPT emissive triangle buffer";
    Desc.Usage                = USAGE_DEFAULT;
    Desc.BindFlags            = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    Desc.Mode                 = BUFFER_MODE_STRUCTURED;
    Desc.ElementByteStride    = sizeof(PolymorphicLightInfo);
    Desc.Size                 = Uint64{ElementCount} * sizeof(PolymorphicLightInfo);

    pDevice->CreateBuffer(Desc, nullptr, &m_EmissiveTriangleBuffer);

    VERIFY(m_EmissiveTriangleBuffer, "Failed to create RTXPT emissive triangle buffer");
    if (!m_EmissiveTriangleBuffer)
    {
        m_Stats.LastError = "Failed to create RTXPT emissive triangle buffer";
        return false;
    }

    return true;
}

bool RTXPTLights::Upload(IRenderDevice* pDevice, const GLTF::Scene& Scene, const GLTF::ModelTransforms& Transforms)
{
    Reset();

    std::vector<PolymorphicLightInfoFull> Lights;
    for (const GLTF::Node* pNode : Scene.LinearNodes)
    {
        if (pNode == nullptr || pNode->pLight == nullptr)
            continue;

        if (pNode->Index < 0 || static_cast<size_t>(pNode->Index) >= Transforms.NodeGlobalMatrices.size())
            continue;

        m_AnalyticSources.push_back({{0u, reinterpret_cast<Uint64>(pNode), 0u, 0u}, static_cast<Uint32>(Lights.size()), 1u});
        Lights.emplace_back(MakeLightData(*pNode->pLight, Transforms.NodeGlobalMatrices[pNode->Index]));
    }

    return UploadLightBuffer(pDevice, Lights);
}

void BuildRTXPTSceneLights(const RTXPTSceneGraphData& SceneData, std::vector<RTXPTSceneLight>& Lights)
{
    Lights.clear();
    auto Add = [&](const PolymorphicLightInfoFull& Light, const std::array<Uint64, 4>& Key) {
        RTXPTSceneLight SceneLight;
        SceneLight.Light = Light;
        SceneLight.Key   = Key;
        Lights.push_back(SceneLight);
    };

    for (const RTXPTSceneLightMetadata& LightMeta : SceneData.Lights)
    {
        if (LightMeta.Type == "EnvironmentLight")
            continue;

        float Color[3] = {1.0f, 1.0f, 1.0f};
        ReadRTXPTFloatArray(LightMeta.RawJson, "color", Color, 3);

        const float3                LightColor{Color[0], Color[1], Color[2]};
        const float                 Intensity = ReadRTXPTLightIntensity(LightMeta.RawJson);
        const std::array<Uint64, 4> Key{1u, LightMeta.GraphNodeId, 0u, 0u};
        if (LightMeta.Type == "DirectionalLight")
        {
            Add(MakeDirectionalLightData(LightColor, Intensity, LightMeta.GlobalTransform,
                                         ReadRTXPTDirectionalAngularSizeRadians(LightMeta.RawJson)),
                Key);
        }
        else if (LightMeta.Type == "PointLight")
        {
            Add(MakeSphereLightData(LightColor, Intensity, LightMeta.GlobalTransform,
                                    ReadRTXPTOptionalFloat(LightMeta.RawJson, "range", 0.0f),
                                    ReadRTXPTOptionalFloat(LightMeta.RawJson, "radius", kDefaultPunctualLightRadius),
                                    0.0f,
                                    0.0f),
                Key);
        }
        else if (LightMeta.Type == "SpotLight")
        {
            Add(MakeSphereLightData(LightColor, Intensity, LightMeta.GlobalTransform,
                                    ReadRTXPTOptionalFloat(LightMeta.RawJson, "range", 0.0f),
                                    ReadRTXPTOptionalFloat(LightMeta.RawJson, "radius", kDefaultPunctualLightRadius),
                                    ReadRTXPTSpotAngleRadians(LightMeta.RawJson, "innerAngle", "innerConeAngle"),
                                    ReadRTXPTSpotAngleRadians(LightMeta.RawJson, "outerAngle", "outerConeAngle")),
                Key);
        }
    }

    //KHR_lights_punctual belongs to the same instantiated glTF scene as the geometry. Its node matrices already
    //contain the manifest transform, just like the instance transforms of the imported scene.
    for (const auto& Instance : SceneData.ModelInstances)
    {
        if (Instance.ModelAssetId >= SceneData.ModelAssets.size())
            continue;
        const auto& Asset = SceneData.ModelAssets[Instance.ModelAssetId];
        if (!Asset.Model || Asset.SceneIndex >= Asset.Model->Scenes.size())
            continue;
        const auto& Transforms = Instance.Transforms.NodeGlobalMatrices.empty() ? Asset.StaticTransforms : Instance.Transforms;
        for (const auto* Node : Asset.Model->Scenes[Asset.SceneIndex].LinearNodes)
        {
            if (!Node || !Node->pLight || Node->Index < 0 || static_cast<size_t>(Node->Index) >= Transforms.NodeGlobalMatrices.size())
                continue;
            Add(MakeLightData(*Node->pLight, Transforms.NodeGlobalMatrices[Node->Index]),
                {0u, Instance.GraphNodeId, reinterpret_cast<Uint64>(Node), 0u});
        }
    }
}

bool RTXPTLights::Upload(IRenderDevice* pDevice, const RTXPTSceneGraphData& SceneData)
{
    Reset();
    std::vector<RTXPTSceneLight> Lights;
    BuildRTXPTSceneLights(SceneData, Lights);
    return UploadAnalyticLights(pDevice, Lights);
}

bool RTXPTLights::UploadAnalyticLights(IRenderDevice* pDevice, const std::vector<RTXPTSceneLight>& SceneLights)
{
    m_LightBuffer.Release();
    m_LightExBuffer.Release();
    m_AnalyticSources.clear();
    std::vector<PolymorphicLightInfoFull> Lights;
    Lights.reserve(SceneLights.size());
    for (const RTXPTSceneLight& Light : SceneLights)
    {
        m_AnalyticSources.push_back({Light.Key, static_cast<Uint32>(Lights.size()), 1u});
        Lights.push_back(Light.Light);
    }
    return UploadLightBuffer(pDevice, Lights);
}

bool RTXPTLights::SetEmissiveTriangles(IRenderDevice* pDevice, std::vector<RTXPTLightSourceRange> Sources, Uint32 TriangleCount, float ProxyWeight)
{
    m_TriangleSources             = std::move(Sources);
    m_Stats.EmissiveTriangleCount = TriangleCount;
    m_EmissiveProxyWeight         = ProxyWeight;
    if (m_EmissiveTriangleBuffer && m_EmissiveTriangleBuffer->GetDesc().Size >= Uint64{std::max(TriangleCount, 1u)} * sizeof(PolymorphicLightInfo))
        return true;
    return UploadEmissiveTriangleBuffer(pDevice, TriangleCount);
}

bool RTXPTLights::UploadEmissiveTriangles(IRenderDevice* pDevice, const RTXPTSceneGraphData& SceneData)
{
    m_TriangleSources.clear();
    m_Stats.EmissiveTriangleCount = 0;
    m_Stats.LastError.clear();
    m_EmissiveProxyWeight = 0.0f;

    Uint64 EmissiveTriangleCount = 0;
    for (Uint32 InstanceId = 0; InstanceId < SceneData.ModelInstances.size(); ++InstanceId)
    {
        const RTXPTModelInstance& Instance = SceneData.ModelInstances[InstanceId];
        if (Instance.ModelAssetId >= SceneData.ModelAssets.size())
            continue;

        const RTXPTModelAsset& Asset = SceneData.ModelAssets[Instance.ModelAssetId];
        if (!Asset.Model || Asset.SceneIndex >= Asset.Model->Scenes.size())
            continue;

        const GLTF::Model&           Model      = *Asset.Model;
        const GLTF::Scene&           Scene      = Model.Scenes[Asset.SceneIndex];
        const GLTF::ModelTransforms& Transforms = Instance.Transforms.NodeGlobalMatrices.empty() ?
            Asset.StaticTransforms :
            Instance.Transforms;
        for (const GLTF::Node* pNode : Scene.LinearNodes)
        {
            if (pNode == nullptr || pNode->pMesh == nullptr)
                continue;
            if (pNode->Index < 0 || static_cast<size_t>(pNode->Index) >= Transforms.NodeGlobalMatrices.size())
                continue;

            for (const GLTF::Primitive& Primitive : pNode->pMesh->Primitives)
            {
                if (Primitive.MaterialId >= Model.Materials.size())
                    continue;

                const RTXPTMaterialExtension* pExtension = RTXPTGetMaterialExtension(SceneData, Asset, Primitive.MaterialId);
                if (!RTXPTMaterialIsEmissiveAreaLight(Model.Materials[Primitive.MaterialId], pExtension))
                    continue;

                const Uint32 TriangleCount = GetPrimitiveTriangleCount(Primitive);
                const Uint64 NewCount      = EmissiveTriangleCount + TriangleCount;
                if (NewCount > Uint64{std::numeric_limits<Uint32>::max()})
                {
                    m_Stats.LastError = "RTXPT emissive triangle count exceeds Uint32";
                    LOG_ERROR_MESSAGE(m_Stats.LastError.c_str());
                    return false;
                }

                m_TriangleSources.push_back({{2u, Instance.GraphNodeId, reinterpret_cast<Uint64>(&Primitive), static_cast<Uint64>(pNode->Index)},
                                             static_cast<Uint32>(EmissiveTriangleCount),
                                             TriangleCount});
                EmissiveTriangleCount = NewCount;
                m_EmissiveProxyWeight += static_cast<float>(TriangleCount) *
                    std::max(1e-6f, GetMaterialEmissionMagnitude(Model.Materials[Primitive.MaterialId], pExtension));
            }
        }
    }

    m_Stats.EmissiveTriangleCount = static_cast<Uint32>(EmissiveTriangleCount);
    return UploadEmissiveTriangleBuffer(pDevice, m_Stats.EmissiveTriangleCount);
}

} // namespace Diligent
