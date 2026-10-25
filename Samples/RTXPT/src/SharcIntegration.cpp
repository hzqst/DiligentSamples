// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#include "SharcIntegration.hpp"
#include "GraphicsTypesX.hpp"
#include "MapHelper.hpp"
#include "RenderStateCache.h"
#include <algorithm>
#include <cstddef>

namespace Diligent
{
namespace
{
using uint = Uint32;
#include "../shaders/PathTracer/SharcConstants.h"
static_assert(sizeof(SharcConstants) == 64, "SHaRC constant ABI");
static_assert(offsetof(SharcConstants, CameraPosition) == 32, "SHaRC camera ABI");
constexpr Uint32 kPathStateStride = RTXPT_SHARC_PATH_STATE_STRIDE;
} // namespace

bool SharcIntegration::IsSupported(IRenderDevice* pDevice)
{
    const auto& Info = pDevice->GetDeviceInfo();
    return Info.Type == RENDER_DEVICE_TYPE_D3D12 && Info.MaxShaderVersion.HLSL >= ShaderVersion{6, 6} &&
        Info.Features.ShaderFloat16 == DEVICE_FEATURE_STATE_ENABLED &&
        Info.Features.ResourceBuffer16BitAccess == DEVICE_FEATURE_STATE_ENABLED;
}

void SharcIntegration::Reset()
{
    m_ClearSRB.Release();
    m_ResolveSRB.Release();
    m_Hash.Release();
    m_Accumulation.Release();
    m_Resolved.Release();
    m_PathState.Release();
    m_Capacity = m_PathSlots = 0;
    m_NeedsClear             = true;
}

bool SharcIntegration::Initialize(IRenderDevice* pDevice, IEngineFactory* pFactory, IRenderStateCache* pCache)
{
    if (!IsSupported(pDevice)) return false;
    if (m_ClearPSO && m_ResolvePSO) return true;
    BufferDesc CB;
    CB.Name           = "SHaRC constants";
    CB.Size           = sizeof(SharcConstants);
    CB.BindFlags      = BIND_UNIFORM_BUFFER;
    CB.Usage          = USAGE_DYNAMIC;
    CB.CPUAccessFlags = CPU_ACCESS_WRITE;
    pDevice->CreateBuffer(CB, nullptr, &m_Constants);
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Factory;
    pFactory->CreateDefaultShaderSourceStreamFactory("shaders;shaders/PathTracer;shaders/SHARC;" RTXPT_SHARC_SHADER_DIR, &Factory);
    for (bool Clear : {true, false})
    {
        ShaderCreateInfo CI;
        CI.Desc.Name       = Clear ? "SHaRC clear" : "SHaRC resolve";
        CI.Desc.ShaderType = SHADER_TYPE_COMPUTE;
        CI.SourceLanguage  = SHADER_SOURCE_LANGUAGE_HLSL;
        CI.ShaderCompiler  = SHADER_COMPILER_DXC;
        // DEFAULT becomes -Od in Debug. Avoid the known unoptimized DXC path.
        CI.ShaderOptimizationLevel    = SHADER_OPTIMIZATION_LEVEL_3;
        CI.HLSLVersion                = {6, 6};
        CI.CompileFlags               = SHADER_COMPILE_FLAG_ENABLE_16BIT_TYPES;
        CI.FilePath                   = "PathTracer/SharcResolve.hlsl";
        CI.EntryPoint                 = Clear ? "Clear" : "Resolve";
        CI.pShaderSourceStreamFactory = Factory;
        RefCntAutoPtr<IShader> Shader;
        pCache->CreateShader(CI, &Shader);
        if (!Shader) return false;
        ComputePipelineStateCreateInfo PSO;
        PSO.PSODesc.Name                               = CI.Desc.Name;
        PSO.PSODesc.PipelineType                       = PIPELINE_TYPE_COMPUTE;
        PSO.PSODesc.ResourceLayout.DefaultVariableType = SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;
        PSO.pCS                                        = Shader;
        pCache->CreateComputePipelineState(PSO, Clear ? &m_ClearPSO : &m_ResolvePSO);
    }
    return m_Constants && m_ClearPSO && m_ResolvePSO;
}

bool SharcIntegration::Prepare(IRenderDevice* pDevice, IDeviceContext* pContext, const SharcSettings& Settings, Uint32 Width, Uint32 Height, const float3& CameraPosition)
{
    const Uint32 Capacity = 1u << std::clamp(Settings.CapacityLog2, 16, 24);
    const Uint32 Scale    = static_cast<Uint32>(std::clamp(Settings.DownscaleFactor, 2, 5));
    const Uint32 Slots    = ((Width + Scale - 1) / Scale) * ((Height + Scale - 1) / Scale);
    auto         Allocate = [&](RefCntAutoPtr<IBuffer>& Buffer, const char* Name, Uint32 Count, Uint32 Stride, bool Raw = false) {
        Buffer.Release();
        BufferDesc Desc;
        Desc.Name              = Name;
        Desc.Size              = Uint64{Count} * Stride;
        Desc.BindFlags         = BIND_UNORDERED_ACCESS | BIND_SHADER_RESOURCE;
        Desc.Mode              = Raw ? BUFFER_MODE_RAW : BUFFER_MODE_STRUCTURED;
        Desc.ElementByteStride = Raw ? 0 : Stride;
        pDevice->CreateBuffer(Desc, nullptr, &Buffer);
    };
    if (Capacity != m_Capacity)
    {
        Reset();
        Allocate(m_Hash, "SHaRC hash", Capacity, 8);
        Allocate(m_Accumulation, "SHaRC accumulation", Capacity, 16);
        Allocate(m_Resolved, "SHaRC resolved", Capacity, 16);
        m_Capacity = Capacity;
    }
    if (Slots != m_PathSlots)
    {
        Allocate(m_PathState, "SHaRC path propagation", Slots, kPathStateStride, true);
        m_PathSlots = Slots;
    }
    if (!m_Hash || !m_Accumulation || !m_Resolved || !m_PathState || !m_Constants) return false;
    if (m_SceneScale != Settings.SceneScale) m_NeedsClear = true;
    m_SceneScale = Settings.SceneScale;
    SharcConstants Constants{};
    Constants.EntriesNum           = Capacity;
    Constants.DownscaleFactor      = Scale;
    Constants.AccumulationFrameNum = std::clamp(Settings.AccumulationFrameNum, 1, 1024);
    Constants.StaleFrameNum        = std::clamp(Settings.StaleFrameNum, 8, 1024);
    Constants.SceneScale           = std::max(Settings.SceneScale, .001f);
    Constants.TerminateThreshold   = std::max(Settings.TerminateThreshold, 0.f);
    Constants.MinRoughnessOverride = std::clamp(Settings.MinRoughnessOverride, 0.f, 1.f);
    Constants.DebugMode            = std::clamp(Settings.DebugMode, 0, 6);
    Constants.CameraPosition       = float4{CameraPosition, 0};
    Constants.CameraPositionPrev   = float4{m_NeedsClear ? CameraPosition : m_PreviousCamera, 0};
    {
        MapHelper<SharcConstants> Mapped{pContext, m_Constants, MAP_WRITE, MAP_FLAG_DISCARD};
        *Mapped = Constants;
    }
    m_PreviousCamera = CameraPosition;
    if (m_NeedsClear)
    {
        if (!DispatchCompute(pContext, true)) return false;
        m_NeedsClear = false;
    }
    return true;
}

void SharcIntegration::Barrier(IDeviceContext* pContext)
{
    for (IBuffer* Buffer : {m_Hash.RawPtr(), m_Accumulation.RawPtr(), m_Resolved.RawPtr(), m_PathState.RawPtr()})
    {
        StateTransitionDesc Transition{Buffer, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_UNORDERED_ACCESS, STATE_TRANSITION_FLAG_UPDATE_STATE};
        pContext->TransitionResourceStates(1, &Transition);
    }
}

bool SharcIntegration::DispatchCompute(IDeviceContext* pContext, bool Clear)
{
    auto& PSO = Clear ? m_ClearPSO : m_ResolvePSO;
    auto& SRB = Clear ? m_ClearSRB : m_ResolveSRB;
    if (!PSO) return false;
    if (!SRB) PSO->CreateShaderResourceBinding(&SRB, true);
    auto Set = [&](const char* Name, IDeviceObject* Object) {
        if (auto* Var = SRB->GetVariableByName(SHADER_TYPE_COMPUTE, Name)) Var->Set(Object);
    };
    Set("cbSharc", m_Constants);
    Set("u_SharcHash", m_Hash->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    Set("u_SharcAccumulation", m_Accumulation->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    Set("u_SharcResolved", m_Resolved->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS));
    if (!Clear) Barrier(pContext);
    pContext->SetPipelineState(PSO);
    pContext->CommitShaderResources(SRB, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const Uint32           Groups = (m_Capacity + RTXPT_SHARC_GROUP_SIZE - 1) / RTXPT_SHARC_GROUP_SIZE;
    DispatchComputeAttribs Dispatch{std::min(Groups, Uint32{RTXPT_SHARC_GROUPS_PER_ROW}),
                                    (Groups + RTXPT_SHARC_GROUPS_PER_ROW - 1) / RTXPT_SHARC_GROUPS_PER_ROW, 1};
    pContext->DispatchCompute(Dispatch);
    Barrier(pContext);
    return true;
}
bool SharcIntegration::Resolve(IDeviceContext* pContext) { return DispatchCompute(pContext, false); }
void SharcIntegration::Bind(RTXPTRayTracingDispatch& Dispatch) const
{
    Dispatch.pSharcConstants    = m_Constants;
    Dispatch.pSharcHash         = m_Hash->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
    Dispatch.pSharcAccumulation = m_Accumulation->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
    Dispatch.pSharcResolved     = m_Resolved->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
    Dispatch.pSharcPathState    = m_PathState->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
}
} // namespace Diligent
