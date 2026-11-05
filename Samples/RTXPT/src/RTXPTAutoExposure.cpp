#include "RTXPTAutoExposure.hpp"
#include "RTXPTPipelineStateCache.hpp"
#include "RTXPTShaderSource.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"
#include <cmath>

namespace Diligent
{
namespace
{
//RTXPTAutoExposureShared.h.
constexpr Uint32 kTileSize          = 16;
constexpr Uint32 kStateTexels       = 2;
constexpr Uint32 kStateTextureCount = 2;

struct RTXPTAutoExposureConstants
{
    float  SeedEV          = 0;
    float  CompensationEV  = 0;
    float  MinEV           = 0;
    float  MaxEV           = 0;
    float  Log2Key         = 0;
    float  BrightenSeconds = 0;
    float  DarkenSeconds   = 0;
    float  DeltaSeconds    = 0;
    Uint32 Seed            = 0;
    Uint32 ValidDelta      = 0;
    Uint32 Width           = 0;
    Uint32 Height          = 0;
    Uint32 TilesX          = 0;
    Uint32 TileCount       = 0;
    Uint32 _Padding0       = 0;
    Uint32 _Padding1       = 0;
};
static_assert(sizeof(RTXPTAutoExposureConstants) == 64, "RTXPTAutoExposureConstants must match RTXPTAutoExposureShared.h");

//RTXPTAutoExposureTile: the sum of a tile's log2 luminances and their count.
constexpr Uint32 kTileStride = sizeof(float) + sizeof(Uint32);
} // namespace

bool IsValidRTXPTAutoExposure(const RTXPTAutoExposureParameters& Params)
{
    //NaN fails every comparison, so it is rejected like a value out of range.
    const bool bKey    = std::isfinite(Params.Key) && Params.Key > 0;
    const bool bLimits = Params.MinEV >= RTXPTMinAutoExposureEV && Params.MaxEV <= RTXPTMaxAutoExposureEV && Params.MinEV <= Params.MaxEV;
    const bool bTimes  = std::isfinite(Params.BrightenSeconds) && std::isfinite(Params.DarkenSeconds) && Params.BrightenSeconds >= 0 && Params.DarkenSeconds >= 0;
    return bKey && bLimits && bTimes && std::isfinite(Params.SeedEV) && std::isfinite(Params.CompensationEV);
}

const char* GetRTXPTAutoExposureSeedReasonName(RTXPTAutoExposureSeedReason Reason)
{
    switch (Reason)
    {
        case RTXPTAutoExposureSeedReason::None: return "none";
        case RTXPTAutoExposureSeedReason::NewView: return "new_view";
        case RTXPTAutoExposureSeedReason::NewScene: return "new_scene";
        case RTXPTAutoExposureSeedReason::CameraCut: return "camera_cut";
        case RTXPTAutoExposureSeedReason::ManualToAuto: return "manual_to_auto";
    }
    return "unknown";
}

const char* GetRTXPTAutoExposureTimeStatusName(RTXPTAutoExposureTimeStatus Status)
{
    switch (Status)
    {
        case RTXPTAutoExposureTimeStatus::Valid: return "valid";
        case RTXPTAutoExposureTimeStatus::Clamped: return "clamped";
        case RTXPTAutoExposureTimeStatus::NoAnchor: return "no_anchor";
        case RTXPTAutoExposureTimeStatus::NotAdvanced: return "not_advanced";
        case RTXPTAutoExposureTimeStatus::Backwards: return "backwards";
        case RTXPTAutoExposureTimeStatus::NonFinite: return "non_finite";
    }
    return "unknown";
}

RefCntAutoPtr<IPipelineState> RTXPTAutoExposure::CreatePipeline(IShaderSourceInputStreamFactory* Shaders, const char* Name, const char* EntryPoint)
{
    ShaderCreateInfo ShaderCI;
    ShaderCI.Desc.ShaderType            = SHADER_TYPE_COMPUTE;
    ShaderCI.Desc.Name                  = Name;
    ShaderCI.SourceLanguage             = SHADER_SOURCE_LANGUAGE_HLSL;
    ShaderCI.ShaderCompiler             = SHADER_COMPILER_DXC;
    ShaderCI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
    ShaderCI.FilePath                   = "PostProcessing/RTXPTAutoExposure.csh";
    ShaderCI.EntryPoint                 = EntryPoint;
    ShaderCI.pShaderSourceStreamFactory = Shaders;
    RefCntAutoPtr<IShader> Shader;
    m_Cache->CreateShader(ShaderCI, &Shader);
    if (!Shader)
        return {};

    ComputePipelineStateCreateInfo PSOCreateInfo;
    PSOCreateInfo.PSODesc.Name         = Name;
    PSOCreateInfo.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    PSOCreateInfo.pCS                  = Shader;
    //The state cache hands every auto exposure the same pipelines, so each binds its constants in its own bindings.
    PipelineResourceLayoutDescX Layout;
    Layout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
    Layout.AddVariable(SHADER_TYPE_COMPUTE, "g_AutoExposureConstants", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE);
    PSOCreateInfo.PSODesc.ResourceLayout = Layout;
    PSOCreateInfo.pPSOCache              = SelectRTXPTPipelineStateCache(m_Device, PSOCreateInfo, m_PSOCache);
    RefCntAutoPtr<IPipelineState> PSO;
    m_Cache->CreateComputePipelineState(PSOCreateInfo, &PSO);
    return PSO;
}

RTXPTErrorCode RTXPTAutoExposure::Initialize(IRenderDevice* Device, IRenderStateCache* Cache, IPipelineStateCache* PSOCache)
{
    *this = {};
    if (!Device || !Cache)
        return RTXPTErrorCode::InvalidArgument;
    m_Device   = Device;
    m_Cache    = Cache;
    m_PSOCache = PSOCache;
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Shaders;
    CreateRTXPTShaderSourceFactory(Device->GetEngineFactory(), "shaders;shaders\\PostProcessing", &Shaders);

    BufferDesc ConstantsDesc;
    ConstantsDesc.Name           = "RTXPT auto exposure constants";
    ConstantsDesc.Size           = sizeof(RTXPTAutoExposureConstants);
    ConstantsDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    ConstantsDesc.Usage          = USAGE_DYNAMIC;
    ConstantsDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    Device->CreateBuffer(ConstantsDesc, nullptr, &m_Constants);
    if (!Shaders || !m_Constants)
        return RTXPTErrorCode::AutoExposureSetupFailed;
    m_BeginPSO = CreatePipeline(Shaders, "RTXPT auto exposure begin", "BeginFrame");
    m_MeterPSO = CreatePipeline(Shaders, "RTXPT auto exposure meter", "MeterLuminance");
    m_AdaptPSO = CreatePipeline(Shaders, "RTXPT auto exposure adapt", "Adapt");
    if (!m_BeginPSO || !m_MeterPSO || !m_AdaptPSO)
        return RTXPTErrorCode::AutoExposureSetupFailed;
    m_BeginPSO->CreateShaderResourceBinding(&m_BeginSRB, true);
    m_MeterPSO->CreateShaderResourceBinding(&m_MeterSRB, true);
    m_AdaptPSO->CreateShaderResourceBinding(&m_AdaptSRB, true);
    if (!m_BeginSRB || !m_MeterSRB || !m_AdaptSRB)
        return RTXPTErrorCode::AutoExposureSetupFailed;
    for (auto* SRB : {m_BeginSRB.RawPtr(), m_MeterSRB.RawPtr(), m_AdaptSRB.RawPtr()})
    {
        auto* Constants = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, "g_AutoExposureConstants");
        if (!Constants)
            return RTXPTErrorCode::AutoExposureSetupFailed;
        Constants->Set(m_Constants);
    }

