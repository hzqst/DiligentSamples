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

#include "RTXPTSample.hpp"
#include "RTXPTCameraBasis.hpp"
#include "DebugUtilities.hpp"
#include "GraphicsAccessories.hpp"
#include "GraphicsUtilities.h"
#include "FileSystem.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"
#include "ArchiverFactoryLoader.h"
#include "FileWrapper.hpp"
#include "DataBlobImpl.hpp"
#include "imgui.h"
#include "CommandLineParser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#if PLATFORM_WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <Windows.h>
#endif

namespace Diligent
{

namespace
{

constexpr float       kDefaultCameraNearPlane         = 1.0f;
constexpr float       kDefaultCameraFarPlane          = 10000.0f;
constexpr float       kMinClipPlaneSeparation         = 1e-3f;
constexpr int         kMaxBounceSliderValue           = 48;
constexpr const char* kPreferredSceneName             = "convergence-test.scene.json";
constexpr const char* kSceneFileSuffix                = ".scene.json";
constexpr Uint32      kMaxPackedEmissiveTriangleCount = 0x7fffffffu;
constexpr bool        kReferencePathTraceMode         = true;
const float           kPhysicalEVMin                  = std::log2(0.1f * 0.1f * 0.1f);
const float           kPhysicalEVMax                  = std::log2(100000.0f * 100.0f * 100.0f);
constexpr int         kToneMapOperatorMin             = static_cast<int>(RTXPTToneMapperOperator::Linear);
constexpr int         kToneMapOperatorMax             = static_cast<int>(RTXPTToneMapperOperator::Aces);
constexpr int         kExposureModeMin                = static_cast<int>(RTXPTExposureMode::AperturePriority);
constexpr int         kExposureModeMax                = static_cast<int>(RTXPTExposureMode::ShutterPriority);
static_assert(kToneMapOperatorMin == 0 && kToneMapOperatorMax == 5, "Tone-map UI assumes contiguous operator values");
static_assert(kExposureModeMin == 0 && kExposureModeMax == 1, "Tone-map UI assumes contiguous exposure mode values");

constexpr const char* kRTXPTRealtimeRoutePendingReason =
    "Realtime mode uses standalone NRD when available; otherwise it uses no-denoiser final merge and presentation.";
constexpr Uint32 kRTXPTRealtimeNoisePeriod = 8192u;

// Disk file used to persist the render state cache (compiled shaders + pipeline states)
// across runs. Resolved relative to the working directory set in Initialize().
constexpr char kStateCachePath[] = "RTXPTCache_017.bin";

bool IsStandaloneNrdAvailable()
{
    return RTXPTIsNrdAvailable();
}

bool ShouldRunStandaloneNrd(const RTXPTRealtimeSettings& RealtimeUI)
{
    return RealtimeUI.ActualUseStandaloneDenoiser() && IsStandaloneNrdAvailable();
}

const char* GetStandaloneNrdDisabledReason()
{
    return RTXPTGetNrdUnavailableReason();
}

const char* GetStandaloneNrdControlsDisabledReason(const RTXPTRealtimeSettings& RealtimeUI)
{
    if (!RealtimeUI.RealtimeMode)
        return "Not available in reference mode.";
    if (RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR)
        return "Standalone NRD is disabled for DLSS-RR.";
    if (!IsStandaloneNrdAvailable())
        return GetStandaloneNrdDisabledReason();
    if (!RealtimeUI.ActualUseStandaloneDenoiser())
        return "Enable standalone denoiser (NRD) to edit these controls.";
    return "";
}

const char* GetRealtimeAAModeName(RTXPTRealtimeAAMode Mode)
{
    switch (Mode)
    {
        case RTXPTRealtimeAAMode::Disabled: return "Disabled";
        case RTXPTRealtimeAAMode::TAA: return "TAA";
        case RTXPTRealtimeAAMode::SuperResolution: return "Super Resolution";
        case RTXPTRealtimeAAMode::DLSSRR: return "DLSS-RR";
        default: return "Unknown";
    }
}

bool IsRealtimeSuperResolutionSelected(const RTXPTRealtimeSettings& RealtimeUI)
{
    return RealtimeUI.RealtimeMode && RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::SuperResolution;
}

Int32 GetFirstTemporalSuperResolutionVariantIdx(const RTXPTSuperResolutionPass& SuperResolutionPass)
{
    const auto& Variants = SuperResolutionPass.GetVariants();
    for (size_t VariantIdx = 0; VariantIdx < Variants.size(); ++VariantIdx)
    {
        if (Variants[VariantIdx].Type == SUPER_RESOLUTION_TYPE_TEMPORAL)
            return static_cast<Int32>(VariantIdx);
    }
    return 0;
}

// Maps the UI quality selection onto the provider-agnostic Diligent optimization type.
SUPER_RESOLUTION_OPTIMIZATION_TYPE ToSuperResolutionOptimizationType(RTXPTSuperResolutionQuality Quality)
{
    switch (Quality)
    {
        case RTXPTSuperResolutionQuality::MaxQuality: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_MAX_QUALITY;
        case RTXPTSuperResolutionQuality::HighQuality: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_QUALITY;
        case RTXPTSuperResolutionQuality::Balanced: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_BALANCED;
        case RTXPTSuperResolutionQuality::HighPerformance: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_PERFORMANCE;
        case RTXPTSuperResolutionQuality::MaxPerformance: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_MAX_PERFORMANCE;
        default: return SUPER_RESOLUTION_OPTIMIZATION_TYPE_BALANCED;
    }
}

// Resolves the persisted provider selection to a usable variant index. The realtime path only
// runs temporal upscalers, so a stale/out-of-range/spatial selection falls back to the first
// temporal variant. Shared by the UI (combo preview) and the per-frame settings builder.
Int32 ResolveActiveSuperResolutionVariantIdx(const RTXPTRealtimeSettings&    RealtimeUI,
                                             const RTXPTSuperResolutionPass& SuperResolutionPass)
{
    const auto& Variants   = SuperResolutionPass.GetVariants();
    const Int32 VariantIdx = RealtimeUI.SuperResolutionVariantIdx;
    if (VariantIdx >= 0 && VariantIdx < static_cast<Int32>(Variants.size()) &&
        Variants[static_cast<size_t>(VariantIdx)].Type == SUPER_RESOLUTION_TYPE_TEMPORAL)
        return VariantIdx;
    return GetFirstTemporalSuperResolutionVariantIdx(SuperResolutionPass);
}

RTXPTSuperResolutionSettings MakeRealtimeSuperResolutionSettings(const RTXPTRealtimeSettings&    RealtimeUI,
                                                                 const RTXPTSuperResolutionPass& SuperResolutionPass)
{
    RTXPTSuperResolutionSettings Settings;
    Settings.Enabled = IsRealtimeSuperResolutionSelected(RealtimeUI);
    if (!Settings.Enabled)
        return Settings;

    const auto& Variants      = SuperResolutionPass.GetVariants();
    Settings.ActiveVariantIdx = ResolveActiveSuperResolutionVariantIdx(RealtimeUI, SuperResolutionPass);
    Settings.OptimizationType = ToSuperResolutionOptimizationType(RealtimeUI.SuperResolutionQuality);
    if (Settings.ActiveVariantIdx >= 0 && Settings.ActiveVariantIdx < static_cast<Int32>(Variants.size()) &&
        SuperResolutionPass.SupportsSharpness(Variants[static_cast<size_t>(Settings.ActiveVariantIdx)]))
        Settings.Sharpness = RealtimeUI.SuperResolutionSharpness;
    return Settings;
}

RTXPTTemporalAASettings MakeRealtimeTemporalAASettings()
{
    RTXPTTemporalAASettings Settings;
    Settings.TemporalStabilityFactor = 0.9375f;
    Settings.SkipRejection           = false;
    return Settings;
}

const char* GetNrdMethodName(RTXPTNrdMethod Method)
{
    switch (Method)
    {
        case RTXPTNrdMethod::REBLUR: return "REBLUR";
        case RTXPTNrdMethod::RELAX: return "RELAX";
        default: return "Unknown";
    }
}

const char* GetDenoisingGuideDebugViewName(RTXPTDenoisingGuideDebugView View)
{
    switch (View)
    {
        case RTXPTDenoisingGuideDebugView::Disabled: return "Disabled";
        case RTXPTDenoisingGuideDebugView::Depth: return "Depth";
        case RTXPTDenoisingGuideDebugView::MotionVectors: return "Motion Vectors";
        case RTXPTDenoisingGuideDebugView::SpecularHitT: return "Specular Hit T";
        case RTXPTDenoisingGuideDebugView::AvgLayerRadiance: return "Average Layer Radiance";
        case RTXPTDenoisingGuideDebugView::PrimaryLayer: return "Primary Layer";
        default: return "Unknown";
    }
}

void DrawDisabledTooltip(const char* Text)
{
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Text);
}

void InsertDenoiserPrepareOutputBarriers(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets)
{
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetOutputColorUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetCombinedHistoryClampRelaxUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserViewspaceZUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserMotionVectorsUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserNormalRoughnessUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserDiffRadianceHitDistUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserSpecRadianceHitDistUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetDenoiserDisocclusionThresholdMixUAV());
}

void InsertDenoiserFinalMergeOutputBarrier(IDeviceContext* pContext, const RTXPTRenderTargets& RenderTargets)
{
    RTXPTRayTracingPass::InsertUAVBarrier(pContext, RenderTargets.GetOutputColorUAV());
}

constexpr int kNEEMaxSampleCount = 63; // NEEBSDFMISInfo packs each sample count into six bits.

Uint32 PackEnvironmentNEEAndEmissiveTriangleCount(bool EnableEnvNEE, Uint32 EmissiveTriangleCount)
{
    const Uint32 ClampedCount = std::min(EmissiveTriangleCount, kMaxPackedEmissiveTriangleCount);
    return (ClampedCount << 1u) | (EnableEnvNEE ? 1u : 0u);
}

RTXPTRayTracingDispatch MakePathTraceDispatch(const RTXPTRenderTargets&  RenderTargets,
                                              const SampleMiniConstants& MiniConstants)
{
    RTXPTRayTracingDispatch Dispatch;
    Dispatch.pOutputColorUAV           = RenderTargets.GetOutputColorUAV();
    Dispatch.pDepthUAV                 = RenderTargets.GetDepthUAV();
    Dispatch.pMotionVectorsUAV         = RenderTargets.GetScreenMotionVectorsUAV();
    Dispatch.pThroughputUAV            = RenderTargets.GetThroughputUAV();
    Dispatch.pSpecularHitTUAV          = RenderTargets.GetSpecularHitTUAV();
    Dispatch.pStableRadianceUAV        = RenderTargets.GetStableRadianceUAV();
    Dispatch.pDenoisingLayersHeaderUAV = RenderTargets.GetDenoisingLayersHeaderUAV();
    Dispatch.pDenoisingLayersBufferUAV = RenderTargets.GetDenoisingLayersBufferUAV();
    Dispatch.pMiniConstants            = &MiniConstants;
    Dispatch.Width                     = RenderTargets.GetRenderWidth();
    Dispatch.Height                    = RenderTargets.GetRenderHeight();
    return Dispatch;
}

PathTracerCameraData MakePathTracerCameraData(const FirstPersonCamera& Camera,
                                              Uint32                   RenderWidth,
                                              Uint32                   RenderHeight,
                                              Uint32                   DisplayWidth,
                                              Uint32                   DisplayHeight,
                                              float                    FocalDistance,
                                              float                    ApertureRadius,
                                              const float2&            Jitter)
{
    const auto&  Proj              = Camera.GetProjAttribs();
    const Uint32 SafeDisplayWidth  = std::max(DisplayWidth, Uint32{1});
    const Uint32 SafeDisplayHeight = std::max(DisplayHeight, Uint32{1});
    const float  SafeRenderHeight  = static_cast<float>(std::max(RenderHeight, Uint32{1}));
    const float  AspectRatio       = static_cast<float>(SafeDisplayWidth) / static_cast<float>(SafeDisplayHeight);
    const float  SafeFocalDistance = std::max(FocalDistance, 0.001f);
    const float  TanHalfFov        = std::tan(Proj.FOV * 0.5f);

    const float3 CameraDir   = normalize(Camera.GetWorldAhead());
    const float3 CameraUp    = normalize(Camera.GetWorldUp());
    const float3 CameraW     = CameraDir * SafeFocalDistance;
    const float3 CameraUUnit = normalize(cross(CameraW, CameraUp));
    const float3 CameraVUnit = normalize(cross(CameraUUnit, CameraW));

    PathTracerCameraData Data;
    Data.PosW                 = Camera.GetPos();
    Data.NearZ                = Proj.NearClipPlane;
    Data.DirectionW           = CameraDir;
    Data.PixelConeSpreadAngle = std::atan(2.0f * TanHalfFov / SafeRenderHeight);
    Data.CameraU              = CameraUUnit * (SafeFocalDistance * TanHalfFov * AspectRatio);
    Data.FarZ                 = Proj.FarClipPlane;
    Data.CameraV              = CameraVUnit * (SafeFocalDistance * TanHalfFov);
    Data.FocalDistance        = SafeFocalDistance;
    Data.CameraW              = CameraW;
    Data.AspectRatio          = AspectRatio;
    Data.ViewportWidth        = std::max(RenderWidth, Uint32{1});
    Data.ViewportHeight       = std::max(RenderHeight, Uint32{1});
    Data.ApertureRadius       = std::max(ApertureRadius, 0.0f);
    // Match RTXPT-fork BridgeCamera jitter storage; shader helpers apply per-camera-model signs.
    Data.Jitter = Jitter * float2{1.0f, -1.0f};
    return Data;
}

Uint32 ComputeGenericTSLineStride(Uint32 ImageWidth)
{
    const Uint32 SafeWidth  = std::max(ImageWidth, Uint32{1});
    const Uint32 TileCountX = (SafeWidth + kRTXPTGenericTSTileSize - 1u) / kRTXPTGenericTSTileSize;
    return TileCountX * kRTXPTGenericTSTileSize;
}

Uint32 ComputeGenericTSPlaneStride(Uint32 ImageWidth, Uint32 ImageHeight)
{
    const Uint32 SafeHeight = std::max(ImageHeight, Uint32{1});
    const Uint32 TileCountY = (SafeHeight + kRTXPTGenericTSTileSize - 1u) / kRTXPTGenericTSTileSize;
    return ComputeGenericTSLineStride(ImageWidth) * TileCountY * kRTXPTGenericTSTileSize;
}

float ComputeSuperResolutionTexLODBias(Uint32 RenderWidth, Uint32 RenderHeight, Uint32 DisplayWidth, Uint32 DisplayHeight)
{
    const float RenderPixels  = static_cast<float>(std::max(RenderWidth, Uint32{1}) * std::max(RenderHeight, Uint32{1}));
    const float DisplayPixels = static_cast<float>(std::max(DisplayWidth, Uint32{1}) * std::max(DisplayHeight, Uint32{1}));
    return -std::log2(std::sqrt(DisplayPixels / RenderPixels));
}

PathTracerViewData MakePathTracerViewData(const float4x4& View,
                                          const float4x4& Proj,
                                          Uint32          RenderWidth,
                                          Uint32          RenderHeight,
                                          const float2&   PixelOffset)
{
    const Uint32 SafeWidth  = std::max(RenderWidth, Uint32{1});
    const Uint32 SafeHeight = std::max(RenderHeight, Uint32{1});
    const float  Width      = static_cast<float>(SafeWidth);
    const float  Height     = static_cast<float>(SafeHeight);

    const float4x4 ViewProj = View * Proj;

    PathTracerViewData Data;
    Data.MatWorldToView         = View;
    Data.MatViewToClip          = Proj;
    Data.MatWorldToClip         = ViewProj;
    Data.MatWorldToClipNoOffset = ViewProj;
    Data.MatClipToWorldNoOffset = ViewProj.Inverse();
    Data.ViewportOrigin         = float2{0.0f, 0.0f};
    Data.ViewportSize           = float2{Width, Height};
    Data.ViewportSizeInv        = float2{1.0f / Width, 1.0f / Height};
    Data.PixelOffset            = PixelOffset;
    Data.ClipToWindowScale      = float2{Width * 0.5f, -Height * 0.5f};
    Data.ClipToWindowBias       = float2{Width * 0.5f, Height * 0.5f};
    return Data;
}

RTXPTFeatureCaps MakeFeatureCaps(const IRenderDevice* pDevice)
{
    RTXPTFeatureCaps Caps{};
    const auto&      DevInfo = pDevice->GetDeviceInfo();
    const auto&      RTProps = pDevice->GetAdapterInfo().RayTracing;

    Caps.RayTracing = DevInfo.Features.RayTracing == DEVICE_FEATURE_STATE_ENABLED;
    // Always true here: ModifyEngineInitInfo hard-requires BindlessResources, so device creation would have
    // failed otherwise. Retained for the InfoGUI readout and as a defensive cap.
    Caps.BindlessResources = DevInfo.Features.BindlessResources == DEVICE_FEATURE_STATE_ENABLED;
    Caps.ComputeShaders    = DevInfo.Features.ComputeShaders == DEVICE_FEATURE_STATE_ENABLED;

    Caps.StandaloneRayTracingShaders =
        Caps.RayTracing &&
        (RTProps.CapFlags & RAY_TRACING_CAP_FLAG_STANDALONE_SHADERS) != 0;

    Caps.RayQuery =
        Caps.RayTracing &&
        (RTProps.CapFlags & RAY_TRACING_CAP_FLAG_INLINE_RAY_TRACING) != 0;

    // TODO(RTXPT-Port Phase 5): replace optimistic compiler flags with explicit DXC/ShaderMake availability checks.
    Caps.DXILCompiler  = true;
    Caps.SPIRVCompiler = true;

    return Caps;
}

RTXPTRenderTargetCreateInfo MakeRenderTargetCreateInfo(const RTXPTRenderTargetDimensions& Dimensions,
                                                       const RTXPTRealtimeSettings&       RealtimeUI,
                                                       bool                               RayTracingAvailable)
{
    RTXPTRenderTargetCreateInfo CreateInfo;
    CreateInfo.Dimensions                       = Dimensions;
    CreateInfo.Formats                          = {};
    CreateInfo.CreateComputeOutput              = false;
    CreateInfo.CreateAccumulatedRadiance        = RayTracingAvailable;
    CreateInfo.CreateRealtimeResources          = RealtimeUI.RealtimeMode;
    CreateInfo.CreateDenoiserValidation         = false;
    CreateInfo.CreateRayReconstructionResources = RealtimeUI.RealtimeMode && RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR;
    if (CreateInfo.CreateRayReconstructionResources)
        CreateInfo.CreateAccumulatedRadiance = false;
    return CreateInfo;
}

std::string JoinPath(const std::string& Root, const char* RelativePath)
{
    if (Root.empty())
        return RelativePath;

    std::string Path = Root;
    if (!FileSystem::IsSlash(Path.back()))
        Path.push_back(FileSystem::SlashSymbol);
    Path += RelativePath;
    FileSystem::CorrectSlashes(Path);
    return FileSystem::SimplifyPath(Path.c_str());
}

bool EndsWith(const std::string& Text, const char* Suffix)
{
    const std::string SuffixString{Suffix};
    return Text.size() >= SuffixString.size() &&
        Text.compare(Text.size() - SuffixString.size(), SuffixString.size(), SuffixString) == 0;
}

std::vector<std::string> EnumerateSceneFiles(const std::string& AssetsRoot)
{
    std::vector<std::string> SceneFiles;

    std::error_code             Error;
    const std::filesystem::path RootPath{AssetsRoot};
    if (!std::filesystem::is_directory(RootPath, Error))
        return SceneFiles;

    std::filesystem::directory_iterator       It{RootPath, Error};
    const std::filesystem::directory_iterator End;
    while (!Error && It != End)
    {
        const std::filesystem::directory_entry& Entry = *It;
        std::error_code                         StatusError;
        if (Entry.is_regular_file(StatusError))
        {
            const std::string FileName = Entry.path().filename().string();
            if (EndsWith(FileName, kSceneFileSuffix))
                SceneFiles.push_back(FileName);
        }
        It.increment(Error);
    }

    std::sort(SceneFiles.begin(), SceneFiles.end());
    return SceneFiles;
}

bool IsRTXPTAssetsRoot(const std::string& Path)
{
    return !EnumerateSceneFiles(Path).empty();
}

bool IsRTXPTShaderRoot(const std::string& Path)
{
    std::error_code             Error;
    const std::filesystem::path RootPath{Path};
    return std::filesystem::is_regular_file(RootPath / "shaders/RTXPTCommon.fxh", Error) &&
        std::filesystem::is_regular_file(RootPath / "shaders/PathTracer/PathTracerSample.rgen", Error);
}

std::string MakeAbsoluteRTXPTPath(const std::string& Path)
{
    std::error_code             Error;
    const std::filesystem::path AbsolutePath = std::filesystem::absolute(std::filesystem::path{Path}, Error);
    std::string                 Result       = Error ? Path : AbsolutePath.string();
    FileSystem::CorrectSlashes(Result);
    return FileSystem::SimplifyPath(Result.c_str());
}

std::string ResolveRTXPTAssetsRoot()
{
    const char* DirectCandidates[] =
        {
            ".",
            "assets",
            "Samples/RTXPT/assets",
            "DiligentSamples/Samples/RTXPT/assets",
        };

    for (const char* Candidate : DirectCandidates)
    {
        const std::string Path = FileSystem::SimplifyPath(Candidate);
        if (IsRTXPTAssetsRoot(Path))
            return MakeAbsoluteRTXPTPath(Path);
    }

    const char* AncestorPrefixes[] =
        {
            "../",
            "../../",
            "../../../",
            "../../../../",
            "../../../../../",
            "../../../../../../",
            "../../../../../../../",
            "../../../../../../../../",
        };
    const char* SourceTreeSuffixes[] =
        {
            "assets",
            "Samples/RTXPT/assets",
            "DiligentSamples/Samples/RTXPT/assets",
        };

    for (const char* Prefix : AncestorPrefixes)
    {
        for (const char* Suffix : SourceTreeSuffixes)
        {
            const std::string Path = FileSystem::SimplifyPath((std::string{Prefix} + Suffix).c_str());
            if (IsRTXPTAssetsRoot(Path))
                return MakeAbsoluteRTXPTPath(Path);
        }
    }

    return MakeAbsoluteRTXPTPath("DiligentSamples/Samples/RTXPT/assets");
}

