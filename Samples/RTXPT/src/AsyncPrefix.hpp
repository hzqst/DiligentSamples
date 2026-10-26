// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once

#include "DeviceContext.h"
#include "Fence.h"
#include "RenderDevice.h"
#include "RefCntAutoPtr.hpp"
#include "EngineFactory.h"
#include <vector>

namespace Diligent
{

// Context 0 presents; context 1 runs the path-tracing prefix. The device masks
// off unavailable contexts, preserving the single-queue fallback.
constexpr Uint64 kRTXPTImmediateContextMask = 3;

template <typename ResourceType>
inline void ReleasePrefixResource(IDeviceContext* Context, ResourceType* Resource)
{
    if (!Resource || Resource->GetState() == RESOURCE_STATE_UNKNOWN || Resource->GetState() == RESOURCE_STATE_UNDEFINED) return;
    StateTransitionDesc Barrier{Resource, RESOURCE_STATE_UNKNOWN, RESOURCE_STATE_COMMON, STATE_TRANSITION_FLAG_UPDATE_STATE};
    Context->TransitionResourceStates(1, &Barrier);
}

class AsyncPrefix
{
public:
    static bool Configure(IEngineFactory* Factory, EngineCreateInfo& CI, std::vector<ImmediateContextCreateInfo>& Contexts, bool SameQueueFamily = false)
    {
        Uint32 Count = 0;
        Factory->EnumerateAdapters(CI.GraphicsAPIVersion, Count, nullptr);
        std::vector<GraphicsAdapterInfo> Adapters(Count);
        if (!Count) return false;
        Factory->EnumerateAdapters(CI.GraphicsAPIVersion, Count, Adapters.data());
        const Uint32 Index    = CI.AdapterId < Count ? CI.AdapterId : 0;
        const auto&  Adapter  = Adapters[Index];
        int          Graphics = -1, Compute = -1;
        for (Uint32 I = 0; I < Adapter.NumQueues; ++I)
        {
            const auto& Queue = Adapter.Queues[I];
            if (!Queue.MaxDeviceContexts) continue;
            const auto Type = Queue.QueueType & COMMAND_QUEUE_TYPE_PRIMARY_MASK;
            if (Type == COMMAND_QUEUE_TYPE_GRAPHICS && Graphics < 0) Graphics = static_cast<int>(I);
            if (Type == COMMAND_QUEUE_TYPE_COMPUTE && Compute < 0) Compute = static_cast<int>(I);
        }
        // Vulkan dynamic uniform/storage uploads use the shared dynamic heap.
        // Keep both contexts in one family; a second queue still permits overlap.
        if (SameQueueFamily && Graphics >= 0)
            Compute = Adapter.Queues[Graphics].MaxDeviceContexts >= 2 ? Graphics : -1;
        if (Graphics < 0 || Compute < 0) return false;
        Contexts.resize(2);
        Contexts[0].Name         = "RTXPT Graphics";
        Contexts[0].QueueId      = static_cast<Uint8>(Graphics);
        Contexts[1].Name         = "RTXPT AsyncPrefix";
        Contexts[1].QueueId      = static_cast<Uint8>(Compute);
        CI.AdapterId             = Index;
        CI.pImmediateContextInfo = Contexts.data();
        CI.NumImmediateContexts  = 2;
        CI.Features.NativeFence  = DEVICE_FEATURE_STATE_OPTIONAL;
        return true;
    }

    void Initialize(IRenderDevice* Device, IDeviceContext* Graphics, IDeviceContext* Compute)
    {
        m_Graphics = Graphics;
        m_Compute  = Compute;
        if (!m_Compute) return;
        FenceDesc Desc;
        Desc.Type = FENCE_TYPE_GENERAL;
        Desc.Name = "RTXPT graphics completion";
        Device->CreateFence(Desc, &m_GraphicsFence);
        Desc.Name = "RTXPT prefix completion";
        Device->CreateFence(Desc, &m_ComputeFence);
    }

    bool            IsAvailable() const { return m_Compute && m_GraphicsFence && m_ComputeFence; }
    IDeviceContext* GetContext() const { return m_Compute; }

    void Begin()
    {
        // A serial frame or a resource rebuild may have populated prefix inputs.
        if (!m_Active)
        {
            SignalGraphics();
            m_Compute->DeviceWaitForFence(m_GraphicsFence, m_GraphicsValue);
        }
        m_Active = true;
        m_Compute->BeginDebugGroup("RTXPT AsyncPrefix early");
    }

    void JoinPreviousGraphics()
    {
        m_Compute->EndDebugGroup();
        // Submit early work before installing the wait: it can overlap RR N-1.
        m_Compute->Flush();
        m_Compute->DeviceWaitForFence(m_GraphicsFence, m_GraphicsValue);
        m_Compute->BeginDebugGroup("RTXPT AsyncPrefix late");
    }

    void Submit()
    {
        m_Compute->EndDebugGroup();
        m_Compute->EnqueueSignal(m_ComputeFence, ++m_ComputeValue);
        m_Compute->Flush();
        m_Graphics->DeviceWaitForFence(m_ComputeFence, m_ComputeValue);
    }

    void EndGraphics()
    {
        SignalGraphics();
        // The swap chain finishes only context 0. Retire context 1's upload
        // allocations/descriptors against its submitted fence each frame.
        m_Compute->FinishFrame();
    }

    void Drain()
    {
        if (!m_Active) return;
        m_Compute->Flush();
        m_Graphics->Flush();
        m_Compute->WaitForIdle();
        m_Graphics->WaitForIdle();
        m_Compute->InvalidateState();
        m_Graphics->InvalidateState();
        m_Active = false;
    }

    bool IsActive() const { return m_Active; }

private:
    void SignalGraphics()
    {
        m_Graphics->EnqueueSignal(m_GraphicsFence, ++m_GraphicsValue);
        m_Graphics->Flush();
    }
    RefCntAutoPtr<IDeviceContext> m_Graphics, m_Compute;
    RefCntAutoPtr<IFence>         m_GraphicsFence, m_ComputeFence;
    Uint64                        m_GraphicsValue = 0, m_ComputeValue = 0;
    bool                          m_Active = false;
};

} // namespace Diligent