    //Ray reconstruction reads the used exposure as its explicit 1x1 R32F exposure texture.
    TextureDesc Desc;
    Desc.Name      = "RTXPT auto exposure used";
    Desc.Type      = RESOURCE_DIM_TEX_2D;
    Desc.Width     = 1;
    Desc.Height    = 1;
    Desc.MipLevels = 1;
    Desc.Format    = TEX_FORMAT_R32_FLOAT;
    Desc.BindFlags = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
    Device->CreateTexture(Desc, nullptr, &m_Used);
    //Zero states: base exposure 0, not initialized. A new state starts from the seed, so they are never consumed.
    const float       Zeros[kStateTexels * 4] = {};
    TextureSubResData StateData{Zeros, sizeof(Zeros)};
    TextureData       StateInit{&StateData, 1};
    Desc.Width  = kStateTexels;
    Desc.Format = TEX_FORMAT_RGBA32_FLOAT;
    for (Uint32 I = 0; I < kStateTextureCount; ++I)
    {
        Desc.Name = I == 0 ? "RTXPT auto exposure state 0" : "RTXPT auto exposure state 1";
        Device->CreateTexture(Desc, &StateInit, &m_States[I]);
        if (!m_States[I])
            return RTXPTErrorCode::AutoExposureSetupFailed;
    }
    if (!m_Used)
        return RTXPTErrorCode::AutoExposureSetupFailed;
    m_Stats.PendingSeed = RTXPTAutoExposureSeedReason::NewView;
    m_Ready             = true;
    return RTXPTErrorCode::Ok;
}