std::string ResolveRTXPTSampleRoot(const std::string& AssetsRoot)
{
    const std::filesystem::path AssetsPath = std::filesystem::path{AssetsRoot};
    const std::filesystem::path ParentPath = AssetsPath.parent_path();

    std::vector<std::string> DirectCandidates;
    DirectCandidates.push_back(AssetsPath.string());
    if (!ParentPath.empty())
        DirectCandidates.push_back(ParentPath.string());
    DirectCandidates.push_back(".");
    DirectCandidates.push_back("Samples/RTXPT");
    DirectCandidates.push_back("DiligentSamples/Samples/RTXPT");

    for (const std::string& Candidate : DirectCandidates)
    {
        const std::string Path = FileSystem::SimplifyPath(Candidate.c_str());
        if (IsRTXPTShaderRoot(Path))
            return MakeAbsoluteRTXPTPath(Path);
    }

    const char* AncestorPrefixes[] =
        {
            "../",
            "../../",
            "../../../",
            "../../../../",
            "../../../../../",
            "../../../../../../",
            "../../../../../../../",
            "../../../../../../../../",
        };
    const char* SourceTreeSuffixes[] =
        {
            "Samples/RTXPT",
            "DiligentSamples/Samples/RTXPT",
        };

    for (const char* Prefix : AncestorPrefixes)
    {
        for (const char* Suffix : SourceTreeSuffixes)
        {
            const std::string Path = FileSystem::SimplifyPath((std::string{Prefix} + Suffix).c_str());
            if (IsRTXPTShaderRoot(Path))
                return MakeAbsoluteRTXPTPath(Path);
        }
    }

    return !ParentPath.empty() ? MakeAbsoluteRTXPTPath(ParentPath.string()) : MakeAbsoluteRTXPTPath(".");
}

void SanitizeCameraClipPlanes(float& NearPlane, float& FarPlane)
{
    if (!(NearPlane > 0.0f))
        NearPlane = kDefaultCameraNearPlane;

    if (!(FarPlane > NearPlane))
        FarPlane = NearPlane + kMinClipPlaneSeparation;
}

float ClampFinite(float Value, float Fallback, float MinValue, float MaxValue)
{
    if (!std::isfinite(Value))
        Value = Fallback;
    return std::clamp(Value, MinValue, MaxValue);
}

void SanitizeToneMappingParameters(RTXPTToneMappingParameters& Params)
{
    Params.ToneMapOperator = static_cast<RTXPTToneMapperOperator>(
        std::clamp(static_cast<int>(Params.ToneMapOperator), kToneMapOperatorMin, kToneMapOperatorMax));
    Params.ExposureMode = static_cast<RTXPTExposureMode>(
        std::clamp(static_cast<int>(Params.ExposureMode), kExposureModeMin, kExposureModeMax));

    Params.ExposureCompensation = ClampFinite(Params.ExposureCompensation, 0.0f, -12.0f, 12.0f);
    Params.ExposureValue        = ClampFinite(Params.ExposureValue, 0.0f, kPhysicalEVMin, kPhysicalEVMax);
    Params.FilmSpeed            = ClampFinite(Params.FilmSpeed, 100.0f, 1.0f, 6400.0f);
    Params.FNumber              = ClampFinite(Params.FNumber, 1.0f, 0.1f, 100.0f);
    Params.Shutter              = ClampFinite(Params.Shutter, 1.0f, 0.1f, 10000.0f);
    Params.WhitePoint           = ClampFinite(Params.WhitePoint, 6500.0f, 1905.0f, 25000.0f);
    Params.WhiteMaxLuminance    = ClampFinite(Params.WhiteMaxLuminance, 1.0f, 0.1f, std::numeric_limits<float>::max());
    Params.WhiteScale           = ClampFinite(Params.WhiteScale, 5.1f, 0.0f, 100.0f);
    Params.ExposureValueMin     = ClampFinite(Params.ExposureValueMin, -16.0f, kPhysicalEVMin, kPhysicalEVMax);
    Params.ExposureValueMax     = ClampFinite(Params.ExposureValueMax, 16.0f, kPhysicalEVMin, kPhysicalEVMax);
    if (Params.ExposureValueMin > Params.ExposureValueMax)
        std::swap(Params.ExposureValueMin, Params.ExposureValueMax);
}

void ResetToneMappingSettings(RTXPTReferenceUIState& UI)
{
    UI.EnableToneMapping           = true;
    UI.ToneMapping                 = {};
    UI.ToneMapping.ToneMapOperator = RTXPTToneMapperOperator::HableUc2;
    SanitizeToneMappingParameters(UI.ToneMapping);
}

} // namespace

SampleBase* CreateSample()
{
    return new RTXPTSample();
}

void RTXPTSample::ModifyEngineInitInfo(const ModifyEngineInitInfoAttribs& Attribs)
{
    SampleBase::ModifyEngineInitInfo(Attribs);

    Attribs.EngineCI.Features.RayTracing    = DEVICE_FEATURE_STATE_ENABLED;
    Attribs.EngineCI.Features.ShaderFloat16 = DEVICE_FEATURE_STATE_OPTIONAL;
    // Also enables shaderInt16 on Vulkan and identifies native 16-bit support on D3D12.
    Attribs.EngineCI.Features.ResourceBuffer16BitAccess = DEVICE_FEATURE_STATE_OPTIONAL;
    // Bindless material textures are mandatory: the emissive-triangle build bakes textured per-triangle radiance
    // for NEE, and the closest-hit path must sample the same textures so the NEE pdf and BSDF-hit MIS weight
    // describe the same emitter. Requiring the feature here fails device creation on backends without bindless
    // (e.g. WebGPU) rather than silently degrading to a factor-only path. Supported by default on D3D12/Vulkan.
    Attribs.EngineCI.Features.BindlessResources = DEVICE_FEATURE_STATE_ENABLED;

#if RTXPT_HAS_RAY_RECONSTRUCTION && VULKAN_SUPPORTED
    if (Attribs.DeviceType == RENDER_DEVICE_TYPE_VULKAN)
        static_cast<EngineVkCreateInfo&>(Attribs.EngineCI).EnableRayReconstruction = m_EnableVulkanRayReconstruction;
#endif

#ifdef DILIGENT_DEBUG
    Attribs.EngineCI.EnableValidation = true;
    Attribs.EngineCI.SetValidationLevel(VALIDATION_LEVEL_2);
#endif
}

RTXPTSample::CommandLineStatus RTXPTSample::ProcessCommandLine(int argc, const char* const* argv)
{
    CommandLineParser Args{argc, argv};
    Args.Parse("realtime", m_RealtimeUI.RealtimeMode);
    Args.Parse("gpu_profile", m_EnableGpuProfiling);
    Args.Parse("inline_shadows", m_EnableInlineVisibility);
    Args.Parse("hit_object", m_EnableHitObject);
    Args.Parse("thread_reorder", m_EnableThreadReorder);
    Args.Parse("fp16", m_ReferenceUI.UseFp16Types);
    Args.Parse("sharc", m_SharcSettings.Enabled);
    Args.Parse("spp", m_RealtimeUI.RealtimeSamplesPerPixel);
    Args.Parse("subsampling", m_RealtimeUI.RealtimeSubSamplingMode);
    Args.Parse("subsampling_pattern", m_RealtimeUI.RealtimeSubSamplingPattern);
    Args.Parse("subsampling_block_size", m_RealtimeUI.RealtimeSubSamplingBlockSize);
    Args.Parse("sharc_capacity_log2", m_SharcSettings.CapacityLog2);
    Args.Parse("sharc_downscale", m_SharcSettings.DownscaleFactor);
    Args.Parse("sharc_debug", m_SharcSettings.DebugMode);
    Args.Parse("sharc_update", m_SharcSettings.EnableUpdate);
    Args.Parse("sharc_resolve", m_SharcSettings.EnableResolve);
    Args.Parse("hdr_capture_path", m_HDRCapture.Path);
    Args.Parse("hdr_capture_start", m_HDRCapture.StartFrame);
    Args.Parse("hdr_capture_count", m_HDRCapture.FrameCount);
    Args.Parse("hdr_capture_processed", m_HDRCapture.Processed);
    Args.Parse("hdr_capture_sequence", m_HDRCapture.Sequence);
    Args.Parse("subsampling_validation", m_ValidateSubSampling);
    Args.Parse("exit_after_frames", m_ExitAfterFrames);
    Args.Parse("bounces", m_MaxBounces);
    m_MaxBounces = std::clamp(m_MaxBounces, 1u, static_cast<Uint32>(kMaxBounceSliderValue));
    Args.Parse("nee", m_EnableNEE);
    Args.Parse("nee_env", m_EnableEnvNEE);
    Args.Parse("nee_emissive", m_EnableEmissiveNEE);
    Args.Parse("nee_mis", m_ReferenceUI.NEEMISType);
    Args.Parse("nee_type", m_ReferenceUI.NEEType);
    Args.Parse("nee_candidates", m_ReferenceUI.NEECandidateSamples);
    m_ReferenceUI.NEECandidateSamples = std::clamp(m_ReferenceUI.NEECandidateSamples, 1, kNEEMaxSampleCount);
    m_ReferenceUI.NEEMISType          = std::clamp(m_ReferenceUI.NEEMISType, 0, 2);
    m_ReferenceUI.NEEType             = std::clamp(m_ReferenceUI.NEEType, 0, 2);
    Args.Parse("gpu_profile_csv", m_GpuProfileCSV);
    Args.Parse("scene", m_InitialSceneName);
    Args.Parse("scene_animations", m_RequestSceneAnimations);
    Args.Parse("nrd", m_RealtimeUI.StandaloneDenoiser);
    Args.Parse("rr_validation", m_ValidateRayReconstruction);
    Args.Parse("vk_rr", m_EnableVulkanRayReconstruction);
    Args.ParseEnum<RTXPTRealtimeAAMode>("aa", 0,
                                        {{"off", RTXPTRealtimeAAMode::Disabled}, {"taa", RTXPTRealtimeAAMode::TAA}, {"sr", RTXPTRealtimeAAMode::SuperResolution}, {"rr", RTXPTRealtimeAAMode::DLSSRR}}, m_RealtimeUI.RealtimeAA);
    Args.ParseEnum<RAY_RECONSTRUCTION_QUALITY>("rr_quality", 0,
                                               {{"native", RAY_RECONSTRUCTION_QUALITY_NATIVE}, {"quality", RAY_RECONSTRUCTION_QUALITY_QUALITY}, {"balanced", RAY_RECONSTRUCTION_QUALITY_BALANCED}, {"performance", RAY_RECONSTRUCTION_QUALITY_PERFORMANCE}, {"ultra_performance", RAY_RECONSTRUCTION_QUALITY_ULTRA_PERFORMANCE}}, m_RayReconstructionSettings.Quality);
    Args.ParseEnum<RAY_RECONSTRUCTION_DLSS_PRESET>("rr_preset", 0,
                                                   {{"F", RAY_RECONSTRUCTION_DLSS_PRESET_F}, {"E", RAY_RECONSTRUCTION_DLSS_PRESET_E}}, m_RayReconstructionSettings.Preset);
    return CommandLineStatus::OK;
}

void RTXPTSample::CreateFrameResources()
{
    CreateUniformBuffer(m_pDevice, sizeof(SampleConstants), "RTXPT frame constants", &m_FrameConstantsCB);
}

void RTXPTSample::EnumerateAvailableScenes()
{
    m_AvailableScenes = EnumerateSceneFiles(m_AssetsRoot);
}

void RTXPTSample::EnumerateEnvironmentMaps()
{
    m_EnvMapSources        = RTXPTEnvMapBaker::EnumerateEnvironmentSources(m_AssetsRoot);
    m_SelectedEnvMapSource = 0;
}

void RTXPTSample::ApplySceneEnvironmentSettings()
{
    m_EnvMapSettings                    = RTXPTEnvMapBaker::MakeSceneDefaultSettings(m_Scene.GetSceneGraphData());
    m_ReferenceUI.EnvironmentMapEnabled = m_EnvMapSettings.Enabled;
    m_EnvIntensity                      = m_EnvMapSettings.Intensity;

    m_SelectedEnvMapSource = 0;
    for (size_t Index = 0; Index < m_EnvMapSources.size(); ++Index)
    {
        if (m_EnvMapSources[Index].RelativePath == m_EnvMapSettings.SourceRelativePath)
        {
            m_SelectedEnvMapSource = static_cast<int>(Index);
            break;
        }
    }

    m_EnvMapBakerDirty         = true;
    m_EnvMapBakerSettingsDirty = true;
}

void RTXPTSample::ResetSceneDependentResources()
{
    m_Materials.Reset();
    m_Lights.Reset();
    m_LightsBaker.SceneReloaded();
    m_EnvMapBaker.SceneReloaded();
    m_AccelerationStructures.Reset();
    m_SkinnedGeometry.Reset();
    m_RayTracingPass.Reset();
    m_EmissiveTrianglePass.Reset();

    m_SelectedSceneCamera   = -1;
    m_EnableSceneAnimations = m_RequestSceneAnimations || !kReferencePathTraceMode;
    m_CameraVerticalFov     = PI_F / 4.0f;
    m_CameraNearPlane       = kDefaultCameraNearPlane;
    m_CameraFarPlane        = kDefaultCameraFarPlane;
    ResetToneMappingSettings(m_ReferenceUI);
    m_AccumulationFrame     = 0;
    m_AccumulationActive    = false;
    m_HasLastCameraMatrices = false;
    InvalidatePreviousFrameConstants();
    m_HasDynamicGeometry       = false;
    m_EmissiveTrianglesDirty   = true;
    m_LightsBakerSettingsDirty = false;
    m_EnvMapBakerDirty         = true;
    m_EnvMapBakerSettingsDirty = true;
}

bool RTXPTSample::UpdateEnvMapBaker(bool ForceRebuild)
{
    if (!m_pDevice || !m_pImmediateContext || !m_pEngineFactory)
        return false;

    m_EnvMapSettings.Enabled   = m_ReferenceUI.EnvironmentMapEnabled;
    m_EnvMapSettings.Intensity = m_EnvIntensity;
    if (m_SelectedEnvMapSource >= 0 && m_SelectedEnvMapSource < static_cast<int>(m_EnvMapSources.size()))
        m_EnvMapSettings.SourceRelativePath = m_EnvMapSources[static_cast<size_t>(m_SelectedEnvMapSource)].RelativePath;

    const bool Updated = m_EnvMapBaker.Update(m_pDevice, m_pImmediateContext, m_pEngineFactory,
                                              m_AssetsRoot, m_EnvMapSettings, ForceRebuild || m_EnvMapBakerDirty,
                                              m_FeatureCaps.ComputeShaders);
    if (Updated)
    {
        m_EnvMapBakerDirty         = false;
        m_EnvMapBakerSettingsDirty = false;
    }
    return Updated;
}

bool RTXPTSample::RebuildSceneDependentResources()
{
    const RTXPTSceneGraphData& SceneData = m_Scene.GetSceneGraphData();
    if (SceneData.ModelAssets.empty() || SceneData.ModelInstances.empty())
    {
        ResetSceneDependentResources();
        return false;
    }

    bool ResourcesReady = true;
    if (m_Scene.HasSkinnedGeometry() && m_Scene.HasAnimation())
    {
        // Seed the initial animated pose before scene-dependent resources snapshot transforms.
        // Reference mode keeps animation playback disabled, but still starts from frame zero.
        m_Scene.Update(0.0, 0.0);
    }

    // Emissive-textured materials are always promoted to NEE area lights: bindless is mandatory, so the
    // emissive-triangle build shader can always sample their textures on the GPU. Classification stays
    // consistent across materials, emissive-triangle counting, and the AS EmissiveTriangleOffset allocation.
    ResourcesReady &= m_Materials.Upload(m_pDevice, SceneData, m_Scene.GetAssetsRoot());
    ResourcesReady &= m_Lights.Upload(m_pDevice, SceneData);
    ResourcesReady &= m_Lights.UploadEmissiveTriangles(m_pDevice, SceneData);

    const SwapChainDesc& SCDesc = m_pSwapChain->GetDesc();
    ResourcesReady &= m_EnvMapBaker.CreateResources(m_pDevice, m_pImmediateContext, m_pEngineFactory, m_pStateCache, m_FeatureCaps.ComputeShaders);
    ResourcesReady &= UpdateEnvMapBaker(true);
    ResourcesReady &= m_LightsBaker.CreateResources(m_pDevice, m_pEngineFactory, m_pStateCache, SCDesc.Width, SCDesc.Height, m_FeatureCaps.ComputeShaders);
    ResourcesReady &= UpdateLightsBaker(true);

    ResourcesReady &= m_SkinnedGeometry.Initialize(m_pDevice,
                                                   m_pEngineFactory,
                                                   m_pStateCache,
                                                   SceneData,
                                                   m_FeatureCaps.ComputeShaders);

    m_HasDynamicGeometry     = m_SkinnedGeometry.HasSkinnedGeometry();
    m_EmissiveTrianglesDirty = true;

    if (m_SkinnedGeometry.HasSkinnedGeometry() && m_SkinnedGeometry.IsReady())
        ResourcesReady &= m_SkinnedGeometry.Update(m_pImmediateContext, SceneData);

    ResourcesReady &=
        m_AccelerationStructures.BuildScene(m_pDevice,
                                            m_pImmediateContext,
                                            SceneData,
                                            m_Scene.GetIndexType(),
                                            &m_SkinnedGeometry,
                                            m_FeatureCaps.RayTracing);

    if (ResourcesReady)
    {
        m_Scene.ClearGeometryDirty();
        CreateSceneDependentPasses();
        ResourcesReady &= BuildEmissiveTriangles();
        // Submit the initial bake before the first resize/settings update can recreate
        // its pass resources. In particular, Vulkan must not retire resources still
        // referenced by an unsubmitted initialization command buffer.
        m_pImmediateContext->Flush();
    }
    else
    {
        m_RayTracingPass.Reset();
        m_EmissiveTrianglePass.Reset();
    }
    return ResourcesReady;
}

bool RTXPTSample::SetCurrentScene(const std::string& SceneName, bool ForceReload)
{
    if (SceneName.empty())
        return false;

    if (!ForceReload && SceneName == m_CurrentSceneName)
        return m_Scene.HasValidContent();

    m_CurrentSceneName = SceneName;
    ResetSceneDependentResources();

    const bool SceneLoaded = m_Scene.LoadScene(m_pDevice, m_pImmediateContext, m_AssetsRoot, SceneName);
    if (SceneLoaded)
    {
        const RTXPTSceneSettings& SceneSettings = m_Scene.GetSceneGraphData().Settings;
        if (SceneSettings.EnableAnimations.has_value())
            m_EnableSceneAnimations = m_RequestSceneAnimations || (SceneSettings.EnableAnimations.value() && !kReferencePathTraceMode);
        if (SceneSettings.MaxBounces.has_value())
            m_MaxBounces = SceneSettings.MaxBounces.value();
        ApplySceneEnvironmentSettings();
    }

    const bool ResourcesReady = SceneLoaded && RebuildSceneDependentResources();

    auto InitializeFreeFlightCamera = [this]() {
        m_CameraVerticalFov = PI_F / 4.0f;
        m_CameraNearPlane   = kDefaultCameraNearPlane;
        m_CameraFarPlane    = kDefaultCameraFarPlane;
        InitializeCamera();
        m_SelectedSceneCamera = -1;
    };

    if (SceneLoaded)
    {
        const RTXPTSceneSettings& SceneSettings = m_Scene.GetSceneGraphData().Settings;
        if (SceneSettings.StartingCamera.has_value())
        {
            const int StartingCamera = SceneSettings.StartingCamera.value();
            if (StartingCamera < 0 || !ApplySceneCamera(static_cast<Uint32>(StartingCamera)))
                InitializeFreeFlightCamera();
        }
        else if (m_Scene.GetCameraCount() == 0 || !ApplySceneCamera(0))
        {
            InitializeFreeFlightCamera();
        }
    }
    else
    {
        InitializeFreeFlightCamera();
    }

    m_HasLastCameraMatrices = false;
    InvalidatePreviousFrameConstants();
    RequestAccumulationReset("Scene changed");
    RequestRealtimeReset(RTXPT_REALTIME_RESET_REALTIME_CACHES |
                             RTXPT_REALTIME_RESET_NRD_HISTORY |
                             RTXPT_REALTIME_RESET_TAA_SR_HISTORY,
                         "Scene changed");
    return SceneLoaded && ResourcesReady;
}

void RTXPTSample::InitializeCamera()
{
    m_Camera.SetReferenceAxes(float3{1.0f, 0.0f, 0.0f}, float3{0.0f, 1.0f, 0.0f}, true);
    m_Camera.SetPos(float3{0.0f, 1.5f, -6.0f});
    m_Camera.SetLookAt(float3{0.0f, 1.5f, 0.0f});
    m_Camera.SetRotationSpeed(0.005f);
    m_Camera.SetMoveSpeed(5.0f);
    m_Camera.SetSpeedUpScales(5.0f, 10.0f);

    const SwapChainDesc& SCDesc = m_pSwapChain->GetDesc();
    UpdateCameraProjection(SCDesc.Width, SCDesc.Height);
    m_Camera.Update(m_InputController, 0.0f);

    m_LastCameraView        = m_Camera.GetViewMatrix();
    m_LastCameraProj        = m_Camera.GetProjMatrix();
    m_HasLastCameraMatrices = true;
}

