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
#include <string>
#include <vector>

#include "BasicMath.hpp"
#include "Buffer.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Sampler.h"
#include "Texture.h"
#include "TextureView.h"

#include "RTXPTLightsBaker.hpp"
#include "RTXPTFrameConstants.hpp"
#include "RTXPTEnvMapBakerPass.hpp"
#include "RTXPTSceneGraph.hpp"
#include "RTXPTSceneJson.hpp"

namespace Diligent
{

struct IRenderStateCache;
struct IPipelineStateCache;

enum class RTXPTEnvMapSourceKind
{
    ProceduralSky,
    TextureFile
};

struct RTXPTEnvMapSource
{
    RTXPTEnvMapSourceKind Kind = RTXPTEnvMapSourceKind::ProceduralSky;
    std::string           DisplayName;
    std::string           RelativePath;
    std::string           ResolvedPath;
};

struct RTXPTEnvMapSettings
{
    bool        Enabled                 = true;
    std::string SourceRelativePath      = "==PROCEDURAL_SKY==";
    float3      RadianceScale           = float3{1, 1, 1};
    float       Intensity               = 1.0f;
    float       RotationRadians         = 0.0f;
    Uint32      ImportanceMapResolution = 1024;
};

struct RTXPTEnvMapBakerStats
{
    bool        Ready                = false;
    bool        SourceLoaded         = false;
    bool        Procedural           = true;
    bool        ImportanceReady      = false;
    bool        BRDFLUTReady         = false;
    bool        CompressedOutput     = false;
    Uint32      CubeResolution       = 0;
    Uint32      CubeMipLevels        = 0;
    Uint32      ImportanceResolution = 0;
    Uint32      ImportanceMipLevels  = 0;
    Uint64      Version              = 0;
    std::string SourceName;
    std::string LastError;
};

struct RTXPTEnvMapBakerUpdateAttribs
{
    IEngineFactory*     pEngineFactory = nullptr;
    std::string         AssetsRoot;
    RTXPTEnvMapSettings Settings;
    bool                ForceRebuild     = false;
    bool                ComputeSupported = false;
};

class RTXPTEnvMapBaker
{
public:
    RTXPTEnvMapBaker();
    ~RTXPTEnvMapBaker();

    void Reset();
    void SceneReloaded();
    void SetFileSource(const RTXPTFileSource& Source) { m_FileSource = Source; }

    bool CreateResources(IRenderDevice* pDevice, IDeviceContext* pContext, IEngineFactory* pEngineFactory, IRenderStateCache* pStateCache, IPipelineStateCache* pPSOCache, bool ComputeSupported);
    bool Update(IRenderDevice* pDevice, IDeviceContext* pContext, const RTXPTEnvMapBakerUpdateAttribs& Attribs);

    bool InfoGUI(float Indent);
    bool DebugGUI(float Indent);

    static std::vector<RTXPTEnvMapSource> EnumerateEnvironmentSources(const std::string& AssetsRoot);
    static RTXPTEnvMapSettings            MakeSceneDefaultSettings(const RTXPTSceneGraphData& SceneData);

    const RTXPTEnvMapBakerStats&      GetStats() const { return m_Stats; }
    const RTXPTEnvMapConstants&       GetConstants() const { return m_Constants; }
    const LightsBakerEnvMapParamsCPU& GetLightsBakerParams() const { return m_LightsBakerParams; }