void RTXPTAutoExposure::RequestSeed(RTXPTAutoExposureSeedReason Reason)
{
    if (Reason != RTXPTAutoExposureSeedReason::None && m_Stats.PendingSeed == RTXPTAutoExposureSeedReason::None)
        m_Stats.PendingSeed = Reason;
}

bool RTXPTAutoExposure::UpdateConstants(IDeviceContext* Context)
{
    RTXPTAutoExposureConstants Constants;
    Constants.SeedEV          = m_Params.SeedEV;
    Constants.CompensationEV  = m_Params.CompensationEV;
    Constants.MinEV           = m_Params.MinEV;
    Constants.MaxEV           = m_Params.MaxEV;
    Constants.Log2Key         = std::log2(m_Params.Key);
    Constants.BrightenSeconds = m_Params.BrightenSeconds;
    Constants.DarkenSeconds   = m_Params.DarkenSeconds;
    Constants.DeltaSeconds    = m_Stats.LastDeltaSeconds;
    Constants.Seed            = m_bFrameSeed ? 1u : 0u;
    Constants.ValidDelta      = m_bValidDelta ? 1u : 0u;
    Constants.Width           = m_Stats.MeterWidth;
    Constants.Height          = m_Stats.MeterHeight;
    Constants.TilesX          = m_TilesX;
    Constants.TileCount       = m_TileCount;
    MapHelper<RTXPTAutoExposureConstants> Mapped{Context, m_Constants, MAP_WRITE, MAP_FLAG_DISCARD};
    if (!Mapped)
        return false;
    *Mapped = Constants;
    return true;
}

bool RTXPTAutoExposure::Dispatch(IDeviceContext* Context, IShaderResourceBinding* SRB, IPipelineState* PSO, Uint32 GroupsX, Uint32 GroupsY)
{
    Context->SetPipelineState(PSO);
    Context->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    DispatchComputeAttribs Attribs;
    Attribs.ThreadGroupCountX = GroupsX;
    Attribs.ThreadGroupCountY = GroupsY;
    Context->DispatchCompute(Attribs);
    return true;
}

RTXPTErrorCode RTXPTAutoExposure::BeginFrame(IDeviceContext* Context, const RTXPTAutoExposureParameters& Params, double Time)
{
    Abort();
    m_bFrameBegun = false;
    if (!m_Ready || !Context || !Params.Enabled || !IsValidRTXPTAutoExposure(Params))
        return RTXPTErrorCode::InvalidArgument;
    m_Params    = Params;
    m_FrameTime = Time;
    //The step from the last committed frame, positive and finite, at most RTXPTMaxAutoExposureDeltaSeconds; any other
    //time holds the adaptation. A seed initializes from its first metered frame instead of adapting.
    auto&        Stats        = m_Stats;
    const double Raw          = m_bAnchored ? Time - m_AnchorTime : 0.0;
    Stats.LastRawDeltaSeconds = std::isfinite(Raw) ? Raw : 0.0;
    if (!std::isfinite(Time))
        Stats.LastTimeStatus = RTXPTAutoExposureTimeStatus::NonFinite;
    else if (!m_bAnchored)
        Stats.LastTimeStatus = RTXPTAutoExposureTimeStatus::NoAnchor;
    else if (Raw < 0)
        Stats.LastTimeStatus = RTXPTAutoExposureTimeStatus::Backwards;
    else if (!(Raw > 0))
        Stats.LastTimeStatus = RTXPTAutoExposureTimeStatus::NotAdvanced;
    else if (Raw > RTXPTMaxAutoExposureDeltaSeconds)
        Stats.LastTimeStatus = RTXPTAutoExposureTimeStatus::Clamped;
    else
        Stats.LastTimeStatus = RTXPTAutoExposureTimeStatus::Valid;
    m_bValidDelta          = Stats.LastTimeStatus == RTXPTAutoExposureTimeStatus::Valid || Stats.LastTimeStatus == RTXPTAutoExposureTimeStatus::Clamped;
    Stats.LastDeltaSeconds = m_bValidDelta ? static_cast<float>(std::min(Raw, RTXPTMaxAutoExposureDeltaSeconds)) : 0.0f;
    m_bFrameSeed           = Stats.PendingSeed != RTXPTAutoExposureSeedReason::None;
    Stats.FrameSeed        = Stats.PendingSeed;
    if (!UpdateConstants(Context))
        return RTXPTErrorCode::AutoExposureFailed;
    m_BeginSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_Committed")->Set(m_States[Stats.CommittedIndex]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    m_BeginSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Used")->Set(m_Used->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    Dispatch(Context, m_BeginSRB, m_BeginPSO, 1, 1);
    ++Stats.Frames;
    m_bFrameBegun = true;
    return RTXPTErrorCode::Ok;
}