void RTXPTSample::UpdateCameraProjection(Uint32 Width, Uint32 Height)
{
    if (Width == 0 || Height == 0)
        return;

    const float AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
    m_Camera.SetProjAttribs(m_CameraNearPlane,
                            m_CameraFarPlane,
                            AspectRatio,
                            m_CameraVerticalFov,
                            m_pSwapChain->GetDesc().PreTransform,
                            m_pDevice->GetDeviceInfo().NDC.MinZ == -1);
}

bool RTXPTSample::ApplySceneCamera(Uint32 CameraIndex)
{
    const RTXPTSceneCamera* pCamera = m_Scene.GetCamera(CameraIndex);
    if (pCamera == nullptr)
        return false;

    const RTXPTCameraBasis CameraBasis = MakeRTXPTDonutCameraBasis(pCamera->Rotation);

    m_SelectedSceneCamera = static_cast<int>(CameraIndex);
    m_CameraVerticalFov   = pCamera->VerticalFov;
    if (pCamera->HasExplicitClipPlanes)
    {
        m_CameraNearPlane = pCamera->NearPlane;
        m_CameraFarPlane  = pCamera->FarPlane;
    }

    m_ReferenceUI.ToneMapping.AutoExposure         = pCamera->EnableAutoExposure.value_or(false);
    m_ReferenceUI.ToneMapping.ExposureCompensation = pCamera->ExposureCompensation.value_or(0.0f);
    m_ReferenceUI.ToneMapping.ExposureValue        = pCamera->ExposureValue.value_or(0.0f);
    m_ReferenceUI.ToneMapping.ExposureValueMin     = pCamera->ExposureValueMin.value_or(-16.0f);
    m_ReferenceUI.ToneMapping.ExposureValueMax     = pCamera->ExposureValueMax.value_or(16.0f);
    SanitizeToneMappingParameters(m_ReferenceUI.ToneMapping);

    // Align scene-camera basis with original RTXPT (Donut): Donut flips local Z for scene cameras
    // and builds the image-plane U vector as cross(CameraW, camUp). Using a left-handed
    // FirstPersonCamera reference makes ReferenceAhead equal the Donut forward vector.
    m_Camera.SetReferenceAxes(CameraBasis.Right, CameraBasis.Up, false);
    m_Camera.SetPos(pCamera->Position);
    m_Camera.SetLookAt(pCamera->Position + CameraBasis.Forward);

    const SwapChainDesc& SCDesc = m_pSwapChain->GetDesc();
    UpdateCameraProjection(SCDesc.Width, SCDesc.Height);
    m_Camera.Update(m_InputController, 0.0f);

    m_HasLastCameraMatrices = false;
    InvalidatePreviousFrameConstants();
    RequestAccumulationReset("Scene camera changed");
    RequestRealtimeReset(RTXPT_REALTIME_RESET_TAA_SR_HISTORY, "Scene camera changed");
    return true;
}

RTXPTSample::~RTXPTSample()
{
    const auto& RRStats = m_PostProcessPipeline.GetRayReconstructionPass().GetStats();
    LOG_INFO_MESSAGE("DLSS-RR totals: created=", RRStats.CreateCount, ", executed=", RRStats.ExecuteCount, ", resets=", RRStats.ResetCount);
    m_PostProcessPipeline.GetRayReconstructionPass().Reset();
    if (!m_pStateCache)
        return;

    // Persist the render state cache so the next run can reuse compiled shaders and pipelines.
    RefCntAutoPtr<IDataBlob> pCacheData;
    if (m_pStateCache->WriteToBlob(0, &pCacheData) && pCacheData)
    {
        FileWrapper CacheDataFile{kStateCachePath, EFileAccessMode::Overwrite};
        if (CacheDataFile->Write(pCacheData->GetConstDataPtr(), pCacheData->GetSize()))
            LOG_INFO_MESSAGE("Saved render state cache to '", kStateCachePath, "'");
        else
            LOG_ERROR_MESSAGE("Failed to write render state cache to '", kStateCachePath, "'");
    }
}

void RTXPTSample::Initialize(const SampleInitInfo& InitInfo)
{
    SampleBase::Initialize(InitInfo);
    m_FeatureCaps = MakeFeatureCaps(m_pDevice);
    InitializeCamera();
    CreateFrameResources();

    m_AssetsRoot                 = ResolveRTXPTAssetsRoot();
    const std::string SampleRoot = ResolveRTXPTSampleRoot(m_AssetsRoot);
    std::error_code   WorkingDirError;
    std::filesystem::current_path(SampleRoot, WorkingDirError);
    if (!WorkingDirError)
        FileSystem::SetWorkingDirectory(SampleRoot.c_str());

    // Create the render state cache (now that the working directory is set) and load any
    // previously saved cache. All passes route their shader/PSO creation through it.
    {
        RenderStateCacheCreateInfo CacheCI;
        CacheCI.pDevice          = m_pDevice;
        CacheCI.pArchiverFactory = LoadAndGetArchiverFactory();
        // RTXPT shaders use relative #includes (e.g. LightsBaker.hlsl -> "LightingConfig.h") that DXC
        // resolves against the including file's directory. The cache's by-content hash scanner instead
        // looks up bare include names against the factory's flat search dirs and cannot find them, so we
        // hash by file name + compile params instead. NOTE: editing a shader's source no longer
        // invalidates the cache automatically - delete RTXPT.cache to force a recompile after shader edits.
        CacheCI.FileHashMode = RENDER_STATE_CACHE_FILE_HASH_MODE_BY_CONTENT;
        CreateRenderStateCache(CacheCI, &m_pStateCache);
        VERIFY_EXPR(m_pStateCache);

        if (m_pStateCache && FileSystem::FileExists(kStateCachePath))
        {
            FileWrapper                 CacheDataFile{kStateCachePath};
            RefCntAutoPtr<DataBlobImpl> pCacheData = DataBlobImpl::Create();
            if (CacheDataFile->Read(pCacheData))
            {
                if (m_pStateCache->Load(pCacheData))
                    LOG_INFO_MESSAGE("Loaded render state cache from '", kStateCachePath, "'");
                else
                    LOG_ERROR_MESSAGE("Failed to load render state cache from '", kStateCachePath, "'");
            }
        }
    }

    EnumerateEnvironmentMaps();
    EnumerateAvailableScenes();
    CreateSceneIndependentPasses();

    std::string InitialScene;
    auto        PreferredIt = std::find(m_AvailableScenes.begin(), m_AvailableScenes.end(), kPreferredSceneName);
    if (PreferredIt != m_AvailableScenes.end())
        InitialScene = *PreferredIt;
    else if (!m_AvailableScenes.empty())
        InitialScene = m_AvailableScenes.front();
    if (!m_InitialSceneName.empty())
        InitialScene = m_InitialSceneName;

    if (!InitialScene.empty())
    {
        SetCurrentScene(InitialScene, true);
    }
    else
    {
        ResetSceneDependentResources();
    }

    UpdateRenderTargetDimensions(m_LastElapsedTimeSeconds);
    EnsureRenderTargets();
}

void RTXPTSample::UpdateRenderTargetDimensions(float TimeDeltaSeconds)
{
    const SwapChainDesc&               SCDesc = m_pSwapChain->GetDesc();
    const RTXPTRenderTargetFormats     Formats;
    const RTXPTSuperResolutionSettings SuperResolution =
        MakeRealtimeSuperResolutionSettings(m_RealtimeUI, m_PostProcessPipeline.GetSuperResolutionPass());

    m_CurrentSuperResolutionFrame =
        m_PostProcessPipeline.ResolveSuperResolutionFrameDesc(SuperResolution,
                                                              SCDesc.Width,
                                                              SCDesc.Height,
                                                              Formats.ProcessedOutputColor,
                                                              false,
                                                              TimeDeltaSeconds);
    m_CurrentTargetDimensions     = m_CurrentSuperResolutionFrame.Dimensions;
    auto& RR                      = m_PostProcessPipeline.GetRayReconstructionPass();
    m_RayReconstructionFrameReady = false;
    if (m_RealtimeUI.RealtimeMode && m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR)
    {
        m_RayReconstructionFrameReady = RR.PrepareFrame(m_pImmediateContext, m_RayReconstructionSettings,
                                                        SCDesc.Width, SCDesc.Height, m_FrameIndex);
        if (m_RayReconstructionFrameReady)
            m_CurrentTargetDimensions = RTXPTRayReconstructionInputs::MakeRenderTargetCreateInfo(RR.GetDesc()).Dimensions;
    }
    else
        RR.Deactivate();
}

void RTXPTSample::UpdateFrameConstants(double CurrTime)
{
    const Uint32 RenderWidth   = m_CurrentTargetDimensions.RenderWidth;
    const Uint32 RenderHeight  = m_CurrentTargetDimensions.RenderHeight;
    const Uint32 DisplayWidth  = m_CurrentTargetDimensions.DisplayWidth;
    const Uint32 DisplayHeight = m_CurrentTargetDimensions.DisplayHeight;
    const float  Width         = static_cast<float>(RenderWidth);
    const float  Height        = static_cast<float>(RenderHeight);

    SanitizeRealtimeSettings(m_RealtimeUI);
    BeginRealtimeFrameResetScope();

    const bool   RealtimeMode = m_RealtimeUI.RealtimeMode;
    const Uint32 ActualSPP    = std::max(m_RealtimeUI.ActualSamplesPerPixel(), 1u);

    m_CurrentSuperResolutionFrame.ResetHistory =
        HasRealtimeResetFlag(m_CurrentFrameRealtimeReset, RTXPT_REALTIME_RESET_TAA_SR_HISTORY);

    float2 CameraJitter = float2{0.0f, 0.0f};
    if (RealtimeMode && m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::TAA)
    {
        CameraJitter = m_PostProcessPipeline.GetRealtimeTAAPixelJitter(m_FrameIndex);
    }
    else if (RealtimeMode &&
             m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::SuperResolution &&
             m_CurrentSuperResolutionFrame.Enabled)
    {
        CameraJitter = m_CurrentSuperResolutionFrame.Jitter;
    }
    m_CurrentRealtimeCameraJitter = CameraJitter;
    if (RealtimeMode && m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR && m_RayReconstructionFrameReady)
        CameraJitter = m_CurrentRealtimeCameraJitter = m_PostProcessPipeline.GetRayReconstructionPass().GetJitter();

    const float3   CameraPosition = m_Camera.GetPos();
    const float4x4 CameraView     = m_Camera.GetViewMatrix();
    const float4x4 CameraProj     = m_Camera.GetProjMatrix();
    const float4x4 ViewProj       = CameraView * CameraProj;

    const PathTracerCameraData CurrentCamera  = MakePathTracerCameraData(m_Camera,
                                                                        RenderWidth,
                                                                        RenderHeight,
                                                                        DisplayWidth,
                                                                        DisplayHeight,
                                                                        m_ReferenceUI.CameraFocalDistance,
                                                                        m_ReferenceUI.CameraAperture,
                                                                        CameraJitter);
    const PathTracerViewData   CurrentView    = MakePathTracerViewData(CameraView,
                                                                  CameraProj,
                                                                  RenderWidth,
                                                                  RenderHeight,
                                                                  CameraJitter);
    const PathTracerCameraData PreviousCamera = m_HasPreviousFrameConstants ? m_PreviousFrameCamera : CurrentCamera;
    const PathTracerViewData   PreviousView   = m_HasPreviousFrameConstants ? m_PreviousFrameView : CurrentView;

    m_LastFrameConstants.viewProj                  = ViewProj;
    m_LastFrameConstants.viewProjInv               = ViewProj.Inverse();
    m_LastFrameConstants.cameraPositionAndTime     = float4{CameraPosition.x, CameraPosition.y, CameraPosition.z, static_cast<float>(CurrTime)};
    m_LastFrameConstants.viewportSizeAndFrameIndex = float4{Width, Height, Width > 0.0f ? 1.0f / Width : 0.0f, static_cast<float>(m_FrameIndex)};
    m_LastFrameConstants.view                      = CurrentView;
    m_LastFrameConstants.previousView              = PreviousView;
    m_LastFrameConstants.camera                    = CurrentCamera;

    if (m_AccumulationActive)
    {
        if (m_ResetAccumulationPending)
            m_AccumulationFrame = 1;
        else
            ++m_AccumulationFrame;
    }
    else
    {
        m_AccumulationFrame = 0;
    }

    const Uint32 ReferenceSampleIndex = m_AccumulationFrame;
    const Uint32 PathTraceSampleIndex = RealtimeMode ? (m_FrameIndex % kRTXPTRealtimeNoisePeriod) : ReferenceSampleIndex;
    const Uint32 SampleBaseIndex      = PathTraceSampleIndex * ActualSPP;
    m_RealtimeSampleIndex             = RealtimeMode ? PathTraceSampleIndex : 0u;
    m_LastSampleBaseIndex             = SampleBaseIndex;

    PathTracerConstants& PtConsts = m_LastFrameConstants.ptConsts;
    PtConsts.imageWidth           = RenderWidth;
    PtConsts.imageHeight          = RenderHeight;
    PtConsts.sampleBaseIndex      = SampleBaseIndex;
    PtConsts.perPixelJitterAAScale =
        RealtimeMode ?
        (m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR ? m_RealtimeUI.DLSSRRMicroJitter : 0.0f) :
        1.0f;

    PtConsts.bounceCount                         = m_MaxBounces;
    PtConsts.diffuseBounceCount                  = static_cast<Uint32>(std::clamp(m_ReferenceUI.DiffuseBounceCount, 0, 16));
    PtConsts.EnvironmentMapDiffuseSampleMIPLevel = 0.0f;
    PtConsts.texLODBias                          = RealtimeMode ?
        (m_RealtimeUI.TexLODBias + ComputeSuperResolutionTexLODBias(RenderWidth, RenderHeight, DisplayWidth, DisplayHeight)) :
        0.0f;

    PtConsts.invSubSampleCount = 1.0f / static_cast<float>(ActualSPP);
    PtConsts.preExposedGrayLuminance =
        m_PostProcessPipeline.ComputePreExposedGrayLuminance(m_ReferenceUI.ToneMapping, m_ReferenceUI.EnableToneMapping);
    const float DisabledFireflyThreshold = 0.0f;
    if (RealtimeMode)
    {
        PtConsts.fireflyFilterThreshold = m_RealtimeUI.RealtimeFireflyFilterEnabled ?
            m_RealtimeUI.RealtimeFireflyFilterThreshold * std::sqrt(PtConsts.preExposedGrayLuminance) * 1000.0f :
            DisabledFireflyThreshold;
    }
    else
    {
        PtConsts.fireflyFilterThreshold = m_ReferenceUI.ReferenceFireflyFilterEnabled ?
            m_ReferenceUI.ReferenceFireflyFilterThreshold :
            DisabledFireflyThreshold;
    }
    PtConsts.denoisingEnabled = RealtimeMode && (m_RealtimeUI.ActualUseStandaloneDenoiser() || m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR) ?
        1u :
        0u;

    PtConsts.frameIndex        = m_FrameIndex;
    PtConsts.useReSTIRDI       = 0u;
    PtConsts.useReSTIRGI       = 0u;
    PtConsts.resetAccumulation = m_ResetAccumulationPending ? 1u : 0u;

    PtConsts.denoisingLayersSplitStopThreshold = m_RealtimeUI.DenoisingLayersSplitStopThreshold;
    PtConsts._padding3                         = 0.0f;
    PtConsts._padding4                         = 0u;
    PtConsts.denoisingLayersSuppressPrimaryIndirectSpecularK =
        m_RealtimeUI.DenoisingLayersSuppressPrimaryIndirectSpecular ?
        m_RealtimeUI.DenoisingLayersSuppressPrimaryIndirectSpecularK :
        0.0f;

    PtConsts.denoiserRadianceClampK = m_RealtimeUI.DenoiserRadianceClampK;
    PtConsts.DLSSRRBrightnessClampK = m_RealtimeUI.DLSSRRBrightnessClampK > 0.0f ?
        m_RealtimeUI.DLSSRRBrightnessClampK * PtConsts.preExposedGrayLuminance :
        0.0f;
    PtConsts.denoisingLayersAntiAliasingFallthrough = m_RealtimeUI.DenoisingLayersAntiAliasingFallthrough;
    PtConsts._activeDenoisingLayerCount =
        static_cast<Uint32>(std::clamp(m_RealtimeUI.DenoisingLayersActiveCount, Int32{1}, static_cast<Int32>(kRTXPTDenoisingLayerCount)));

    PtConsts.maxDenoisingLayerVertexDepth =
        std::min(static_cast<Uint32>(std::clamp(m_RealtimeUI.DenoisingLayersMaxVertexDepth,
                                                Int32{2},
                                                static_cast<Int32>(kRTXPTDenoisingLayerMaxVertexIndex))),
                 m_MaxBounces);
    PtConsts.allowPrimarySurfaceReplacement = m_RealtimeUI.AllowPrimarySurfaceReplacement ? 1u : 0u;
    PtConsts.genericTSLineStride            = ComputeGenericTSLineStride(RenderWidth);
    PtConsts.genericTSPlaneStride           = ComputeGenericTSPlaneStride(RenderWidth, RenderHeight);

    PtConsts.NEEEnabled          = m_EnableNEE ? 1u : 0u;
    PtConsts.NEEType             = static_cast<Uint32>(std::clamp(m_ReferenceUI.NEEType, 0, 2));
    PtConsts.NEECandidateSamples = static_cast<Uint32>(std::clamp(m_ReferenceUI.NEECandidateSamples, 1, kNEEMaxSampleCount));
    PtConsts.NEEFullSamples      = static_cast<Uint32>(std::clamp(m_ReferenceUI.NEEFullSamples, 0, kNEEMaxSampleCount));

    PtConsts.sampleIndex    = PathTraceSampleIndex;
    PtConsts.minBounceCount = m_MinBounces;
    const Uint32 EmissiveTriangleCount =
        (m_EnableNEE && m_EnableEmissiveNEE && m_EmissiveTrianglePass.IsReady() && !m_EmissiveTrianglesDirty) ?
        m_Lights.GetEmissiveTriangleCount() :
        0u;
    PtConsts.environmentNEEEnabled = PackEnvironmentNEEAndEmissiveTriangleCount(m_EnableEnvNEE, EmissiveTriangleCount);
    PtConsts.environmentIntensity  = m_EnvIntensity;

    PtConsts.lightIntensityScale      = m_LightIntensityScale;
    PtConsts.maxNEEBounceCount        = m_MaxNEEBounces;
    PtConsts.analyticLightCount       = m_Lights.GetStats().LightCount;
    PtConsts.NEEMISType               = static_cast<Uint32>(std::clamp(m_ReferenceUI.NEEMISType, 0, 2));
    PtConsts.nestedDielectricsQuality = static_cast<Uint32>(std::clamp(m_ReferenceUI.NestedDielectricsQuality, 0, 2));
    PtConsts.superResolutionActive =
        RealtimeMode && m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::SuperResolution &&
            m_CurrentSuperResolutionFrame.Enabled ?
        1u :
        0u;
    PtConsts._paddingR6_1       = 0u;
    PtConsts._paddingR6_2       = 0u;
    PtConsts.camera             = CurrentCamera;
    PtConsts.prevCamera         = PreviousCamera;
    m_LastFrameConstants.envMap = m_EnvMapBaker.GetConstants();

    if (m_FrameConstantsCB)
    {
        MapHelper<SampleConstants> Constants{m_pImmediateContext, m_FrameConstantsCB, MAP_WRITE, MAP_FLAG_DISCARD};
        *Constants = m_LastFrameConstants;
    }

    m_PreviousFrameCamera       = CurrentCamera;
    m_PreviousFrameView         = CurrentView;
    m_HasPreviousFrameConstants = true;

    m_ResetAccumulationPending = false;

    ++m_FrameIndex;
}

bool RTXPTSample::UpdateLightsBaker(bool ResetFeedback)
{
    if (!m_pDevice || !m_pSwapChain || !m_Scene.HasValidContent())
        return false;

    const SwapChainDesc&     SCDesc = m_pSwapChain->GetDesc();
    RTXPTLightsBakerSettings BakerSettings;
    BakerSettings.ImportanceSamplingType        = static_cast<Uint32>(std::clamp(m_ReferenceUI.NEEType, 0, 2));
    BakerSettings.CameraPosition                = m_Camera.GetPos();
    BakerSettings.ViewProjMatrix                = m_Camera.GetViewMatrix() * m_Camera.GetProjMatrix();
    BakerSettings.GlobalTemporalFeedbackWeight  = m_ReferenceUI.NEEAT_GlobalTemporalFeedbackWeight;
    BakerSettings.LocalToGlobalSampleRatio      = m_ReferenceUI.NEEAT_LocalToGlobalSampleRatio;
    BakerSettings.DistantVsLocalImportanceScale = m_ReferenceUI.NEEAT_DistantVsLocalImportance;
    BakerSettings.ResetFeedback                 = ResetFeedback;
    BakerSettings.ViewportSize                  = float2{static_cast<float>(SCDesc.Width), static_cast<float>(SCDesc.Height)};
    BakerSettings.PrevViewportSize              = BakerSettings.ViewportSize;
    BakerSettings.EnvMapParams                  = m_EnvMapBaker.GetLightsBakerParams();
    BakerSettings.pEnvironmentRadianceMap       = m_EnvMapBaker.GetRadianceMapSRV();
    BakerSettings.EnvMapImportanceMapResolution = m_EnvMapBaker.GetStats().ImportanceResolution;
    BakerSettings.EnvMapImportanceMapMipCount   = m_EnvMapBaker.GetStats().ImportanceMipLevels;
    BakerSettings.FrameIndex                    = static_cast<Int64>(m_FrameIndex);

    const bool Updated = m_LightsBaker.UpdateBegin(m_pDevice, m_Lights, BakerSettings);
    if (Updated)
        m_LightsBakerSettingsDirty = false;
    else
        m_LightsBakerSettingsDirty = true;
    return Updated;
}