    ITextureView* GetEnvironmentMapSRV() const { return m_EnvironmentMapSRV; }
    ITextureView* GetDiffuseIrradianceSRV() const { return m_DiffuseIrradianceSRV; }
    ITextureView* GetImportanceMapSRV() const { return m_ImportanceMapSRV; }
    ITextureView* GetRadianceMapSRV() const { return m_RadianceMapSRV; }
    ITextureView* GetBRDFLUTSRV() const { return m_BRDFLUTSRV; }
    ISampler*     GetEnvironmentSampler() const { return m_EnvironmentSampler; }
    ISampler*     GetImportanceSampler() const { return m_ImportanceSampler; }

private:
    RTXPTFileSource m_FileSource;
    bool            LoadSourceTexture(IRenderDevice* pDevice, const std::string& AssetsRoot, const RTXPTEnvMapSettings& Settings);
    bool            CreateProceduralSourceTexture(IRenderDevice* pDevice, const RTXPTEnvMapSettings& Settings);
    bool            PrecomputeCubemap(IRenderDevice* pDevice, IDeviceContext* pContext, IEngineFactory* pEngineFactory, const RTXPTEnvMapSettings& Settings);
    bool            BakeHighResEnvironmentCube(IRenderDevice* pDevice, IDeviceContext* pContext, IEngineFactory* pEngineFactory);
    bool            EnsureEnvCubeBakePipeline(IRenderDevice* pDevice, IEngineFactory* pEngineFactory, bool SourceIsCube);
    bool            CreateImportanceMaps(IRenderDevice* pDevice, IDeviceContext* pContext, IEngineFactory* pEngineFactory, const RTXPTEnvMapSettings& Settings, bool ComputeSupported);
    bool            CreateImportanceTextures(IRenderDevice* pDevice, Uint32 Resolution);
    bool            DispatchImportanceBuild(IDeviceContext* pContext, Uint32 Resolution);
    bool            DispatchImportanceReduce(IDeviceContext* pContext, Uint32 Resolution, Uint32 MipLevels);
    void            UseFallbackImportanceMaps(Uint32 RequestedResolution);
    bool            CreateFallbackTextures(IRenderDevice* pDevice);
    bool            CreateSamplers(IRenderDevice* pDevice);
    void            UpdateConstants(const RTXPTEnvMapSettings& Settings);

    RefCntAutoPtr<ITexture>     m_SourceTexture;
    RefCntAutoPtr<ITextureView> m_SourceSRV;
    RefCntAutoPtr<ITexture>     m_FallbackEnvironmentMap;
    RefCntAutoPtr<ITexture>     m_FallbackDiffuseIrradiance;
    RefCntAutoPtr<ITexture>     m_FallbackImportanceMap;
    RefCntAutoPtr<ITexture>     m_FallbackRadianceMap;
    RefCntAutoPtr<ITexture>     m_FallbackBRDFLUT;
    RefCntAutoPtr<IBuffer>      m_ImportanceConstants;
    RefCntAutoPtr<ITexture>     m_ImportanceMap;
    RefCntAutoPtr<ITexture>     m_RadianceMap;
    RefCntAutoPtr<ITextureView> m_EnvironmentMapSRV;
    RefCntAutoPtr<ITextureView> m_DiffuseIrradianceSRV;
    RefCntAutoPtr<ITextureView> m_ImportanceMapSRV;
    RefCntAutoPtr<ITextureView> m_RadianceMapSRV;
    RefCntAutoPtr<ITextureView> m_BRDFLUTSRV;
    RefCntAutoPtr<ISampler>     m_EnvironmentSampler;
    RefCntAutoPtr<ISampler>     m_ImportanceSampler;
#ifndef RTXPT_PATH_TRACING_ONLY
    std::unique_ptr<class PBR_Renderer> m_IBLPrecompute;
#endif
    // High-resolution environment cube baked from the source (cube or 2D equirect). Bound to the path
    // tracer's t_EnvironmentMap instead of the low-resolution GGX-prefiltered IBL cube. See
    // BakeHighResEnvironmentCube.
    RefCntAutoPtr<ITexture>               m_EnvironmentCube;
    RefCntAutoPtr<IBuffer>                m_EnvCubeBakeConstants;
    RefCntAutoPtr<IPipelineState>         m_EnvCubeBakePSOCube;
    RefCntAutoPtr<IShaderResourceBinding> m_EnvCubeBakeSRBCube;
    RefCntAutoPtr<IPipelineState>         m_EnvCubeBakePSOSphere;
    RefCntAutoPtr<IShaderResourceBinding> m_EnvCubeBakeSRBSphere;
    Uint32                                m_EnvironmentCubeResolution = 0;
    IRenderStateCache*                    m_pStateCache               = nullptr; // Owned by RTXPTSample; set in CreateResources.
    IPipelineStateCache*                  m_pPSOCache                 = nullptr; // Owned by the caller of CreateResources; may be null.
    RTXPTEnvMapBakerPass                  m_BuildImportanceBasePass;
    RTXPTEnvMapBakerPass                  m_ReduceImportanceMipPass;

    RTXPTEnvMapConstants       m_Constants;
    LightsBakerEnvMapParamsCPU m_LightsBakerParams;
    RTXPTEnvMapSettings        m_LastSettings;
    RTXPTEnvMapBakerStats      m_Stats;
};

} // namespace Diligent