//The tiles of a meter of Width x Height, created again only when the number of tiles grows.
bool RTXPTAutoExposure::PrepareTiles(Uint32 Width, Uint32 Height)
{
    const Uint32 TilesX = (Width + kTileSize - 1) / kTileSize;
    const Uint32 Count  = TilesX * ((Height + kTileSize - 1) / kTileSize);
    if (!m_Tiles || m_Tiles->GetDesc().Size < Uint64{Count} * kTileStride)
    {
        m_Tiles.Release();
        BufferDesc Desc;
        Desc.Name              = "RTXPT auto exposure tiles";
        Desc.Size              = Uint64{Count} * kTileStride;
        Desc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        Desc.Mode              = BUFFER_MODE_STRUCTURED;
        Desc.ElementByteStride = kTileStride;
        m_Device->CreateBuffer(Desc, nullptr, &m_Tiles);
        if (!m_Tiles)
            return false;
        ++m_Stats.TileCreations;
        m_Stats.ResourceBytes = Desc.Size;
    }
    m_TilesX    = TilesX;
    m_TileCount = Count;
    return true;
}

RTXPTErrorCode RTXPTAutoExposure::Meter(IDeviceContext* Context, ITexture* HDR, Uint32 Width, Uint32 Height, RTXPTGpuProfiler* Profiler)
{
    if (!Context || !HDR || !Width || !Height || HDR->GetDesc().Width < Width || HDR->GetDesc().Height < Height)
        return RTXPTErrorCode::InvalidArgument;
    //Once per begun frame.
    if (!m_bFrameBegun || m_Stats.PendingReady)
        return RTXPTErrorCode::AutoExposureFailed;
    if (!PrepareTiles(Width, Height))
        return RTXPTErrorCode::AutoExposureFailed;
    m_Stats.MeterWidth  = Width;
    m_Stats.MeterHeight = Height;
    if (!UpdateConstants(Context))
        return RTXPTErrorCode::AutoExposureFailed;

    RTXPTGpuProfiler::Scope Timing{Profiler, Context, RTXPTGpuProfiler::Stage::AutoExposure};
    const Uint32            Pending = 1 - m_Stats.CommittedIndex;
    m_MeterSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_HDR")->Set(HDR->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    m_MeterSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Tiles")->Set(m_Tiles->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    Dispatch(Context, m_MeterSRB, m_MeterPSO, m_TilesX, m_TileCount / m_TilesX);
    m_AdaptSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_Tiles")->Set(m_Tiles->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE));
    m_AdaptSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "t_Committed")->Set(m_States[m_Stats.CommittedIndex]->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE));
    m_AdaptSRB->GetVariableByName(SHADER_TYPE_COMPUTE, "u_Pending")->Set(m_States[Pending]->GetDefaultView(TEXTURE_VIEW_UNORDERED_ACCESS));
    Dispatch(Context, m_AdaptSRB, m_AdaptPSO, 1, 1);
    ++m_Stats.Meters;
    m_MeteredIndex       = Pending;
    m_bMetered           = true;
    m_Stats.PendingReady = true;
    return RTXPTErrorCode::Ok;
}

void RTXPTAutoExposure::Commit()
{
    if (!m_Stats.PendingReady)
        return;
    auto& Stats          = m_Stats;
    Stats.PendingReady   = false;
    Stats.CommittedIndex = m_MeteredIndex;
    ++Stats.Commits;
    m_bFrameBegun = false;
    //A frame time that is not finite never becomes the start of the next step; one that went back restarts it.
    if (std::isfinite(m_FrameTime))
    {
        m_AnchorTime = m_FrameTime;
        m_bAnchored  = true;
    }
    if (m_bFrameSeed)
    {
        ++Stats.SeedCommits;
        Stats.LastSeedCommit = Stats.FrameSeed;
        Stats.PendingSeed    = RTXPTAutoExposureSeedReason::None;
    }
}

void RTXPTAutoExposure::Abort()
{
    m_bFrameBegun = false;
    if (!m_Stats.PendingReady)
        return;
    m_Stats.PendingReady = false;
    ++m_Stats.Aborts;
}
} // namespace Diligent