void RTXPTSample::CreateSceneIndependentPasses()
{
    m_BlitPass.Initialize(m_pDevice, m_pEngineFactory, m_pStateCache, m_pSwapChain);
    m_PostProcessPipeline.Initialize(m_pDevice,
                                     m_pEngineFactory,
                                     m_pStateCache,
                                     m_pSwapChain,
                                     m_FrameConstantsCB,
                                     m_FeatureCaps.ComputeShaders);
    m_DenoisingGuidesBaker.Initialize(m_pDevice,
                                      m_pEngineFactory,
                                      m_pStateCache,
                                      m_FrameConstantsCB,
                                      m_FeatureCaps.ComputeShaders);
}

void RTXPTSample::CreateSceneDependentPasses()
{
    if (!m_AdaptiveSampling.Initialize(m_pDevice, m_pEngineFactory, m_pStateCache, m_FrameConstantsCB))
        LOG_ERROR_MESSAGE("SubSampling initialization failed");
    m_Sharc.Invalidate();
    const bool EnableSharc = m_SharcSettings.Enabled && SharcIntegration::IsSupported(m_pDevice);
    if (m_SharcSettings.Enabled && !EnableSharc)
        LOG_WARNING_MESSAGE("SHaRC unavailable: requires D3D12 SM 6.6 and native FP16; Vulkan 64-bit atomics are not enabled by this backend. Rendering without cache.");
    if (EnableSharc && !m_Sharc.Initialize(m_pDevice, m_pEngineFactory, m_pStateCache))
        LOG_ERROR_MESSAGE("SHaRC initialization failed");
    // Build the textured shader variants whenever the scene actually carries material textures. Bindless is
    // mandatory (required in ModifyEngineInitInfo), so descriptor indexing is always available; the only
    // factor-only path left is the legitimate texture-less scene (MaterialTextureCount == 0).
    const bool  EnableMaterialTextures = m_Materials.GetTextureCount() > 0;
    const bool  EnableAnyHit           = EnableMaterialTextures || m_AccelerationStructures.GetStats().ExcludedFromNEEGeometryCount > 0;
    const auto& Features               = m_pDevice->GetDeviceInfo().Features;
    const bool  UseFp16Types           = m_ReferenceUI.UseFp16Types &&
        Features.ShaderFloat16 == DEVICE_FEATURE_STATE_ENABLED &&
        Features.ResourceBuffer16BitAccess == DEVICE_FEATURE_STATE_ENABLED;
    LOG_INFO_MESSAGE("RTXPT native FP16: ", UseFp16Types ? "enabled" : "disabled",
                     "; requested: ", m_ReferenceUI.UseFp16Types ? "yes" : "no");

    m_EmissiveTrianglePass.Initialize(m_pDevice,
                                      m_pEngineFactory,
                                      m_pStateCache,
                                      m_Materials.GetMaterialBuffer(),
                                      m_AccelerationStructures.GetSubInstanceBuffer(),
                                      m_AccelerationStructures.GetSubInstanceTransformBuffer(),
                                      m_Scene.GetVertexBuffer0(m_pDevice, m_pImmediateContext),
                                      m_SkinnedGeometry.GetSkinnedVertexBuffer(),
                                      m_Scene.GetIndexBuffer(m_pDevice, m_pImmediateContext),
                                      m_Scene.GetIndexType(),
                                      m_Lights.GetEmissiveTriangleBuffer(),
                                      m_Materials.GetTextureBindings(),
                                      m_Materials.GetTextureCount(),
                                      EnableMaterialTextures,
                                      m_FeatureCaps.ComputeShaders);

    const bool RTReady =
        m_RayTracingPass.Initialize(m_pDevice,
                                    m_pImmediateContext,
                                    m_pEngineFactory,
                                    m_pStateCache,
                                    m_FrameConstantsCB,
                                    m_Materials.GetMaterialBuffer(),
                                    m_AccelerationStructures.GetSubInstanceBuffer(),
                                    m_LightsBaker.GetLightBuffer(),
                                    m_LightsBaker.GetLightExBuffer(),
                                    m_LightsBaker.GetControlBuffer(),
                                    m_LightsBaker.GetLightProxyCounters(),
                                    m_LightsBaker.GetLightSamplingProxies(),
                                    m_LightsBaker.GetLocalSamplingBuffer(),
                                    m_LightsBaker.GetEnvironmentLightLookup(),
                                    m_LightsBaker.GetFeedbackTotalWeightUAV(),
                                    m_LightsBaker.GetFeedbackCandidatesUAV(),
                                    m_EnvMapBaker.GetEnvironmentMapSRV(),
                                    m_EnvMapBaker.GetImportanceMapSRV(),
                                    m_EnvMapBaker.GetRadianceMapSRV(),
                                    m_EnvMapBaker.GetEnvironmentSampler(),
                                    m_EnvMapBaker.GetImportanceSampler(),
                                    m_Scene.GetVertexBuffer0(m_pDevice, m_pImmediateContext),
                                    m_SkinnedGeometry.GetSkinnedVertexBuffer(),
                                    m_Scene.GetIndexBuffer(m_pDevice, m_pImmediateContext),
                                    m_Scene.GetIndexType(),
                                    m_AccelerationStructures.GetTLAS(),
                                    m_Materials.GetTextureBindings(),
                                    m_Materials.GetTextureCount(),
                                    EnableMaterialTextures,
                                    EnableAnyHit,
                                    m_ReferenceUI.EnableLDSamplerForBSDF,
                                    m_FeatureCaps.RayTracing,
                                    m_FeatureCaps.StandaloneRayTracingShaders,
                                    m_EnableInlineVisibility,
                                    static_cast<Uint32>(m_ReferenceUI.NEEMISType),
                                    static_cast<Uint32>(std::clamp(m_ReferenceUI.NEECandidateSamples, 1, kNEEMaxSampleCount)),
                                    static_cast<Uint32>(std::clamp(m_ReferenceUI.NEEFullSamples, 0, kNEEMaxSampleCount)),
                                    UseFp16Types,
                                    m_EnableHitObject,
                                    m_EnableThreadReorder,
                                    EnableSharc);

    if (!RTReady)
    {
        // Bindless is mandatory, so the textured ray-tracing pipeline must build; there is no factor-only
        // capability fallback. A failure here leaves m_RayTracingPass un-ready and the scene won't render.
        LOG_ERROR_MESSAGE("RTXPT ray-tracing pipeline failed to initialize; the scene cannot be path traced");
    }
}

bool RTXPTSample::BuildEmissiveTriangles()
{
    const Uint32 EmissiveTriangleCount = m_Lights.GetEmissiveTriangleCount();
    if (EmissiveTriangleCount == 0u)
    {
        m_EmissiveTrianglesDirty = false;
        return true;
    }

    if (!m_EmissiveTrianglePass.IsReady() || m_AccelerationStructures.GetStats().SubInstanceCount == 0)
        return false;

    const bool Executed = m_EmissiveTrianglePass.Dispatch(m_pImmediateContext, m_AccelerationStructures.GetStats().SubInstanceCount);
    if (Executed)
        m_EmissiveTrianglesDirty = false;
    return Executed;
}

bool RTXPTSample::EnsureRenderTargets()
{
    const bool                        WasValid              = m_RenderTargets.IsValid();
    const RTXPTRenderTargetDimensions OldDimensions         = m_RenderTargets.GetDimensions();
    const bool                        WasAccumulationActive = m_AccumulationActive;
    const bool                        WasRealtimeRequested  = m_RenderTargets.AreRealtimeRenderTargetsRequested();
    const bool                        WasRealtimeAllocated  = m_RenderTargets.HasRealtimeRenderTargets();
    const bool                        WasRR                 = m_RenderTargets.HasRayReconstructionRenderTargets();

    SanitizeRealtimeSettings(m_RealtimeUI);
    const RTXPTRenderTargetCreateInfo CreateInfo =
        MakeRenderTargetCreateInfo(m_CurrentTargetDimensions, m_RealtimeUI, m_FeatureCaps.RayTracing);
    if (WasRR && (!(OldDimensions == CreateInfo.Dimensions) || !CreateInfo.CreateRayReconstructionResources))
    {
        m_pImmediateContext->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
        m_pImmediateContext->Flush();
        m_pImmediateContext->WaitForIdle();
    }
    const bool Ok             = m_RenderTargets.Resize(m_pDevice, CreateInfo);
    const bool ResourcesValid = m_PostProcessPipeline.ValidateRenderTargets(m_RenderTargets);

    m_AccumulationActive =
        Ok && ResourcesValid &&
        m_RenderTargets.IsAccumulationActive() &&
        m_PostProcessPipeline.GetStats().AccumulationStageReady;
    if (Ok && ResourcesValid &&
        (!WasValid ||
         !(OldDimensions == m_CurrentTargetDimensions) ||
         WasAccumulationActive != m_AccumulationActive ||
         WasRealtimeRequested != m_RenderTargets.AreRealtimeRenderTargetsRequested() ||
         WasRR != m_RenderTargets.HasRayReconstructionRenderTargets() ||
         WasRealtimeAllocated != m_RenderTargets.HasRealtimeRenderTargets()))
    {
        RequestAccumulationReset("Render targets (re)created");
        RequestRealtimeReset(RTXPT_REALTIME_RESET_RENDER_TARGET_RECREATE |
                                 RTXPT_REALTIME_RESET_REALTIME_CACHES |
                                 RTXPT_REALTIME_RESET_NRD_HISTORY |
                                 RTXPT_REALTIME_RESET_TAA_SR_HISTORY,
                             "Render targets (re)created");
        ResetNrdIntegrations();
        InvalidatePreviousFrameConstants();
    }
    return Ok && ResourcesValid;
}

void RTXPTSample::ClearFallback(const float4& ClearColor)
{
    ITextureView* pRTV = m_pSwapChain->GetCurrentBackBufferRTV();
    m_pImmediateContext->SetRenderTargets(1, &pRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    m_pImmediateContext->ClearRenderTarget(pRTV, ClearColor.Data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void RTXPTSample::RecordRealtimePathTraceStatus(const char* Status)
{
    m_RealtimePathTraceStatus = Status != nullptr ? Status : "";
}

bool RTXPTSample::DispatchPathTracePrePass(const RTXPTRayTracingDispatch& BaseDispatch)
{
    RTXPTGpuProfiler::Scope Timing{&m_GpuProfiler, m_pImmediateContext, RTXPTGpuProfiler::Stage::Build};
    if (!m_RenderTargets.HasRealtimeRenderTargets())
    {
        RecordRealtimePathTraceStatus("Realtime render targets are not allocated");
        return false;
    }

    const bool PrePassOk =
        m_RayTracingPass.Dispatch(m_pImmediateContext,
                                  RTXPTPathTraceVariant::BuildDenoisingLayers,
                                  BaseDispatch);
    if (!PrePassOk)
    {
        RecordRealtimePathTraceStatus("BUILD_DENOISING_LAYERS dispatch failed");
        return false;
    }

    RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetDenoisingLayersBuffer());
    RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetDenoisingLayersHeaderUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetStableRadianceUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetDepthUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetScreenMotionVectorsUAV());
    RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetThroughputUAV());

    return true;
}

bool RTXPTSample::PathTrace()
{
    m_SharcActive = m_SharcSettings.Enabled && m_RealtimeUI.RealtimeMode &&
        SharcIntegration::IsSupported(m_pDevice) && m_RayTracingPass.IsReady(RTXPTPathTraceVariant::SharcFill);
    if (!m_SharcActive)
    {
        m_RayTracingPass.UnbindSharcBuffers();
        m_Sharc.Reset();
    }
    m_LastRealtimePathTraceExecuted = false;
    m_LastRealtimeFinalMergeReady   = false;
    RecordRealtimePathTraceStatus("");

    SampleMiniConstants           MiniConstants      = {};
    const RTXPTRayTracingDispatch BaseDispatch       = MakePathTraceDispatch(m_RenderTargets, MiniConstants);
    const bool                    UseDenoisingLayers = m_RealtimeUI.RealtimeMode;

    // BUILD can shade analytic proxies too; expose current packed light data before either RT pass.
    if (!m_LightsBaker.PrepareLightBuffers(m_pImmediateContext))
        return false;

    // PathTracePrePass dispatches BuildDenoisingLayers and synchronizes exported realtime state.
    if (UseDenoisingLayers && !DispatchPathTracePrePass(BaseDispatch))
        return false;

    m_GpuProfiler.Begin(m_pImmediateContext, RTXPTGpuProfiler::Stage::LightsBaker);
    m_LightsBaker.UpdateFrame(m_Camera.GetViewMatrix() * m_Camera.GetProjMatrix(), m_Camera.GetPos());
    const bool LightsOk = m_LightsBaker.UpdateEnd(m_pImmediateContext,
                                                  m_RenderTargets.GetDepthSRV(),
                                                  m_RenderTargets.GetScreenMotionVectorsSRV());
    m_GpuProfiler.End(m_pImmediateContext, RTXPTGpuProfiler::Stage::LightsBaker);
    if (!LightsOk)
    {
        RecordRealtimePathTraceStatus("LightsBaker UpdateEnd failed");
        return false;
    }

    if (m_SharcActive)
    {
        if (!m_Sharc.Prepare(m_pDevice, m_pImmediateContext, m_SharcSettings,
                             BaseDispatch.Width, BaseDispatch.Height, m_Camera.GetPos()))
        {
            RecordRealtimePathTraceStatus("SHaRC resource preparation failed");
            LOG_ERROR_MESSAGE("SHaRC resource preparation failed");
            return false;
        }
        if (m_SharcSettings.EnableUpdate)
        {
            RTXPTGpuProfiler::Scope Timing{&m_GpuProfiler, m_pImmediateContext, RTXPTGpuProfiler::Stage::SharcUpdate};
            SampleConstants         Training     = m_LastFrameConstants;
            Training.ptConsts.bounceCount        = 8;
            Training.ptConsts.diffuseBounceCount = 4;
            Training.ptConsts.texLODBias += 1.f;
            {
                MapHelper<SampleConstants> CB{m_pImmediateContext, m_FrameConstantsCB, MAP_WRITE, MAP_FLAG_DISCARD};
                *CB = Training;
            }
            RTXPTRayTracingDispatch Dispatch = BaseDispatch;
            const Uint32            Scale    = std::clamp(m_SharcSettings.DownscaleFactor, 2, 5);
            Dispatch.Width                   = (Dispatch.Width + Scale - 1) / Scale;
            Dispatch.Height                  = (Dispatch.Height + Scale - 1) / Scale;
            m_Sharc.Bind(Dispatch);
            const bool Trained = m_RayTracingPass.Dispatch(m_pImmediateContext, RTXPTPathTraceVariant::SharcTrain, Dispatch);
            {
                MapHelper<SampleConstants> CB{m_pImmediateContext, m_FrameConstantsCB, MAP_WRITE, MAP_FLAG_DISCARD};
                *CB = m_LastFrameConstants;
            }
            if (!Trained)
            {
                RecordRealtimePathTraceStatus("SHaRC training dispatch failed");
                LOG_ERROR_MESSAGE("SHaRC training dispatch failed");
                return false;
            }
            m_Sharc.Barrier(m_pImmediateContext);
        }
        if (m_SharcSettings.EnableResolve)
        {
            RTXPTGpuProfiler::Scope Timing{&m_GpuProfiler, m_pImmediateContext, RTXPTGpuProfiler::Stage::SharcResolve};
            if (!m_Sharc.Resolve(m_pImmediateContext))
            {
                RecordRealtimePathTraceStatus("SHaRC resolve failed");
                LOG_ERROR_MESSAGE("SHaRC resolve failed");
                return false;
            }
        }
    }

    // FillDenoisingLayers/Reference loop runs after LightsBaker.UpdateEnd and SHaRC resolve.
    if (!DispatchPathTraceLoop(UseDenoisingLayers, BaseDispatch))
        return false;

    if (UseDenoisingLayers)
    {
        if (!BakeDenoisingGuides())
            return false;

        // RTXDI/ReSTIR final shading and standalone NRD final merge are future port hooks.
        m_LastRealtimePathTraceExecuted = true;
        RecordRealtimePathTraceStatus("Realtime PathTrace and denoising guides dispatched; final merge pending");
    }

    return true;
}

bool RTXPTSample::DispatchPathTraceLoop(bool UseDenoisingLayers, const RTXPTRayTracingDispatch& BaseDispatch)
{
    const Uint32                SPP = std::max(m_RealtimeUI.ActualSamplesPerPixel(), 1u);
    const RTXPTPathTraceVariant Variant =
        UseDenoisingLayers ? (m_SharcActive ? RTXPTPathTraceVariant::SharcFill : RTXPTPathTraceVariant::FillDenoisingLayers) : RTXPTPathTraceVariant::Reference;

    RTXPTRayTracingDispatch PreparedDispatch = BaseDispatch;
    SampleMiniConstants     PreparedMini     = {};
    if (UseDenoisingLayers)
    {
        RTXPTGpuProfiler::Scope Timing{&m_GpuProfiler, m_pImmediateContext, RTXPTGpuProfiler::Stage::SubSampling};
        if (!m_AdaptiveSampling.PrepareSubSampling(m_pDevice, m_pImmediateContext, m_RealtimeUI, PreparedMini, PreparedDispatch))
        {
            LOG_ERROR_MESSAGE("SubSampling preparation failed");
            return false;
        }
    }
    RTXPTGpuProfiler::Scope Timing{&m_GpuProfiler, m_pImmediateContext, RTXPTGpuProfiler::Stage::Fill};
    for (Uint32 SubSampleIndex = 0; SubSampleIndex < SPP; ++SubSampleIndex)
    {
        if (UseDenoisingLayers)
        {
            RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetDenoisingLayersBuffer());
            RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetSpecularHitTUAV());
        }

        SampleMiniConstants MiniConstants = PreparedMini;
        MiniConstants.params.x            = SubSampleIndex;

        RTXPTRayTracingDispatch Dispatch = PreparedDispatch;
        Dispatch.pMiniConstants          = &MiniConstants;
        if (m_SharcActive) m_Sharc.Bind(Dispatch);

        const bool TraceOk = m_RayTracingPass.Dispatch(m_pImmediateContext, Variant, Dispatch);
        if (!TraceOk)
        {
            const char* Status = UseDenoisingLayers ? "FILL_DENOISING_LAYERS dispatch failed" : "REFERENCE dispatch failed";
            RecordRealtimePathTraceStatus(Status);
            return false;
        }

        if (UseDenoisingLayers)
            RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetSpecularHitTUAV());
    }

    if (UseDenoisingLayers)
        RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetDenoisingLayersBuffer());

    return true;
}

bool RTXPTSample::BakeDenoisingGuides()
{
    RTXPTGpuProfiler::Scope Timing{&m_GpuProfiler, m_pImmediateContext, RTXPTGpuProfiler::Stage::Guides};
    if (!m_RenderTargets.HasRealtimeRenderTargets())
    {
        RecordRealtimePathTraceStatus("Realtime render targets are not allocated for denoising guides");
        return false;
    }

    if (!m_DenoisingGuidesBaker.IsReady())
    {
        RecordRealtimePathTraceStatus("DenoisingGuidesBaker is not ready");
        return false;
    }

    const bool GuidesOk =
        m_DenoisingGuidesBaker.Bake(m_pImmediateContext,
                                    m_RenderTargets,
                                    m_RealtimeUI.DenoisingGuideDebugView);
    if (!GuidesOk)
    {
        RecordRealtimePathTraceStatus("DenoisingGuidesBaker dispatch failed");
        return false;
    }

    return true;
}

bool RTXPTSample::PresentRealtimeGuideDebug()
{
    if (m_RealtimeUI.DenoisingGuideDebugView == RTXPTDenoisingGuideDebugView::Disabled ||
        !m_DenoisingGuidesBaker.GetStats().LastDebugVizExecuted)
    {
        return false;
    }

    const bool ToneMappingExecuted =
        m_PostProcessPipeline.RunToneMapping(m_pImmediateContext,
                                             m_RenderTargets,
                                             m_ReferenceUI.ToneMapping,
                                             m_ReferenceUI.EnableToneMapping);
    if (!ToneMappingExecuted)
    {
        ClearFallback(float4{0.9f, 0.2f, 0.6f, 1.0f});
        return true;
    }

    ITextureView* pPresentationSRV = m_RenderTargets.GetPresentationSRV();
    if (!m_BlitPass.Render(m_pImmediateContext, m_pSwapChain, pPresentationSRV))
    {
        ClearFallback(float4{0.0f, 1.0f, 1.0f, 1.0f});
        return true;
    }

    return true;
}

bool RTXPTSample::RunReferencePathTraceAndPostProcess()
{
    if (!PathTrace())
    {
        if (!m_RayTracingPass.IsReady(RTXPTPathTraceVariant::Reference))
            ClearFallback(float4{1.0f, 0.0f, 1.0f, 1.0f});
        else
            ClearFallback(float4{1.0f, 1.0f, 0.0f, 1.0f});
        return false;
    }

    const float BloomRadius    = std::clamp(m_ReferenceUI.BloomRadius, 0.0f, 64.0f);
    const float BloomIntensity = std::clamp(m_ReferenceUI.BloomIntensity, 0.0f, 0.1f);

    const bool AccumulationExecuted =
        m_PostProcessPipeline.RunAccumulation(m_pImmediateContext,
                                              m_RenderTargets,
                                              m_LastFrameConstants.ptConsts.sampleIndex,
                                              m_LastFrameConstants.ptConsts.resetAccumulation != 0);
    if (!AccumulationExecuted)
    {
        ClearFallback(float4{0.0f, 0.2f, 1.0f, 1.0f});
        return false;
    }

    RTXPTBloomParameters BloomParams;
    BloomParams.Enabled   = m_ReferenceUI.EnableBloom;
    BloomParams.Radius    = BloomRadius;
    BloomParams.Intensity = BloomIntensity;

    const bool PreTonePostProcessExecuted =
        m_PostProcessPipeline.RunPreToneMapping(m_pImmediateContext,
                                                m_RenderTargets,
                                                BloomParams);
    if (!PreTonePostProcessExecuted)
    {
        ClearFallback(float4{0.9f, 0.2f, 0.6f, 1.0f});
        return false;
    }

    const bool ToneMappingExecuted =
        m_PostProcessPipeline.RunToneMapping(m_pImmediateContext,
                                             m_RenderTargets,
                                             m_ReferenceUI.ToneMapping,
                                             m_ReferenceUI.EnableToneMapping);
    if (!ToneMappingExecuted)
    {
        ClearFallback(float4{0.0f, 0.8f, 0.3f, 1.0f});
        return false;
    }

    ITextureView* pPresentationSRV = m_RenderTargets.GetPresentationSRV();
    if (!m_BlitPass.Render(m_pImmediateContext, m_pSwapChain, pPresentationSRV))
    {
        ClearFallback(float4{0.0f, 1.0f, 1.0f, 1.0f});
        return false;
    }

    return true;
}

bool RTXPTSample::RunRealtimeNoDenoiserFinalMerge()
{
    if (!m_RenderTargets.HasRealtimeRenderTargets())
    {
        RecordRealtimePathTraceStatus("Realtime render targets are missing for final merge");
        return false;
    }

    const bool MergeOk =
        m_PostProcessPipeline.RunNoDenoiserFinalMerge(m_pImmediateContext, m_RenderTargets);
    m_LastRealtimeFinalMergeReady = MergeOk;
    if (!MergeOk)
        RecordRealtimePathTraceStatus("NoDenoiserFinalMerge dispatch failed");
    else
        RecordRealtimePathTraceStatus("Realtime PathTrace, denoising guides, and final merge dispatched");
    if (MergeOk)
        RTXPTRayTracingPass::InsertUAVBarrier(m_pImmediateContext, m_RenderTargets.GetOutputColorUAV());
    return MergeOk;
}

bool RTXPTSample::PresentRealtimeFinalOutput()
{
    const bool ResetTaaSrHistory =
        HasRealtimeResetFlag(m_CurrentFrameRealtimeReset, RTXPT_REALTIME_RESET_TAA_SR_HISTORY);

    switch (m_RealtimeUI.RealtimeAA)
    {
        case RTXPTRealtimeAAMode::Disabled:
        {
            if (!m_PostProcessPipeline.CopyRealtimeOutputToProcessed(m_pImmediateContext, m_RenderTargets))
            {
                RecordRealtimePathTraceStatus("Realtime OutputColor copy to ProcessedOutputColor failed");
                return false;
            }
            break;
        }

        case RTXPTRealtimeAAMode::TAA:
        {
            if (!m_PostProcessPipeline.RunTemporalAA(m_pImmediateContext,
                                                     m_RenderTargets,
                                                     m_LastFrameConstants,
                                                     m_LastFrameConstants.ptConsts.frameIndex,
                                                     ResetTaaSrHistory,
                                                     m_HasPreviousFrameConstants,
                                                     MakeRealtimeTemporalAASettings()))
            {
                const auto& TAAStats = m_PostProcessPipeline.GetTemporalAAPass().GetStats();
                RecordRealtimePathTraceStatus(
                    TAAStats.DisabledReason.empty() ? "Realtime TAA failed" : TAAStats.DisabledReason.c_str());
                return false;
            }
            break;
        }

        case RTXPTRealtimeAAMode::SuperResolution:
        {
            if (!m_CurrentSuperResolutionFrame.Enabled)
            {
                const auto& SRStats = m_PostProcessPipeline.GetSuperResolutionPass().GetStats();
                RecordRealtimePathTraceStatus(
                    SRStats.DisabledReason.empty() ? "Realtime super resolution is unavailable" : SRStats.DisabledReason.c_str());
                return false;
            }

            if (!m_PostProcessPipeline.RunRealtimeSuperResolution(m_pImmediateContext,
                                                                  m_RenderTargets,
                                                                  m_CurrentSuperResolutionFrame,
                                                                  m_CameraNearPlane,
                                                                  m_CameraFarPlane,
                                                                  m_CameraVerticalFov))
            {
                const auto& SRStats = m_PostProcessPipeline.GetSuperResolutionPass().GetStats();
                RecordRealtimePathTraceStatus(
                    SRStats.DisabledReason.empty() ? "Realtime super-resolution pass failed" : SRStats.DisabledReason.c_str());
                return false;
            }
            break;
        }

        case RTXPTRealtimeAAMode::DLSSRR:
        {
            auto&      RR           = m_PostProcessPipeline.GetRayReconstructionPass();
            const bool ResetHistory = ResetTaaSrHistory ||
                HasRealtimeResetFlag(m_CurrentFrameRealtimeReset, RTXPT_REALTIME_RESET_REALTIME_CACHES) ||
                HasRealtimeResetFlag(m_RealtimeResetPending, RTXPT_REALTIME_RESET_TAA_SR_HISTORY);
            if (!m_RayReconstructionFrameReady ||
                !m_PostProcessPipeline.RunRayReconstruction(m_pImmediateContext, m_RenderTargets, m_LastFrameConstants, ResetHistory, &m_GpuProfiler))
            {
                RecordRealtimePathTraceStatus(RR.GetStats().Message.c_str());
                RR.InvalidateHistory();
                return false;
            }
            m_LastRealtimeFinalMergeReady = true;
            RecordRealtimePathTraceStatus("DLSS-RR executed");
            break;
        }

        default:
            RecordRealtimePathTraceStatus("Unknown realtime AA/SR mode");
            return false;
    }

    if (m_HDRCapture.Processed)
        m_HDRCapture.Capture(m_pDevice, m_pImmediateContext, m_RenderTargets.GetProcessedOutputColorSRV()->GetTexture(), m_FrameIndex);

    RTXPTBloomParameters BloomParams;
    BloomParams.Enabled   = m_ReferenceUI.EnableBloom;
    BloomParams.Radius    = std::clamp(m_ReferenceUI.BloomRadius, 0.0f, 64.0f);
    BloomParams.Intensity = std::clamp(m_ReferenceUI.BloomIntensity, 0.0f, 0.1f);

    RTXPTGpuProfiler::Scope Timing{&m_GpuProfiler, m_pImmediateContext, RTXPTGpuProfiler::Stage::PostProcess};
    if (!m_PostProcessPipeline.RunPreToneMapping(m_pImmediateContext, m_RenderTargets, BloomParams))
    {
        RecordRealtimePathTraceStatus("Realtime pre-tone post-process failed");
        return false;
    }

    if (!m_PostProcessPipeline.RunToneMapping(m_pImmediateContext,
                                              m_RenderTargets,
                                              m_ReferenceUI.ToneMapping,
                                              m_ReferenceUI.EnableToneMapping))
    {
        RecordRealtimePathTraceStatus("Realtime tone mapping failed");
        return false;
    }

    ITextureView* pPresentationSRV = m_RenderTargets.GetPresentationSRV();
    if (!m_BlitPass.Render(m_pImmediateContext, m_pSwapChain, pPresentationSRV))
    {
        RecordRealtimePathTraceStatus("Realtime presentation blit failed");
        return false;
    }

    return true;
}

void RTXPTSample::ResetNrdIntegrations()
{
    for (RTXPTNrdIntegration& Integration : m_NrdIntegrations)
        Integration.Reset();
}

bool RTXPTSample::EnsureNrdIntegrations()
{
    if (!IsStandaloneNrdAvailable())
    {
        RecordRealtimePathTraceStatus(GetStandaloneNrdDisabledReason());
        return false;
    }

    const Uint32 Width  = m_RenderTargets.GetRenderWidth();
    const Uint32 Height = m_RenderTargets.GetRenderHeight();
    for (RTXPTNrdIntegration& Integration : m_NrdIntegrations)
    {
        const bool NeedsRecreate =
            !Integration.IsReady() ||
            Integration.GetMethod() != m_RealtimeUI.NRDMethod ||
            Integration.GetWidth() != Width ||
            Integration.GetHeight() != Height;

        if (NeedsRecreate &&
            !Integration.Initialize(m_pDevice,
                                    m_pEngineFactory,
                                    m_pStateCache,
                                    m_RealtimeUI.NRDMethod,
                                    Width,
                                    Height,
                                    m_FeatureCaps.ComputeShaders))
        {
            RecordRealtimePathTraceStatus(Integration.GetLastFailureReason());
            ResetNrdIntegrations();
            return false;
        }
    }

    return true;
}

bool RTXPTSample::Denoise()
{
    if (!m_RealtimeUI.ActualUseStandaloneDenoiser())
        return true;

    if (m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR)
    {
        RecordRealtimePathTraceStatus("Standalone NRD is disabled for DLSS-RR");
        return false;
    }

    if (!EnsureNrdIntegrations())
        return false;

    const bool ResetHistory =
        HasRealtimeResetFlag(m_CurrentFrameRealtimeReset, RTXPT_REALTIME_RESET_NRD_HISTORY) ||
        HasRealtimeResetFlag(m_CurrentFrameRealtimeReset, RTXPT_REALTIME_RESET_REALTIME_CACHES);
    const bool EnableValidation = m_RenderTargets.GetDenoiserOutValidationUAV() != nullptr;

    Int32 MaxPassCount = std::min(m_RealtimeUI.DenoisingLayersActiveCount, static_cast<Int32>(kRTXPTDenoisingLayerCount));
    MaxPassCount       = std::max(MaxPassCount, Int32{1});

    bool InitWithStableRadiance = true;
    for (Int32 Pass = MaxPassCount - 1; Pass >= 0; --Pass)
    {
        const Uint32 PlaneIndex = static_cast<Uint32>(Pass);
        if (!m_PostProcessPipeline.RunDenoiserPrepare(m_pImmediateContext,
                                                      m_RenderTargets,
                                                      m_RealtimeUI.NRDMethod,
                                                      PlaneIndex,
                                                      InitWithStableRadiance))
        {
            RecordRealtimePathTraceStatus("Denoiser prepare dispatch failed");
            return false;
        }

        InitWithStableRadiance = false;
        InsertDenoiserPrepareOutputBarriers(m_pImmediateContext, m_RenderTargets);

        RTXPTNrdFrameAttribs Attribs;
        Attribs.pRenderTargets   = &m_RenderTargets;
        Attribs.pFrameConstants  = &m_LastFrameConstants;
        Attribs.pRealtime        = &m_RealtimeUI;
        Attribs.PlaneIndex       = PlaneIndex;
        Attribs.FrameIndex       = m_FrameIndex;
        Attribs.TimeDeltaSeconds = m_LastElapsedTimeSeconds > 0.0f ? m_LastElapsedTimeSeconds : -1.0f;
        Attribs.ResetHistory     = ResetHistory;
        Attribs.EnableValidation = EnableValidation;

        if (!m_NrdIntegrations[PlaneIndex].Dispatch(m_pImmediateContext, Attribs))
        {
            RecordRealtimePathTraceStatus(m_NrdIntegrations[PlaneIndex].GetLastFailureReason());
            return false;
        }

        const bool HasValidation = m_RenderTargets.GetDenoiserOutValidationSRV() != nullptr;
        if (!m_PostProcessPipeline.RunDenoiserFinalMerge(m_pImmediateContext,
                                                         m_RenderTargets,
                                                         m_RealtimeUI.NRDMethod,
                                                         PlaneIndex,
                                                         HasValidation))
        {
            RecordRealtimePathTraceStatus("Denoiser final merge dispatch failed");
            return false;
        }

        InsertDenoiserFinalMergeOutputBarrier(m_pImmediateContext, m_RenderTargets);
    }

    m_LastRealtimeFinalMergeReady = true;
    RecordRealtimePathTraceStatus("Realtime PathTrace, NRD denoise, and final merge dispatched");
    return true;
}

bool RTXPTSample::RunRealtimePostProcess()
{
    if (m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR)
        return PresentRealtimeFinalOutput();
    if (ShouldRunStandaloneNrd(m_RealtimeUI))
    {
        if (!Denoise())
            return false;
        return PresentRealtimeFinalOutput();
    }

    if (!RunRealtimeNoDenoiserFinalMerge())
        return false;

    return PresentRealtimeFinalOutput();
}

bool RTXPTSample::RunRealtimePathTraceOnly()
{
    if (!m_RenderTargets.HasRealtimeRenderTargets())
    {
        RecordRealtimePathTraceStatus(m_RenderTargets.GetLastFailureReason());
        ClearFallback(float4{0.08f, 0.08f, 0.10f, 1.0f});
        return false;
    }

    const bool PathTraceOk = PathTrace();
    if (!PathTraceOk)
    {
        m_PostProcessPipeline.GetRayReconstructionPass().InvalidateHistory();
        ClearFallback(float4{0.9f, 0.15f, 0.05f, 1.0f});
        return false;
    }

    if (PresentRealtimeGuideDebug())
    {
        m_PostProcessPipeline.GetRayReconstructionPass().InvalidateHistory();
        return true;
    }

    if (!RunRealtimePostProcess())
    {
        ClearFallback(float4{0.08f, 0.08f, 0.10f, 1.0f});
        return false;
    }

    if (!m_HDRCapture.Processed)
        m_HDRCapture.Capture(m_pDevice, m_pImmediateContext, m_RenderTargets.GetOutputColorSRV()->GetTexture(), m_FrameIndex);

    return true;
}

void RTXPTSample::Render()
{
    if (m_EnableGpuProfiling && m_FrameIndex == 120)
    {
        const auto& Pt  = m_LastFrameConstants.ptConsts;
        const auto  Pos = m_Camera.GetPos();
        std::cout << "GPU profile settings: render=" << Pt.imageWidth << 'x' << Pt.imageHeight
                  << " camera=(" << Pos.x << ',' << Pos.y << ',' << Pos.z << ") fovRadians=" << m_CameraVerticalFov
                  << " bounces=" << Pt.bounceCount << " diffuse=" << Pt.diffuseBounceCount
                  << " spp=" << m_RealtimeUI.ActualSamplesPerPixel() << " NEE=" << Pt.NEEEnabled
                  << " type=" << Pt.NEEType << " candidates=" << Pt.NEECandidateSamples << " full=" << Pt.NEEFullSamples
                  << " planes=" << Pt._activeDenoisingLayerCount << " rrPreset=" << static_cast<int>(m_RayReconstructionSettings.Preset) << std::endl;
    }
    auto Timing          = m_GpuProfiler.BeginFrame(m_pDevice, m_pImmediateContext, m_EnableGpuProfiling, m_GpuProfileCSV);
    m_LastFrameSucceeded = false;
    if (m_RealtimeUI.RealtimeMode)
    {
        m_LastRealtimePathTraceExecuted = false;
        m_LastRealtimeFinalMergeReady   = false;
        RecordRealtimePathTraceStatus("");
    }

    if (!EnsureRenderTargets())
    {
        if (m_RealtimeUI.RealtimeMode)
        {
            const char* LastFailureReason = m_RenderTargets.GetLastFailureReason();
            RecordRealtimePathTraceStatus(
                LastFailureReason[0] != '\0' ? LastFailureReason : "Realtime render target setup failed");
        }
        ClearFallback(float4{1.0f, 0.0f, 0.0f, 1.0f});
        return;
    }

    if (m_RealtimeUI.RealtimeMode)
    {
        if (m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR && !m_RayReconstructionFrameReady)
        {
            RecordRealtimePathTraceStatus(m_PostProcessPipeline.GetRayReconstructionPass().GetStats().Message.c_str());
            ClearFallback(float4{0.08f, 0.08f, 0.10f, 1.0f});
            return;
        }
        m_LastFrameSucceeded = RunRealtimePathTraceOnly();
        return;
    }

    m_LastFrameSucceeded = RunReferencePathTraceAndPostProcess();
}

void RTXPTSample::Update(double CurrTime, double ElapsedTime, bool DoUpdateUI)
{
    RunRayReconstructionValidation(CurrTime);
    RunSubSamplingValidation();
    SampleBase::Update(CurrTime, ElapsedTime, DoUpdateUI);

    m_Camera.Update(m_InputController, static_cast<float>(ElapsedTime));
    if (m_HasLastCameraMatrices &&
        (m_Camera.GetViewMatrix() != m_LastCameraView ||
         m_Camera.GetProjMatrix() != m_LastCameraProj))
    {
        RequestAccumulationReset("Camera changed", false);
    }
    m_LastCameraView        = m_Camera.GetViewMatrix();
    m_LastCameraProj        = m_Camera.GetProjMatrix();
    m_HasLastCameraMatrices = true;

    if (m_EnableSceneAnimations)
    {
        m_Scene.Update(CurrTime, ElapsedTime);
        if (m_Scene.IsGeometryDirty() && m_SkinnedGeometry.HasSkinnedGeometry() && m_SkinnedGeometry.IsReady())
        {
            const RTXPTSceneGraphData& SceneData        = m_Scene.GetSceneGraphData();
            const bool                 SkinningExecuted = m_SkinnedGeometry.Update(m_pImmediateContext, SceneData);
            const bool                 ASUpdated =
                SkinningExecuted &&
                m_AccelerationStructures.UpdateDynamicBLAS(m_pImmediateContext, SceneData, m_SkinnedGeometry);
            if (ASUpdated)
            {
                m_EmissiveTrianglesDirty = true;
                BuildEmissiveTriangles();
                // The emissive-triangle buffer was rebuilt in place; re-run the per-triangle power proxy build
                // on the next LightsBaker UpdateEnd so the sampling weights track the deformed emitters.
                m_LightsBaker.RequestProxyRebuild();
                RequestAccumulationReset("Skinned geometry updated", false);
                m_Sharc.Invalidate();
                m_Scene.ClearGeometryDirty();
            }
        }
    }

    bool RecreateSceneDependentPasses = false;
    if ((m_EnvMapBakerDirty || m_EnvMapBakerSettingsDirty) && UpdateEnvMapBaker(m_EnvMapBakerDirty))
    {
        m_LightsBakerSettingsDirty = true;
        RequestAccumulationReset("Environment map changed", true, false);
    }

    if (m_LightsBakerSettingsDirty && UpdateLightsBaker(false))
        RecreateSceneDependentPasses = true;

    // Compare the compiled counts as well as explicit UI invalidation so all
    // callers (including automated scene validation) rebuild before dispatch.
    const auto& RTStats = m_RayTracingPass.GetStats();
    if (m_RayTracingPassSettingsDirty ||
        RTStats.NEECandidateSamples != static_cast<Uint32>(std::clamp(m_ReferenceUI.NEECandidateSamples, 1, kNEEMaxSampleCount)) ||
        RTStats.NEEFullSamples != static_cast<Uint32>(std::clamp(m_ReferenceUI.NEEFullSamples, 0, kNEEMaxSampleCount)))
    {
        RecreateSceneDependentPasses  = true;
        m_RayTracingPassSettingsDirty = false;
    }

    if (RecreateSceneDependentPasses)
        CreateSceneDependentPasses();

    m_LastElapsedTimeSeconds = static_cast<float>(ElapsedTime);
    UpdateRenderTargetDimensions(m_LastElapsedTimeSeconds);
    EnsureRenderTargets();
    UpdateFrameConstants(CurrTime);
#if PLATFORM_WIN32
    if (m_ExitAfterFrames != 0 && m_FrameIndex >= m_ExitAfterFrames)
        PostQuitMessage(0);
#endif
}

void RTXPTSample::WindowResize(Uint32 Width, Uint32 Height)
{
    if (Width == 0 || Height == 0)
        return;

    UpdateCameraProjection(Width, Height);
    UpdateRenderTargetDimensions(m_LastElapsedTimeSeconds);
    m_HasLastCameraMatrices = false;
    InvalidatePreviousFrameConstants();

    const bool Ok = EnsureRenderTargets();
    ResetNrdIntegrations();
    const bool ResourcesValid = m_PostProcessPipeline.ValidateRenderTargets(m_RenderTargets);
    m_AccumulationActive =
        Ok && ResourcesValid &&
        m_RenderTargets.IsAccumulationActive() &&
        m_PostProcessPipeline.GetStats().AccumulationStageReady;
    if (Ok && ResourcesValid)
    {
        RequestAccumulationReset("Window resized");
        RequestRealtimeReset(RTXPT_REALTIME_RESET_RENDER_TARGET_RECREATE |
                                 RTXPT_REALTIME_RESET_REALTIME_CACHES |
                                 RTXPT_REALTIME_RESET_NRD_HISTORY |
                                 RTXPT_REALTIME_RESET_TAA_SR_HISTORY,
                             "Window resized");
        if (m_Scene.HasValidContent())
        {
            const bool EnvUpdated = UpdateEnvMapBaker(false);
            const bool LightsResourcesReady =
                m_LightsBaker.CreateResources(m_pDevice,
                                              m_pEngineFactory,
                                              m_pStateCache,
                                              m_CurrentTargetDimensions.DisplayWidth,
                                              m_CurrentTargetDimensions.DisplayHeight,
                                              m_FeatureCaps.ComputeShaders);
            const bool LightsUpdated = LightsResourcesReady && UpdateLightsBaker(true);
            if (EnvUpdated && LightsUpdated)
            {
                CreateSceneDependentPasses();
                m_AccumulationActive =
                    Ok && ResourcesValid &&
                    m_RenderTargets.IsAccumulationActive() &&
                    m_PostProcessPipeline.GetStats().AccumulationStageReady;
            }
            else
            {
                if (!LightsResourcesReady)
                    m_LightsBakerSettingsDirty = true;
                m_RayTracingPass.Reset();
                m_EmissiveTrianglePass.Reset();
            }
        }
    }
}

void RTXPTSample::UpdateUI()
{
    // RESET_ON_CHANGE equivalent (cf. D:/RTXPT-fork/Rtxpt/SampleUI.cpp:49): when a control
    // reports a change, restart progressive accumulation. Returns the change flag so callers
    // can also write the edited value back.
    auto ResetOnChange = [this](bool Changed, const char* Reason) -> bool {
        if (Changed)
            RequestAccumulationReset(Reason);
        return Changed;
    };
    auto ResetLightsBakerOnChange = [this, &ResetOnChange](bool Changed, const char* Reason) -> bool {
        if (ResetOnChange(Changed, Reason))
        {
            m_LightsBakerSettingsDirty = true;
            return true;
        }
        return false;
    };
    auto ResetRealtimeOnChange = [this](bool Changed, const char* Reason) -> bool {
        if (Changed)
            RequestRealtimeCachesReset(Reason);
        return Changed;
    };
    auto ResetTaaSrOnChange = [this](bool Changed, const char* Reason) -> bool {
        if (Changed)
            RequestRealtimeReset(RTXPT_REALTIME_RESET_TAA_SR_HISTORY, Reason);
        return Changed;
    };
    auto ResetRenderTargetsOnChange = [this](bool Changed, const char* Reason) -> bool {
        if (Changed)
            RequestRealtimeReset(RTXPT_REALTIME_RESET_RENDER_TARGET_RECREATE |
                                     RTXPT_REALTIME_RESET_REALTIME_CACHES |
                                     RTXPT_REALTIME_RESET_NRD_HISTORY |
                                     RTXPT_REALTIME_RESET_TAA_SR_HISTORY,
                                 Reason);
        return Changed;
    };
    // Tooltip for a present-but-disabled placeholder control. AllowWhenDisabled lets the
    // tooltip appear even though the preceding item was inside BeginDisabled()/EndDisabled().
    auto PlaceholderTooltip = [](const char* Text) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", Text);
    };

    const ImVec4 CategoryColor{0.60f, 0.85f, 1.00f, 1.00f};
    const float  Indent = 16.0f;

    ImGui::Begin("RTXPT");
    if (ImGui::CollapsingHeader("SHaRC radiance cache"))
    {
        const bool Supported = SharcIntegration::IsSupported(m_pDevice);
        if (!Supported) ImGui::TextWrapped("Unavailable: requires D3D12 SM 6.6 and native FP16. Vulkan backend does not enable 64-bit atomics.");
        if (ImGui::Checkbox("Enable SHaRC (realtime only)", &m_SharcSettings.Enabled))
        {
            m_RayTracingPassSettingsDirty = true;
            RequestAccumulationReset("SHaRC mode changed");
        }
        bool CacheChanged = ImGui::SliderInt("Cache capacity log2", &m_SharcSettings.CapacityLog2, 16, 24);
        CacheChanged |= ImGui::SliderInt("Training downscale", &m_SharcSettings.DownscaleFactor, 2, 5);
        CacheChanged |= ImGui::SliderFloat("Grid scene scale", &m_SharcSettings.SceneScale, 1.f, 100.f);
        CacheChanged |= ImGui::SliderFloat("Training roughness floor", &m_SharcSettings.MinRoughnessOverride, 0.f, 1.f);
        bool HistoryChanged = ImGui::SliderInt("Cache accumulation frames", &m_SharcSettings.AccumulationFrameNum, 1, 128);
        HistoryChanged |= ImGui::SliderInt("Cache stale frames", &m_SharcSettings.StaleFrameNum, 8, 256);
        HistoryChanged |= ImGui::SliderFloat("Termination threshold", &m_SharcSettings.TerminateThreshold, 0.f, 10.f);
        HistoryChanged |= ImGui::Checkbox("Update cache", &m_SharcSettings.EnableUpdate);
        HistoryChanged |= ImGui::Checkbox("Resolve cache", &m_SharcSettings.EnableResolve);
        HistoryChanged |= ImGui::Combo("Cache debug", &m_SharcSettings.DebugMode, "Off\0Radiance\0Hash\0Occupancy\0Collisions\0Voxel size\0Grid level\0");
        const bool ClearCache = ImGui::Button("Clear radiance cache");
        if (CacheChanged || ClearCache)
            RequestAccumulationReset("SHaRC cache settings changed");
        else if (HistoryChanged)
            RequestRealtimeReset(RTXPT_REALTIME_RESET_NRD_HISTORY | RTXPT_REALTIME_RESET_TAA_SR_HISTORY, "SHaRC query/update settings changed");
    }

    // ------------------------------------------------------------------ Path Tracer
    if (ImGui::CollapsingHeader("Path Tracer", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent(Indent);

        {
            int ModeIndex = m_RealtimeUI.RealtimeMode ? 1 : 0;
            if (ImGui::Combo("Mode", &ModeIndex, "Reference\0Realtime\0\0"))
            {
                const bool NewRealtimeMode = ModeIndex != 0;
                if (m_RealtimeUI.RealtimeMode != NewRealtimeMode)
                {
                    m_RealtimeUI.RealtimeMode = NewRealtimeMode;
                    RequestRealtimeReset(RTXPT_REALTIME_RESET_ACCUMULATION |
                                             RTXPT_REALTIME_RESET_RENDER_TARGET_RECREATE |
                                             RTXPT_REALTIME_RESET_REALTIME_CACHES |
                                             RTXPT_REALTIME_RESET_NRD_HISTORY |
                                             RTXPT_REALTIME_RESET_TAA_SR_HISTORY,
                                         "Path-tracer mode changed");
                }
            }
            if (m_RealtimeUI.RealtimeMode && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", kRTXPTRealtimeRoutePendingReason);
        }

        ImGui::TextColored(CategoryColor, "Setup:");
        ImGui::Indent(Indent);
        {
            if (m_RealtimeUI.RealtimeMode)
            {
                if (ImGui::Button("Reset##RTMACC"))
                    RequestRealtimeCachesReset("User reset realtime caches");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Reset realtime temporal caches, NRD history, TAA/SR history, and lighting feedback.");
                ImGui::SameLine();

                int RealtimeSPP = m_RealtimeUI.RealtimeSamplesPerPixel;
                if (ResetRealtimeOnChange(ImGui::InputInt("Samples per pixel", &RealtimeSPP), "Realtime samples-per-pixel changed"))
                    m_RealtimeUI.RealtimeSamplesPerPixel = RealtimeSPP;
                m_RealtimeUI.RealtimeSamplesPerPixel =
                    std::clamp(m_RealtimeUI.RealtimeSamplesPerPixel, Int32{1}, static_cast<Int32>(kRTXPTRealtimeSamplesPerPixelMax));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Full paths per pixel per realtime frame; camera ray anti-aliasing is handled by the selected AA/SR mode.");
                ImGui::BeginDisabled(m_RealtimeUI.ActualSamplesPerPixel() != 1);
                ResetRealtimeOnChange(ImGui::Combo("Sub-sampling rate", &m_RealtimeUI.RealtimeSubSamplingMode, "1 spp\0Three quarters\0Half\0Quarter\0"), "Sub-sampling rate changed");
                ResetRealtimeOnChange(ImGui::Combo("Sub-sampling pattern", &m_RealtimeUI.RealtimeSubSamplingPattern, "Random per block\0Stochastic compacted\0Block stratified\0Sobol block\0"), "Sub-sampling pattern changed");
                int BlockIndex = m_RealtimeUI.RealtimeSubSamplingBlockSize <= 4 ? 0 : (m_RealtimeUI.RealtimeSubSamplingBlockSize <= 8 ? 1 : 2);
                if (ResetRealtimeOnChange(ImGui::Combo("Sub-sampling block size", &BlockIndex, "4\0 8\0 16\0"), "Sub-sampling block size changed"))
                    m_RealtimeUI.RealtimeSubSamplingBlockSize = 4 << BlockIndex;
                ImGui::EndDisabled();
            }
            else
            {
                if (ImGui::Button("Reset##REFMACC"))
                    RequestAccumulationReset("User reset");
                ImGui::SameLine();
                ImGui::Text("Accumulated samples: %u", m_AccumulationFrame);
            }

            int MaxBouncesUI = static_cast<int>(m_MaxBounces);
            if (ResetOnChange(ImGui::SliderInt("Max bounces", &MaxBouncesUI, 1, kMaxBounceSliderValue), "Max bounces changed"))
                m_MaxBounces = static_cast<Uint32>(MaxBouncesUI);

            int DiffuseBouncesUI = m_ReferenceUI.DiffuseBounceCount;
            if (ResetOnChange(ImGui::SliderInt("Max diffuse bounces", &DiffuseBouncesUI, 0, 16), "Max diffuse bounces changed"))
                m_ReferenceUI.DiffuseBounceCount = std::clamp(DiffuseBouncesUI, 0, 16);

            int MinBouncesUI = static_cast<int>(m_MinBounces);
            if (ResetOnChange(ImGui::SliderInt("Min bounces (RR start)", &MinBouncesUI, 0, 16), "Min bounces changed"))
                m_MinBounces = static_cast<Uint32>(MinBouncesUI);

            // Russian roulette: always on; start bounce is "Min bounces (RR start)".
            ImGui::BeginDisabled(true);
            ImGui::Checkbox("Use Russian Roulette early out", &m_ReferenceUI.EnableRussianRoulette);
            ImGui::EndDisabled();
            PlaceholderTooltip("Russian roulette is always enabled; its start bounce is 'Min bounces (RR start)'.");

            if (m_RealtimeUI.RealtimeMode)
            {
                ResetRealtimeOnChange(ImGui::Checkbox("FireflyFilter (realtime)", &m_RealtimeUI.RealtimeFireflyFilterEnabled),
                                      "Realtime firefly filter toggled");
                if (m_RealtimeUI.RealtimeFireflyFilterEnabled)
                {
                    ImGui::Indent(Indent);
                    ResetRealtimeOnChange(ImGui::InputFloat("FF Threshold", &m_RealtimeUI.RealtimeFireflyFilterThreshold, 0.01f, 0.1f, "%.5f"),
                                          "Realtime firefly threshold changed");
                    m_RealtimeUI.RealtimeFireflyFilterThreshold =
                        std::clamp(m_RealtimeUI.RealtimeFireflyFilterThreshold, 0.00001f, 1000.0f);
                    ImGui::Unindent(Indent);
                }
            }
            else
            {
                // Adaptive firefly filter (G1, live). Disabling it uploads a zero threshold so the soft
                // cap is a no-op and the converged image is identical to the filter-on image.
                ResetOnChange(ImGui::Checkbox("FireflyFilter (reference *)", &m_ReferenceUI.ReferenceFireflyFilterEnabled),
                              "Firefly filter toggled");
                if (m_ReferenceUI.ReferenceFireflyFilterEnabled)
                {
                    ImGui::Indent(Indent);
                    ResetOnChange(ImGui::InputFloat("FF Threshold", &m_ReferenceUI.ReferenceFireflyFilterThreshold, 0.1f, 0.2f, "%.5f"),
                                  "Firefly threshold changed");
                    ImGui::Unindent(Indent);
                }
            }

            if (m_RealtimeUI.RealtimeMode)
            {
                ResetRealtimeOnChange(ImGui::InputFloat("Texture MIP bias", &m_RealtimeUI.TexLODBias),
                                      "Realtime texture MIP bias changed");
            }
        }
        ImGui::Unindent(Indent); // end Setup

        ImGui::TextColored(CategoryColor, "Post processing:");
        ImGui::Indent(Indent);
        {
            if (m_RealtimeUI.RealtimeMode)
            {
                const char* AAItems[] = {"Disabled", "TAA", "Super Resolution", "DLSS-RR"};
                int         AAMode    = static_cast<int>(m_RealtimeUI.RealtimeAA);
                if (ImGui::BeginCombo("AA/SR/Denoising", AAItems[std::clamp(AAMode, 0, 3)]))
                {
                    const bool TaaAvailable = m_PostProcessPipeline.GetTemporalAAPass().IsReady();
                    const bool SrAvailable  = m_PostProcessPipeline.GetSuperResolutionPass().HasTemporalVariant();
                    for (int Item = 0; Item < 4; ++Item)
                    {
                        const bool Enabled =
                            Item == static_cast<int>(RTXPTRealtimeAAMode::Disabled) ||
                            (Item == static_cast<int>(RTXPTRealtimeAAMode::TAA) && TaaAvailable) ||
                            (Item == static_cast<int>(RTXPTRealtimeAAMode::SuperResolution) && SrAvailable) ||
                            (Item == static_cast<int>(RTXPTRealtimeAAMode::DLSSRR) && m_PostProcessPipeline.GetRayReconstructionPass().GetStats().Available);

                        ImGui::BeginDisabled(!Enabled);
                        const bool Selected = AAMode == Item;
                        if (ImGui::Selectable(AAItems[Item], Selected))
                        {
                            AAMode                  = Item;
                            m_RealtimeUI.RealtimeAA = static_cast<RTXPTRealtimeAAMode>(AAMode);
                            ResetTaaSrOnChange(true, "Realtime AA/SR mode changed");
                        }
                        if (Selected)
                            ImGui::SetItemDefaultFocus();
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !Enabled)
                        {
                            if (Item == static_cast<int>(RTXPTRealtimeAAMode::TAA))
                            {
                                const auto& TAAStats = m_PostProcessPipeline.GetTemporalAAPass().GetStats();
                                ImGui::SetTooltip("%s",
                                                  TAAStats.DisabledReason.empty() ?
                                                      "DiligentFX TemporalAntiAliasing is not initialized." :
                                                      TAAStats.DisabledReason.c_str());
                            }
                            else if (Item == static_cast<int>(RTXPTRealtimeAAMode::SuperResolution))
                            {
                                const auto& SRStats          = m_PostProcessPipeline.GetSuperResolutionPass().GetStats();
                                const char* SRDisabledReason = nullptr;
                                if (!SRStats.FactoryReady)
                                {
                                    SRDisabledReason = "Diligent super-resolution provider is unavailable.";
                                }
                                else if (SRStats.VariantCount == 0)
                                {
                                    SRDisabledReason = "No Diligent super-resolution variants are available.";
                                }
                                else
                                {
                                    SRDisabledReason = "No temporal Diligent super-resolution variant is available.";
                                }
                                ImGui::SetTooltip("%s", SRDisabledReason);
                            }
                            else if (Item == static_cast<int>(RTXPTRealtimeAAMode::DLSSRR))
                                ImGui::SetTooltip("%s", m_PostProcessPipeline.GetRayReconstructionPass().GetStats().Message.c_str());
                        }
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("DLSS-RR reconstructs noisy radiance and replaces standalone denoising and temporal upscaling.");

                if (m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR)
                {
                    int         Quality     = static_cast<int>(m_RayReconstructionSettings.Quality);
                    const char* Qualities[] = {"Native", "Ultra Quality", "Quality", "Balanced", "Performance", "Ultra Performance"};
                    if (ImGui::Combo("RR Quality", &Quality, Qualities, _countof(Qualities)))
                    {
                        m_RayReconstructionSettings.Quality = static_cast<RAY_RECONSTRUCTION_QUALITY>(Quality);
                        ResetTaaSrOnChange(true, "RR quality changed");
                    }
                    int Preset = m_RayReconstructionSettings.Preset == RAY_RECONSTRUCTION_DLSS_PRESET_E ? 1 : 0;
                    if (ImGui::Combo("RR Preset", &Preset, "F (default)\0E\0"))
                    {
                        m_RayReconstructionSettings.Preset = Preset == 1 ? RAY_RECONSTRUCTION_DLSS_PRESET_E : RAY_RECONSTRUCTION_DLSS_PRESET_F;
                        ResetTaaSrOnChange(true, "RR preset changed");
                    }
                    if (m_pDevice->GetDeviceInfo().IsVulkanDevice())
                        ImGui::TextWrapped("Vulkan preset F has a known SDK validation failure. Preset E executes, but RTXPT Vulkan image quality is not yet validated.");
                    ResetRealtimeOnChange(ImGui::SliderFloat("RR Micro Jitter", &m_RealtimeUI.DLSSRRMicroJitter, 0.0f, 1.0f), "RR micro jitter changed");
                    ResetRealtimeOnChange(ImGui::InputFloat("RR Brightness Clamp", &m_RealtimeUI.DLSSRRBrightnessClampK), "RR brightness clamp changed");
                }
                const auto& RRStats = m_PostProcessPipeline.GetRayReconstructionPass().GetStats();
                ImGui::TextWrapped("%s", RRStats.Message.c_str());
                // Only for debugging: manual DLSS-RR re-detection.
                // if (ImGui::Button("Retry DLSS-RR detection"))
                // {
                //     m_PostProcessPipeline.GetRayReconstructionPass().Initialize(m_pDevice);
                //     ResetTaaSrOnChange(true, "RR retry");
                // }

                if (m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::SuperResolution &&
                    m_PostProcessPipeline.GetSuperResolutionPass().HasTemporalVariant())
                {
                    const RTXPTSuperResolutionPass& SuperResolutionPass = m_PostProcessPipeline.GetSuperResolutionPass();
                    const auto&                     Variants            = SuperResolutionPass.GetVariants();
                    const Int32                     ActiveVariantIdx    = ResolveActiveSuperResolutionVariantIdx(m_RealtimeUI, SuperResolutionPass);

                    ImGui::Indent(Indent);

                    // Provider (variant) selection. Only temporal providers can drive the realtime path.
                    const char* ProviderPreview =
                        (ActiveVariantIdx >= 0 && ActiveVariantIdx < static_cast<Int32>(Variants.size())) ?
                        Variants[static_cast<size_t>(ActiveVariantIdx)].Name :
                        "none";
                    if (ImGui::BeginCombo("SR Provider", ProviderPreview))
                    {
                        for (size_t VariantIdx = 0; VariantIdx < Variants.size(); ++VariantIdx)
                        {
                            const bool IsTemporal = Variants[VariantIdx].Type == SUPER_RESOLUTION_TYPE_TEMPORAL;
                            ImGui::BeginDisabled(!IsTemporal);
                            const bool Selected = static_cast<Int32>(VariantIdx) == ActiveVariantIdx;
                            if (ImGui::Selectable(Variants[VariantIdx].Name, Selected))
                            {
                                m_RealtimeUI.SuperResolutionVariantIdx = static_cast<Int32>(VariantIdx);
                                ResetTaaSrOnChange(true, "Super resolution provider changed");
                            }
                            if (Selected)
                                ImGui::SetItemDefaultFocus();
                            ImGui::EndDisabled();
                            if (!IsTemporal && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                                ImGui::SetTooltip("Spatial-only variant; the realtime path requires a temporal upscaler.");
                        }
                        ImGui::EndCombo();
                    }

                    // Quality / performance trade-off (provider-agnostic optimization type).
                    const char* SRQualityItems[] = {
                        "Max Quality",
                        "High Quality",
                        "Balanced",
                        "High Performance",
                        "Max Performance"};
                    int SRQuality = static_cast<int>(m_RealtimeUI.SuperResolutionQuality);
                    if (ImGui::Combo("SR Quality", &SRQuality, SRQualityItems, _countof(SRQualityItems)))
                    {
                        m_RealtimeUI.SuperResolutionQuality = static_cast<RTXPTSuperResolutionQuality>(
                            std::clamp(SRQuality, 0, static_cast<int>(_countof(SRQualityItems) - 1)));
                        ResetTaaSrOnChange(true, "Super resolution quality changed");
                    }

                    // Sharpness is only exposed when the active provider advertises sharpening support.
                    if (ActiveVariantIdx >= 0 && ActiveVariantIdx < static_cast<Int32>(Variants.size()) &&
                        SuperResolutionPass.SupportsSharpness(Variants[static_cast<size_t>(ActiveVariantIdx)]))
                    {
                        ResetTaaSrOnChange(ImGui::SliderFloat("SR Sharpness", &m_RealtimeUI.SuperResolutionSharpness, 0.0f, 1.0f),
                                           "Super resolution sharpness changed");
                    }

                    ImGui::Unindent(Indent);
                }

                const bool DenoiserDisabled =
                    m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR ||
                    !IsStandaloneNrdAvailable();
                ImGui::BeginDisabled(DenoiserDisabled);
                if (ResetRealtimeOnChange(ImGui::Checkbox("Use standalone denoiser (NRD)", &m_RealtimeUI.StandaloneDenoiser),
                                          "Standalone denoiser toggled"))
                {
                    RequestRealtimeReset(RTXPT_REALTIME_RESET_NRD_HISTORY, "Standalone denoiser toggled");
                }
                ImGui::EndDisabled();
                if (DenoiserDisabled)
                    DrawDisabledTooltip(m_RealtimeUI.RealtimeAA == RTXPTRealtimeAAMode::DLSSRR ?
                                            "Standalone NRD is disabled for DLSS-RR." :
                                            GetStandaloneNrdDisabledReason());

                const char* GuideDebugItems[] = {
                    "Disabled",
                    "Depth",
                    "Motion Vectors",
                    "Specular Hit T",
                    "Average Layer Radiance",
                    "Primary Layer"};
                int GuideDebugView = static_cast<int>(m_RealtimeUI.DenoisingGuideDebugView);
                if (ImGui::Combo("Denoising guide debug", &GuideDebugView, GuideDebugItems, _countof(GuideDebugItems)))
                {
                    m_RealtimeUI.DenoisingGuideDebugView =
                        static_cast<RTXPTDenoisingGuideDebugView>(std::clamp(GuideDebugView, 0, static_cast<int>(_countof(GuideDebugItems) - 1)));
                    RequestRealtimeReset(RTXPT_REALTIME_RESET_TAA_SR_HISTORY, "Denoising guide debug view changed");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Displays guide debug output from the G6 denoising-guides pass when realtime mode is active.");
            }

            if (ImGui::CollapsingHeader("Bloom"))
            {
                ImGui::Checkbox("Enable Bloom", &m_ReferenceUI.EnableBloom);
                ImGui::SliderFloat("Bloom Width (Pixels)", &m_ReferenceUI.BloomRadius, 0.0f, 64.0f);
                ImGui::SliderFloat("Bloom Intensity", &m_ReferenceUI.BloomIntensity, 0.0f, 0.1f);
            }

            ImGui::Checkbox("Enable tone mapping", &m_ReferenceUI.EnableToneMapping);

            RTXPTToneMappingParameters& ToneMapping = m_ReferenceUI.ToneMapping;

            const char* ToneMapOperatorItems = "Linear\0Reinhard\0Reinhard Modified\0Heji Hable ALU\0Hable UC2\0Aces\0\0";
            int         ToneMapOperator      = std::clamp(static_cast<int>(ToneMapping.ToneMapOperator), kToneMapOperatorMin, kToneMapOperatorMax);
            if (ImGui::Combo("Operator", &ToneMapOperator, ToneMapOperatorItems))
                ToneMapping.ToneMapOperator = static_cast<RTXPTToneMapperOperator>(std::clamp(ToneMapOperator, kToneMapOperatorMin, kToneMapOperatorMax));
            else
                ToneMapping.ToneMapOperator = static_cast<RTXPTToneMapperOperator>(ToneMapOperator);

            ImGui::Checkbox("Auto Exposure", &ToneMapping.AutoExposure);

            if (ToneMapping.AutoExposure)
            {
                ImGui::InputFloat("Auto Exposure Min", &ToneMapping.ExposureValueMin);
                ImGui::InputFloat("Auto Exposure Max", &ToneMapping.ExposureValueMax);
            }

            const char* ExposureModeItems = "Aperture Priority\0Shutter Priority\0\0";
            int         ExposureMode      = std::clamp(static_cast<int>(ToneMapping.ExposureMode), kExposureModeMin, kExposureModeMax);
            if (ImGui::Combo("Exposure Mode", &ExposureMode, ExposureModeItems))
                ToneMapping.ExposureMode = static_cast<RTXPTExposureMode>(std::clamp(ExposureMode, kExposureModeMin, kExposureModeMax));
            else
                ToneMapping.ExposureMode = static_cast<RTXPTExposureMode>(ExposureMode);

            ImGui::InputFloat("Exposure Compensation", &ToneMapping.ExposureCompensation);

            ImGui::InputFloat("Exposure Value", &ToneMapping.ExposureValue);

            ImGui::InputFloat("Film Speed", &ToneMapping.FilmSpeed);

            ImGui::InputFloat("fNumber", &ToneMapping.FNumber);

            ImGui::InputFloat("Shutter", &ToneMapping.Shutter);

            ImGui::Checkbox("Enable White Balance", &ToneMapping.WhiteBalance);

            ImGui::InputFloat("White Point", &ToneMapping.WhitePoint);

            ImGui::InputFloat("White Max Luminance", &ToneMapping.WhiteMaxLuminance);

            ImGui::InputFloat("White Scale", &ToneMapping.WhiteScale);

            ImGui::Checkbox("Enable Clamp", &ToneMapping.Clamped);
            SanitizeToneMappingParameters(ToneMapping);
        }
        ImGui::Unindent(Indent); // end Post processing

        ImGui::TextColored(CategoryColor, "Light sampling:");
        ImGui::Indent(Indent);
        {
            ResetOnChange(ImGui::Checkbox("Use Next Event Estimation", &m_EnableNEE), "NEE toggled");
            ResetOnChange(ImGui::Checkbox("Emissive mesh NEE + MIS", &m_EnableEmissiveNEE), "Emissive NEE toggled");
            ResetOnChange(ImGui::Checkbox("Environment NEE + MIS", &m_EnableEnvNEE), "Environment NEE toggled");

            int MaxNEEBouncesUI = static_cast<int>(m_MaxNEEBounces);
            if (ResetOnChange(ImGui::SliderInt("NEE bounces", &MaxNEEBouncesUI, 0, 16), "NEE bounce budget changed"))
                m_MaxNEEBounces = static_cast<Uint32>(MaxNEEBouncesUI);

            ResetOnChange(ImGui::SliderFloat("Light intensity scale", &m_LightIntensityScale, 0.0f, 10.0f), "Light intensity changed");

            if (m_EnableNEE)
            {
                ImGui::TextColored(CategoryColor, "NEE settings:");
                ImGui::Indent(Indent);
                {
                    // Light importance sampling + MIS type: G5/R3.
                    const char* SamplingTechniqueItems = "Uniform\0Power+\0NEE-AT\0\0";
                    if (ResetLightsBakerOnChange(ImGui::Combo("Sampling technique", &m_ReferenceUI.NEEType, SamplingTechniqueItems),
                                                 "NEE sampling technique changed"))
                        m_ReferenceUI.NEEType = std::clamp(m_ReferenceUI.NEEType, 0, 2);

                    if (ResetOnChange(ImGui::InputInt("Candidate samples", &m_ReferenceUI.NEECandidateSamples, 1),
                                      "NEE candidate count changed"))
                        m_ReferenceUI.NEECandidateSamples = std::clamp(m_ReferenceUI.NEECandidateSamples, 1, kNEEMaxSampleCount);

                    if (ResetOnChange(ImGui::InputInt("Full samples", &m_ReferenceUI.NEEFullSamples, 1),
                                      "NEE full sample count changed"))
                        m_ReferenceUI.NEEFullSamples = std::clamp(m_ReferenceUI.NEEFullSamples, 0, kNEEMaxSampleCount);

                    if (ResetOnChange(ImGui::Combo("MIS Type", &m_ReferenceUI.NEEMISType, "Full\0ApproxInRealtime\0Approximate\0\0"),
                                      "NEE MIS mode changed"))
                        m_RayTracingPassSettingsDirty = true;

                    ImGui::TextColored(CategoryColor, "NEE-AT settings:");
                    if (m_ReferenceUI.NEEType != 2)
                    {
                        ImGui::TextWrapped("NOTE: NEE-AT inactive (enable in Path Tracer -> Next Event Estimation settings).");
                    }
                    else
                    {
                        ResetLightsBakerOnChange(ImGui::SliderFloat("Global feedback weight", &m_ReferenceUI.NEEAT_GlobalTemporalFeedbackWeight, 0.0f, 0.95f),
                                                 "NEE-AT global feedback changed");
                        ResetLightsBakerOnChange(ImGui::SliderFloat("Local to global sampler ratio", &m_ReferenceUI.NEEAT_LocalToGlobalSampleRatio, 0.0f, 0.95f),
                                                 "NEE-AT local/global ratio changed");
                        ResetLightsBakerOnChange(ImGui::SliderFloat("Distant vs Local initial importance", &m_ReferenceUI.NEEAT_DistantVsLocalImportance, 0.01f, 100.0f, "%.2f"),
                                                 "NEE-AT distant/local importance changed");
                    }
                }
                ImGui::Unindent(Indent);
            }
        }
        ImGui::Unindent(Indent); // end Light sampling

        ImGui::Unindent(Indent); // end Path Tracer
    }

    // ------------------------------------------------------------ PT: Advanced Settings
    if (ImGui::CollapsingHeader("PT: Advanced Settings"))
    {
        ImGui::Indent(Indent);

        if (ResetOnChange(ImGui::Combo("Nested Dielectrics", &m_ReferenceUI.NestedDielectricsQuality, "Off\0Fast\0Quality\0\0"),
                          "Nested dielectric quality changed"))
        {
            m_ReferenceUI.NestedDielectricsQuality = std::clamp(m_ReferenceUI.NestedDielectricsQuality, 0, 2);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Priority-based nested dielectrics. Fast allows fewer false-hit rejections; Quality allows more.");

        if (ResetOnChange(ImGui::Checkbox("Enable LD sampler for BSDF", &m_ReferenceUI.EnableLDSamplerForBSDF),
                          "BSDF LD sampler toggled"))
            m_RayTracingPassSettingsDirty = true;

        ImGui::Unindent(Indent);
    }

    // ------------------------------------------------ Stable Planes (denoising layers)
    if (ImGui::CollapsingHeader("Stable Planes (denoising layers)"))
    {
        ImGui::Indent(Indent);

        if (m_RealtimeUI.RealtimeMode)
        {
            int ActiveDenoisingLayers = m_RealtimeUI.DenoisingLayersActiveCount;
            if (ResetRealtimeOnChange(ImGui::InputInt("Active stable planes", &ActiveDenoisingLayers),
                                      "Active stable-plane count changed"))
                m_RealtimeUI.DenoisingLayersActiveCount = ActiveDenoisingLayers;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How many stable planes to allow; one plane is standard denoising.");

            int MaxDenoisingLayerVertexDepth = m_RealtimeUI.DenoisingLayersMaxVertexDepth;
            if (ResetRealtimeOnChange(ImGui::InputInt("Max stable plane vertex depth", &MaxDenoisingLayerVertexDepth),
                                      "Stable-plane max vertex depth changed"))
                m_RealtimeUI.DenoisingLayersMaxVertexDepth = MaxDenoisingLayerVertexDepth;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How deep the stable part of path tracing can go.");

            ResetRealtimeOnChange(ImGui::SliderFloat("Path split stop threshold", &m_RealtimeUI.DenoisingLayersSplitStopThreshold, 0.0f, 2.0f),
                                  "Stable-plane split threshold changed");
            ResetRealtimeOnChange(ImGui::Checkbox("Primary Surface Replacement", &m_RealtimeUI.AllowPrimarySurfaceReplacement),
                                  "Primary surface replacement toggled");
            ResetRealtimeOnChange(ImGui::Checkbox("Suppress primary plane noisy specular", &m_RealtimeUI.DenoisingLayersSuppressPrimaryIndirectSpecular),
                                  "Primary-plane noisy specular suppression toggled");
            ResetRealtimeOnChange(ImGui::SliderFloat("Suppress primary plane noisy specular amount",
                                                     &m_RealtimeUI.DenoisingLayersSuppressPrimaryIndirectSpecularK, 0.0f, 1.0f),
                                  "Primary-plane noisy specular suppression amount changed");
            ResetRealtimeOnChange(ImGui::SliderFloat("Non-primary plane anti-aliasing fallthrough",
                                                     &m_RealtimeUI.DenoisingLayersAntiAliasingFallthrough, 0.0f, 1.0f),
                                  "Stable-plane anti-aliasing fallthrough changed");
        }
        else
        {
            ImGui::Text("Not available in reference mode");
        }

        ImGui::Unindent(Indent);
    }

    // ------------------------------------------------------- Standalone Denoiser (NRD)
    if (ImGui::CollapsingHeader("Standalone Denoiser (NRD)"))
    {
        ImGui::Indent(Indent);

        if (!m_RealtimeUI.RealtimeMode)
            ImGui::TextWrapped("Not available in reference mode.");
        if (m_RealtimeUI.RealtimeMode && !IsStandaloneNrdAvailable())
            ImGui::TextWrapped("%s", GetStandaloneNrdDisabledReason());

        const bool DisableNrdControls =
            !m_RealtimeUI.RealtimeMode ||
            !m_RealtimeUI.ActualUseStandaloneDenoiser() ||
            !IsStandaloneNrdAvailable();

        ImGui::BeginDisabled(DisableNrdControls);

        auto SliderUint = [&](const char* Label, Uint32& Value, int MinValue, int MaxValue, const char* Reason) {
            int ValueUI = static_cast<int>(Value);
            if (ResetRealtimeOnChange(ImGui::SliderInt(Label, &ValueUI, MinValue, MaxValue), Reason))
            {
                Value = static_cast<Uint32>(std::clamp(ValueUI, MinValue, MaxValue));
                return true;
            }
            return false;
        };

        ResetRealtimeOnChange(ImGui::InputFloat("Disocclusion Threshold", &m_RealtimeUI.NRDDisocclusionThreshold),
                              "NRD disocclusion threshold changed");
        ResetRealtimeOnChange(ImGui::Checkbox("Use Alternate Disocclusion Threshold Mix",
                                              &m_RealtimeUI.NRDUseAlternateDisocclusionThresholdMix),
                              "NRD alternate disocclusion mix toggled");
        ResetRealtimeOnChange(ImGui::InputFloat("Disocclusion Threshold Alt", &m_RealtimeUI.NRDDisocclusionThresholdAlternate),
                              "NRD alternate disocclusion threshold changed");
        ResetRealtimeOnChange(ImGui::InputFloat("Radiance clamping", &m_RealtimeUI.DenoiserRadianceClampK),
                              "NRD radiance clamp changed");

        ImGui::Separator();

        int NrdMethod = static_cast<int>(m_RealtimeUI.NRDMethod);
        if (ImGui::Combo("Denoiser Mode", &NrdMethod, "REBLUR\0RELAX\0\0"))
        {
            m_RealtimeUI.NRDMethod = static_cast<RTXPTNrdMethod>(std::clamp(NrdMethod, 0, 1));
            RequestRealtimeReset(RTXPT_REALTIME_RESET_NRD_HISTORY, "NRD mode changed");
            ResetNrdIntegrations();
        }

        if (ImGui::CollapsingHeader("Advanced Settings"))
        {
            if (m_RealtimeUI.NRDMethod == RTXPTNrdMethod::REBLUR)
            {
                RTXPTNrdReblurUiSettings& Reblur = m_RealtimeUI.ReblurSettings;
                SliderUint("Max Accumulated Frames", Reblur.MaxAccumulatedFrameNum, 0, 500,
                           "REBLUR max accumulated frames changed");
                SliderUint("Fast Max Accumulated Frames", Reblur.MaxFastAccumulatedFrameNum, 0, 500,
                           "REBLUR fast max accumulated frames changed");
                SliderUint("History Fix Frames", Reblur.HistoryFixFrameNum, 0, 500,
                           "REBLUR history fix frames changed");
                ResetRealtimeOnChange(ImGui::SliderFloat("Diffuse Prepass Blur Radius (pixels)", &Reblur.DiffusePrepassBlurRadius, 0.0f, 100.0f),
                                      "REBLUR diffuse prepass blur radius changed");
                ResetRealtimeOnChange(ImGui::SliderFloat("Specular Prepass Blur Radius (pixels)", &Reblur.SpecularPrepassBlurRadius, 0.0f, 100.0f),
                                      "REBLUR specular prepass blur radius changed");
                int HitDistanceMode = static_cast<int>(Reblur.HitDistanceReconstructionMode);
                if (ImGui::Combo("Hit Distance Reconstruction Mode", &HitDistanceMode, "Off\0AREA_3X3\0AREA_5X5\0\0"))
                {
                    Reblur.HitDistanceReconstructionMode =
                        static_cast<RTXPTNrdHitDistanceReconstructionMode>(std::clamp(HitDistanceMode, 0, 2));
                    RequestRealtimeReset(RTXPT_REALTIME_RESET_NRD_HISTORY, "REBLUR hit-distance mode changed");
                }
                ResetRealtimeOnChange(ImGui::Checkbox("Enable Firefly Filter", &Reblur.EnableAntiFirefly),
                                      "REBLUR anti-firefly toggled");
            }
            else
            {
                RTXPTNrdRelaxUiSettings& Relax = m_RealtimeUI.RelaxSettings;
                ResetRealtimeOnChange(ImGui::SliderFloat("Diffuse Prepass Blur Radius", &Relax.DiffusePrepassBlurRadius, 0.0f, 100.0f),
                                      "RELAX diffuse prepass blur radius changed");
                ResetRealtimeOnChange(ImGui::SliderFloat("Specular Prepass Blur Radius", &Relax.SpecularPrepassBlurRadius, 0.0f, 100.0f),
                                      "RELAX specular prepass blur radius changed");
                SliderUint("Diffuse Max Accumulated Frames", Relax.DiffuseMaxAccumulatedFrameNum, 0, 500,
                           "RELAX diffuse max accumulated frames changed");
                SliderUint("Specular Max Accumulated Frames", Relax.SpecularMaxAccumulatedFrameNum, 0, 500,
                           "RELAX specular max accumulated frames changed");
                SliderUint("Diffuse Fast Max Accumulated Frames", Relax.DiffuseMaxFastAccumulatedFrameNum, 0, 10,
                           "RELAX diffuse fast max accumulated frames changed");
                SliderUint("Specular Fast Max Accumulated Frames", Relax.SpecularMaxFastAccumulatedFrameNum, 0, 10,
                           "RELAX specular fast max accumulated frames changed");
                SliderUint("History Fix Frame Num", Relax.HistoryFixFrameNum, 0, 500,
                           "RELAX history fix frames changed");
                SliderUint("Number of Atrous iterations", Relax.AtrousIterationNum, 2, 8,
                           "RELAX atrous iterations changed");
                ResetRealtimeOnChange(ImGui::SliderFloat("Lobe Angle Fraction", &Relax.LobeAngleFraction, 0.0f, 1.0f),
                                      "RELAX lobe angle fraction changed");
                ResetRealtimeOnChange(ImGui::SliderFloat("Specular Lobe Angle Slack", &Relax.SpecularLobeAngleSlack, 0.0f, 1.0f),
                                      "RELAX specular lobe angle slack changed");
                ResetRealtimeOnChange(ImGui::SliderFloat("Edge Stopping Threshold", &Relax.DepthThreshold, 0.0f, 0.1f),
                                      "RELAX depth threshold changed");
                ResetRealtimeOnChange(ImGui::SliderFloat("Antilag Acceleration Amount", &Relax.AntilagAccelerationAmount, 0.0f, 1.0f),
                                      "RELAX antilag acceleration changed");
                ResetRealtimeOnChange(ImGui::SliderFloat("Antilag Spatial Sigma Scale", &Relax.AntilagSpatialSigmaScale, 0.0f, 5.0f),
                                      "RELAX antilag spatial sigma changed");
                ResetRealtimeOnChange(ImGui::SliderFloat("Antilag Temporal Sigma Scale", &Relax.AntilagTemporalSigmaScale, 0.0f, 5.0f),
                                      "RELAX antilag temporal sigma changed");
                ResetRealtimeOnChange(ImGui::SliderFloat("Antilag Reset Amount", &Relax.AntilagResetAmount, 0.0f, 1.0f),
                                      "RELAX antilag reset amount changed");
                int HitDistanceMode = static_cast<int>(Relax.HitDistanceReconstructionMode);
                if (ImGui::Combo("Hit Distance Reconstruction Mode", &HitDistanceMode, "Off\0AREA_3X3\0AREA_5X5\0\0"))
                {
                    Relax.HitDistanceReconstructionMode =
                        static_cast<RTXPTNrdHitDistanceReconstructionMode>(std::clamp(HitDistanceMode, 0, 2));
                    RequestRealtimeReset(RTXPT_REALTIME_RESET_NRD_HISTORY, "RELAX hit-distance mode changed");
                }
                ResetRealtimeOnChange(ImGui::Checkbox("Enable Firefly Filter", &Relax.EnableAntiFirefly),
                                      "RELAX anti-firefly toggled");
            }
        }

        ImGui::EndDisabled();
        if (DisableNrdControls && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", GetStandaloneNrdControlsDisabledReason(m_RealtimeUI));

        ImGui::Unindent(Indent);
    }

    // ------------------------------------------------------------------ Environment Map
    if (ImGui::CollapsingHeader("Environment Map"))
    {
        ImGui::Indent(Indent);

        if (ImGui::Checkbox("Enabled", &m_ReferenceUI.EnvironmentMapEnabled))
            m_EnvMapBakerSettingsDirty = true;

        const char* EnvPreview = "none";
        if (!m_EnvMapSources.empty())
        {
            const int ClampedSource = std::clamp(m_SelectedEnvMapSource, 0, static_cast<int>(m_EnvMapSources.size()) - 1);
            EnvPreview              = m_EnvMapSources[static_cast<size_t>(ClampedSource)].DisplayName.c_str();
        }

        if (ImGui::BeginCombo("Source", EnvPreview))
        {
            for (size_t Index = 0; Index < m_EnvMapSources.size(); ++Index)
            {
                const bool Selected = static_cast<int>(Index) == m_SelectedEnvMapSource;
                if (ImGui::Selectable(m_EnvMapSources[Index].DisplayName.c_str(), Selected))
                {
                    m_SelectedEnvMapSource              = static_cast<int>(Index);
                    m_EnvMapSettings.SourceRelativePath = m_EnvMapSources[Index].RelativePath;
                    m_EnvMapBakerDirty                  = true;
                    m_EnvMapBakerSettingsDirty          = true;
                    // UpdateEnvMapBaker resets image history after rebuilding; keep light-source history for remapping.
                }
                if (Selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (ImGui::SliderFloat("Intensity", &m_EnvIntensity, 0.0f, 20.0f))
            m_EnvMapBakerSettingsDirty = true;

        if (ImGui::SliderFloat("Rotation", &m_EnvMapSettings.RotationRadians, -PI_F, PI_F))
            m_EnvMapBakerSettingsDirty = true;

        const RTXPTEnvMapBakerStats& EnvStats = m_EnvMapBaker.GetStats();
        if (EnvStats.Ready)
            ImGui::Text("Cube resolution: %u", EnvStats.CubeResolution);
        else
            ImGui::Text("Cube resolution: pending");
        PlaceholderTooltip("DiligentFX PBR precompute currently owns the processed cubemap resolution.");

        ImGui::Text("BC6H compression: not wired");
        PlaceholderTooltip("BC6H output compression is not wired into the current DiligentFX precompute path.");

        m_EnvMapBaker.InfoGUI(Indent);

        ImGui::Unindent(Indent);
    }

    // ------------------------------------------------------------------------- Scene
    if (ImGui::CollapsingHeader("Scene"))
    {
        ImGui::Indent(Indent);

        const char* ScenePreview = "none";
        if (!m_CurrentSceneName.empty())
            ScenePreview = m_CurrentSceneName.c_str();
        else if (m_AvailableScenes.empty())
            ScenePreview = "no scenes found";

        ImGui::BeginDisabled(m_AvailableScenes.empty());
        if (ImGui::BeginCombo("Scene##SceneSelection", ScenePreview))
        {
            for (const std::string& SceneName : m_AvailableScenes)
            {
                const bool IsSelected = SceneName == m_CurrentSceneName;
                if (ImGui::Selectable(SceneName.c_str(), IsSelected))
                    SetCurrentScene(SceneName);
                if (IsSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        if (m_AvailableScenes.empty())
            ImGui::TextWrapped("No RTXPT scene files found under assets root: %s", m_AssetsRoot.c_str());

        const RTXPTSceneGeometryStats& GeometryStats = m_Scene.GetGeometryStats();
        ImGui::BeginDisabled(!GeometryStats.HasAnimations || kReferencePathTraceMode);
        ResetOnChange(ImGui::Checkbox("Enable animations", &m_EnableSceneAnimations), "Scene animations toggled");
        ImGui::EndDisabled();
        if (kReferencePathTraceMode)
            PlaceholderTooltip("Animations are not available in reference mode.");
        else if (!GeometryStats.HasAnimations)
            PlaceholderTooltip("This scene does not contain animations.");

        ImGui::Text("Scene: %s", m_Scene.HasValidContent() ? "loaded" : "missing");
        ImGui::Text("Scene file: %s", m_Scene.GetLoadedSceneName().empty() ? "none" : m_Scene.GetLoadedSceneName().c_str());
        ImGui::Text("Model path: %s", m_Scene.GetModelPath().empty() ? "none" : m_Scene.GetModelPath().c_str());
        ImGui::Text("Scene cameras: %u", m_Scene.GetCameraCount());
        if (m_Scene.GetCameraCount() > 0)
        {
            const char* PreviewName = "none";
            if (m_SelectedSceneCamera >= 0)
            {
                if (const RTXPTSceneCamera* pSelectedCamera = m_Scene.GetCamera(static_cast<Uint32>(m_SelectedSceneCamera)))
                    PreviewName = pSelectedCamera->Name.c_str();
            }

            if (ImGui::BeginCombo("Scene camera", PreviewName))
            {
                for (Uint32 CameraIdx = 0; CameraIdx < m_Scene.GetCameraCount(); ++CameraIdx)
                {
                    const RTXPTSceneCamera* pCamera = m_Scene.GetCamera(CameraIdx);
                    if (pCamera == nullptr)
                        continue;

                    const bool IsSelected = static_cast<int>(CameraIdx) == m_SelectedSceneCamera;
                    ImGui::PushID(static_cast<int>(CameraIdx));
                    if (ImGui::Selectable(pCamera->Name.c_str(), IsSelected))
                        ApplySceneCamera(CameraIdx);
                    if (IsSelected)
                        ImGui::SetItemDefaultFocus();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }

        if (ResetOnChange(ImGui::InputFloat("Aperture", &m_ReferenceUI.CameraAperture, 0.001f, 0.01f, "%.4f"),
                          "Camera aperture changed"))
        {
            m_ReferenceUI.CameraAperture = std::clamp(m_ReferenceUI.CameraAperture, 0.0f, 1.0f);
            InvalidatePreviousFrameConstants();
            ResetTaaSrOnChange(true, "Camera aperture changed");
        }

        if (ResetOnChange(ImGui::InputFloat("Focal Distance", &m_ReferenceUI.CameraFocalDistance, 0.1f),
                          "Camera focal distance changed"))
        {
            m_ReferenceUI.CameraFocalDistance = std::clamp(m_ReferenceUI.CameraFocalDistance, 0.001f, 1.0e16f);
            InvalidatePreviousFrameConstants();
            ResetTaaSrOnChange(true, "Camera focal distance changed");
        }

        m_ReferenceUI.CameraAperture      = std::clamp(m_ReferenceUI.CameraAperture, 0.0f, 1.0f);
        m_ReferenceUI.CameraFocalDistance = std::clamp(m_ReferenceUI.CameraFocalDistance, 0.001f, 1.0e16f);

        bool ClipPlanesChanged = false;
        ClipPlanesChanged |= ImGui::InputFloat("zNear", &m_CameraNearPlane, 0.1f, 1.0f, "%.6f");
        ClipPlanesChanged |= ImGui::InputFloat("zFar", &m_CameraFarPlane, 10.0f, 100.0f, "%.1f");
        if (ClipPlanesChanged)
        {
            SanitizeCameraClipPlanes(m_CameraNearPlane, m_CameraFarPlane);
            const SwapChainDesc& SCDesc = m_pSwapChain->GetDesc();
            UpdateCameraProjection(SCDesc.Width, SCDesc.Height);
            RequestAccumulationReset("Camera clip planes changed");
            RequestRealtimeReset(RTXPT_REALTIME_RESET_TAA_SR_HISTORY, "Camera clip planes changed");
            InvalidatePreviousFrameConstants();
        }

        const RTXPTSceneAdapterStats& AdapterStats = m_Scene.GetAdapterStats();
        ImGui::Text("Graph nodes: %u", AdapterStats.GraphNodeCount);
        ImGui::Text("Model assets: %u", AdapterStats.ModelAssetCount);
        ImGui::Text("Model instances: %u", AdapterStats.ModelInstanceCount);
        ImGui::Text("Material extensions: %u", AdapterStats.MaterialExtensionCount);
        ImGui::Text("Material fallbacks: %u", AdapterStats.MaterialFallbackCount);
        ImGui::Text("Directional lights: %u", AdapterStats.DirectionalLightCount);
        ImGui::Text("Point lights: %u", AdapterStats.PointLightCount);
        ImGui::Text("Spot lights: %u", AdapterStats.SpotLightCount);
        ImGui::Text("Environment lights: %u", AdapterStats.EnvironmentLightCount);
        ImGui::Text("Unknown typed nodes: %u", AdapterStats.UnknownTypedNodeCount);
        ImGui::Text("Skinned instances: %u", AdapterStats.SkinnedInstanceCount);
        ImGui::Text("Mesh nodes: %u", m_Scene.GetMeshNodeCount());
        ImGui::Text("Primitives: %u", m_Scene.GetPrimitiveCount());
        ImGui::Text("Materials: %u", m_Materials.GetStats().MaterialCount);
        ImGui::Text("Lights: %u", m_Lights.GetStats().LightCount);
        ImGui::Text("Emissive triangles: %u", m_Lights.GetStats().EmissiveTriangleCount);
        ImGui::Text("Animations: %s", GeometryStats.HasAnimations ? "yes" : "no");
        ImGui::Text("Skinned nodes: %u", GeometryStats.SkinnedNodeCount);
        ImGui::Text("Skinned primitives: %u", GeometryStats.SkinnedPrimitiveCount);

        ImGui::Unindent(Indent);
    }

    // ------------------------------------------------------------------ Status / Debug
    if (ImGui::CollapsingHeader("Status / Debug"))
    {
        ImGui::Indent(Indent);

        const RTXPTAccelerationStructureStats& ASStats     = m_AccelerationStructures.GetStats();
        const RTXPTRayTracingPassStats&        RTPassStats = m_RayTracingPass.GetStats();

        ImGui::Text("Backend: %s", GetRenderDeviceTypeString(m_pDevice->GetDeviceInfo().Type));
        ImGui::Text("RayTracing: %s", m_FeatureCaps.RayTracing ? "yes" : "no");
        ImGui::Text("Standalone RT shaders: %s", m_FeatureCaps.StandaloneRayTracingShaders ? "yes" : "no");
        ImGui::Text("RayQuery: %s", m_FeatureCaps.RayQuery ? "yes" : "no");
        ImGui::Text("Bindless: %s", m_FeatureCaps.BindlessResources ? "yes" : "no");
        ImGui::Text("Compute: %s", m_FeatureCaps.ComputeShaders ? "yes" : "no");
        ImGui::Text("Assets root: %s", m_AssetsRoot.c_str());
        ImGui::Separator();
        ImGui::Text("Acceleration structures: %s", m_AccelerationStructures.IsBuilt() ? "built" : "not built");
        ImGui::Text("BLAS: %u", ASStats.BLASCount);
        ImGui::Text("TLAS instances: %u", ASStats.InstanceCount);
        ImGui::Text("RT geometries: %u", ASStats.GeometryCount);
        ImGui::Text("Sub-instances: %u", ASStats.SubInstanceCount);
        ImGui::Text("Alpha-tested geometries: %u", ASStats.AlphaTestedGeometryCount);
        ImGui::Text("Excluded from NEE geometries: %u", ASStats.ExcludedFromNEEGeometryCount);
        ImGui::Separator();
        ImGui::Text("Frame constants: %s", m_FrameConstantsCB ? "created" : "missing");
        ImGui::Text("Frame index: %u", m_FrameIndex);
        ImGui::Text("Path tracer mode: %s", m_RealtimeUI.RealtimeMode ? "Realtime" : "Reference");
        if (m_RealtimeUI.RealtimeMode)
        {
            ImGui::Text("Realtime PathTrace: %s", m_LastRealtimePathTraceExecuted ? "BUILD/FILL dispatched" : "not dispatched");
            ImGui::Text("Realtime final merge: %s", m_LastRealtimeFinalMergeReady ? "ready" : "not dispatched");
            if (!m_RealtimePathTraceStatus.empty())
                ImGui::TextWrapped("Realtime PathTrace status: %s", m_RealtimePathTraceStatus.c_str());
        }
        ImGui::Text("Realtime samples per pixel: %u", m_RealtimeUI.ActualSamplesPerPixel());
        ImGui::Text("Realtime AA/SR: %s", GetRealtimeAAModeName(m_RealtimeUI.RealtimeAA));
        ImGui::Text("Standalone NRD requested: %s", m_RealtimeUI.StandaloneDenoiser ? "yes" : "no");
        ImGui::Text("Standalone NRD active: %s", ShouldRunStandaloneNrd(m_RealtimeUI) ? "yes" : "no");
        ImGui::Text("NRD availability: %s", IsStandaloneNrdAvailable() ? "available" : GetStandaloneNrdDisabledReason());
        ImGui::Text("NRD method: %s", GetNrdMethodName(m_RealtimeUI.NRDMethod));
        ImGui::Text("Stable planes active: %d / %u", m_RealtimeUI.DenoisingLayersActiveCount, kRTXPTDenoisingLayerCount);
        ImGui::Text("Current realtime reset flags: 0x%08x", static_cast<Uint32>(m_CurrentFrameRealtimeReset));
        ImGui::Text("Pending realtime reset flags: 0x%08x", static_cast<Uint32>(m_RealtimeResetPending));
        ImGui::Text("Path-trace frame index: %u", m_LastFrameConstants.ptConsts.frameIndex);
        ImGui::Text("Reference sample index: %u", m_AccumulationFrame);
        ImGui::Text("Realtime sample index: %u", m_RealtimeSampleIndex);
        ImGui::Text("Sample base index: %u", m_LastSampleBaseIndex);
        ImGui::Text("Sub-sample count inverse: %.4f", m_LastFrameConstants.ptConsts.invSubSampleCount);
        ImGui::Text("Pre-exposed gray luminance: %.4f", m_LastFrameConstants.ptConsts.preExposedGrayLuminance);
        ImGui::Text("Generic TS stride: line=%u plane=%u",
                    m_LastFrameConstants.ptConsts.genericTSLineStride,
                    m_LastFrameConstants.ptConsts.genericTSPlaneStride);
        ImGui::Text("Viewport: %.0f x %.0f", m_LastFrameConstants.viewportSizeAndFrameIndex.x, m_LastFrameConstants.viewportSizeAndFrameIndex.y);
        ImGui::Separator();
        ImGui::Text("OutputColor: %s", m_RenderTargets.GetOutputColorSRV() != nullptr ? "created" : "missing");
        ImGui::Text("TraceRays pass: %s", m_RayTracingPass.IsReady() ? "ready" : "not ready");
        const RTXPTRayTracingVariantStats& ReferenceStats = m_RayTracingPass.GetVariantStats(RTXPTPathTraceVariant::Reference);
        const RTXPTRayTracingVariantStats& BuildStats     = m_RayTracingPass.GetVariantStats(RTXPTPathTraceVariant::BuildDenoisingLayers);
        const RTXPTRayTracingVariantStats& FillStats      = m_RayTracingPass.GetVariantStats(RTXPTPathTraceVariant::FillDenoisingLayers);
        ImGui::Text("Reference variant: %s, dispatches=%u", ReferenceStats.Ready ? "ready" : "missing", ReferenceStats.TraceCount);
        ImGui::Text("BuildDenoisingLayers variant: %s, dispatches=%u", BuildStats.Ready ? "ready" : "missing", BuildStats.TraceCount);
        ImGui::Text("FillDenoisingLayers variant: %s, dispatches=%u", FillStats.Ready ? "ready" : "missing", FillStats.TraceCount);
        ImGui::Text("Material bridge: %s", RTPassStats.MaterialBridgeBound ? "bound" : "fallback");
        ImGui::Text("Sub-instance bridge: %s", RTPassStats.SubInstanceBound ? "bound" : "fallback");
        ImGui::Text("Light bridge: %s", RTPassStats.LightBridgeBound ? "bound" : "fallback");
        ImGui::Text("Environment bridge: %s", RTPassStats.EnvironmentBridgeBound ? "bound" : "missing");
        const RTXPTEnvMapBakerStats& EnvStats = m_EnvMapBaker.GetStats();
        ImGui::Text("EnvMapBaker: %s", EnvStats.Ready ? "ready" : "not ready");
        ImGui::Text("Env source: %s", EnvStats.SourceName.empty() ? "none" : EnvStats.SourceName.c_str());
        ImGui::Text("Env importance: %s", EnvStats.ImportanceReady ? "ready" : "missing");
        const RTXPTLightsBakerStats& BakerStats = m_LightsBaker.GetStats();
        ImGui::Text("LightsBaker lights: %u", BakerStats.TotalLightCount);
        ImGui::Text("LightsBaker proxies: %u", BakerStats.SamplingProxyCount);
        ImGui::Text("LightsBaker proxy weight: %.3f", BakerStats.ProxyTotalWeight);
        ImGui::Text("LightsBaker feedback: %s", BakerStats.FeedbackReady ? "ready" : "missing");
        ImGui::Text("LightsBaker update: %u", BakerStats.UpdateCounter);
        ImGui::Text("LightsBaker bridge: %s", RTPassStats.LightsBakerBridgeBound ? "bound" : "missing");
        if (m_RealtimeUI.RealtimeMode)
        {
            ImGui::Text("Realtime fill light resources: %s",
                        RTPassStats.LightBridgeBound && RTPassStats.LightsBakerBridgeBound && RTPassStats.EmissiveLightBridgeBound ?
                            "bound" :
                            "missing");
        }
        m_EnvMapBaker.DebugGUI(Indent);
        ImGui::Text("Emissive triangle pass: %s", m_EmissiveTrianglePass.IsReady() ? "ready" : "not ready");
        ImGui::Text("Emissive triangle dispatch count: %u", m_EmissiveTrianglePass.GetStats().DispatchCount);
        ImGui::Text("Vertex buffer: %s", RTPassStats.VertexBufferBound ? "bound" : "fallback");
        ImGui::Text("Skinned vertex buffer: %s", RTPassStats.SkinnedVertexBufferBound ? "bound" : "fallback");
        ImGui::Text("Skinning pass: %s", m_SkinnedGeometry.IsReady() ? "ready" : "not ready");
        ImGui::Text("Index buffer: %s", RTPassStats.IndexBufferBound ? "bound" : "fallback");
        ImGui::Text("Material textures loaded: %u", m_Materials.GetStats().TextureCount);
        ImGui::Text("Material textures bound: %s (%u)", RTPassStats.MaterialTexturesBound ? "yes" : "no", RTPassStats.MaterialTextureCount);
        ImGui::Text("Alpha-test / NEE any-hit: %s", RTPassStats.AnyHitEnabled ? "enabled" : "disabled");
        ImGui::Text("Visibility rays: %s", RTPassStats.InlineVisibilityEnabled ? "inline RayQuery" : "TraceRay");
        ImGui::Text("AccumulatedRadiance: %s", m_AccumulationActive ? "active (RGBA32F)" : "inactive (RGBA32F unavailable)");
        ImGui::Text("Post-process targets: %s", m_RenderTargets.HasPostProcessTargets() ? "allocated" : "missing");
        ImGui::Text("Realtime render targets: %s",
                    m_RenderTargets.AreRealtimeRenderTargetsRequested() ?
                        (m_RenderTargets.HasRealtimeRenderTargets() ? "allocated" : "missing") :
                        "not requested");
        if (m_RenderTargets.AreRealtimeRenderTargetsRequested())
        {
            ImGui::Text("DenoisingLayersBuffer elements: %llu",
                        static_cast<unsigned long long>(m_RenderTargets.GetDenoisingLayersElementCount()));
            ImGui::Text("StableRadiance: %s", m_RenderTargets.GetStableRadianceSRV() != nullptr ? "created" : "missing");
            ImGui::Text("DenoisingLayersHeader: %s", m_RenderTargets.GetDenoisingLayersHeaderSRV() != nullptr ? "created" : "missing");
            ImGui::Text("DenoisingLayersBuffer: %s", m_RenderTargets.GetDenoisingLayersBufferSRV() != nullptr ? "created" : "missing");
            ImGui::Text("NRD guide inputs: %s",
                        m_RenderTargets.GetDenoiserViewspaceZSRV() != nullptr &&
                                m_RenderTargets.GetDenoiserMotionVectorsSRV() != nullptr &&
                                m_RenderTargets.GetDenoiserNormalRoughnessSRV() != nullptr ?
                            "created" :
                            "missing");
        }
        const RTXPTDenoisingGuidesBakerStats& GuideStats = m_DenoisingGuidesBaker.GetStats();
        ImGui::Text("DenoisingGuidesBaker: %s", GuideStats.Ready ? "ready" : "not ready");
        ImGui::Text("DenoiseSpecHitT: %s (%u)",
                    GuideStats.LastDenoiseSpecHitTExecuted ? "dispatched" : "not dispatched",
                    GuideStats.DenoiseSpecHitTDispatchCount);
        ImGui::Text("Avg layer radiance: %s (%u)",
                    GuideStats.LastAvgLayerRadianceExecuted ? "dispatched" : "not dispatched",
                    GuideStats.AvgLayerRadianceDispatchCount);
        ImGui::Text("Guide debug view: %s",
                    GetDenoisingGuideDebugViewName(m_RealtimeUI.DenoisingGuideDebugView));
        if (m_RenderTargets.GetLastFailureReason()[0] != '\0')
            ImGui::TextWrapped("Render target error: %s", m_RenderTargets.GetLastFailureReason());
        ImGui::Text("Post-process pipeline: %s", m_PostProcessPipeline.IsReady() ? "ready" : "not ready");
        const auto& PostStats = m_PostProcessPipeline.GetStats();
        ImGui::Text("Bloom stage: %s", PostStats.BloomStageReady ? "ready" : "not ready");
        const auto& TAAStats = m_PostProcessPipeline.GetTemporalAAPass().GetStats();
        const auto& SRStats  = m_PostProcessPipeline.GetSuperResolutionPass().GetStats();
        ImGui::Text("Realtime TAA: %s, executed=%s",
                    TAAStats.Ready ? "ready" : "not ready",
                    PostStats.LastTemporalAAActive ? "yes" : "no");
        if (!TAAStats.DisabledReason.empty())
            ImGui::TextWrapped("Realtime TAA status: %s", TAAStats.DisabledReason.c_str());
        ImGui::Text("Super resolution: variants=%u, active=%s, executed=%s",
                    SRStats.VariantCount,
                    m_CurrentSuperResolutionFrame.Enabled ? "yes" : "no",
                    PostStats.LastSuperResolutionActive ? "yes" : "no");
        if (!SRStats.DisabledReason.empty())
            ImGui::TextWrapped("Super resolution status: %s", SRStats.DisabledReason.c_str());
        ImGui::Text("Accumulation frame: %u", m_AccumulationFrame);
        ImGui::Text("TraceRays executed: %s", RTPassStats.LastTraceExecuted ? "yes" : "no");
        ImGui::Text("TraceRays count: %u", RTPassStats.TraceCount);
        ImGui::Separator();
        ImGui::Text("Presentation source: LdrColor");
        ImGui::Text("Blit draw count: %u", m_BlitPass.GetDrawCount());

        ImGui::Unindent(Indent);
    }

    ImGui::End();
}

void RTXPTSample::RequestAccumulationReset(const char* /*Reason*/, bool ResetTemporalHistory, bool ResetLightFeedback)
{
    m_AccumulationFrame        = 0;
    m_ResetAccumulationPending = true;
    if (ResetTemporalHistory) m_Sharc.Invalidate();
    if (ResetLightFeedback && (!m_RealtimeUI.RealtimeMode || ResetTemporalHistory))
        m_LightsBaker.RequestFeedbackReset();

    if (!m_RealtimeUI.RealtimeMode || ResetTemporalHistory)
    {
        m_RealtimeResetPending |= RTXPT_REALTIME_RESET_REALTIME_CACHES |
            RTXPT_REALTIME_RESET_NRD_HISTORY |
            RTXPT_REALTIME_RESET_TAA_SR_HISTORY;
    }
}

void RTXPTSample::RequestRealtimeReset(RTXPTRealtimeResetFlags Flags, const char* Reason)
{
    if (HasRealtimeResetFlag(Flags, RTXPT_REALTIME_RESET_REALTIME_CACHES)) m_Sharc.Invalidate();
    if (HasRealtimeResetFlag(Flags, RTXPT_REALTIME_RESET_ACCUMULATION))
        RequestAccumulationReset(Reason);

    m_RealtimeResetPending |= Flags;

    if (HasRealtimeResetFlag(Flags, RTXPT_REALTIME_RESET_RENDER_TARGET_RECREATE) ||
        HasRealtimeResetFlag(Flags, RTXPT_REALTIME_RESET_TAA_SR_HISTORY))
    {
        InvalidatePreviousFrameConstants();
    }

    if (HasRealtimeResetFlag(Flags, RTXPT_REALTIME_RESET_REALTIME_CACHES))
        m_LightsBaker.RequestFeedbackReset();
}

void RTXPTSample::RequestRealtimeCachesReset(const char* Reason)
{
    RequestRealtimeReset(RTXPT_REALTIME_RESET_REALTIME_CACHES |
                             RTXPT_REALTIME_RESET_NRD_HISTORY |
                             RTXPT_REALTIME_RESET_TAA_SR_HISTORY,
                         Reason);
}

void RTXPTSample::BeginRealtimeFrameResetScope()
{
    m_CurrentFrameRealtimeReset = m_RealtimeResetPending;
    m_RealtimeResetPending      = RTXPT_REALTIME_RESET_NONE;
}

void RTXPTSample::InvalidatePreviousFrameConstants()
{
    m_HasPreviousFrameConstants = false;
}

#include "../tests/RRSceneValidation.hpp"
#include "../tests/SubSamplingSceneValidation.hpp"

} // namespace Diligent
