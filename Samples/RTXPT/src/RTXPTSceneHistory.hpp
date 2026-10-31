// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once

#include <algorithm>
#include <atomic>
#include "RTXPTSceneRuntime.hpp"

namespace Diligent
{
// Identifies successfully recorded scene preparation on one ordered GPU queue, not GPU completion.
// Snapshot/record-frame IDs are deliberately independent. A residency reset changes Epoch.
struct RTXPTPreparedSceneToken
{
    Uint64 Epoch             = 0;
    Uint64 Execution         = 0;
    Uint64 PreviousExecution = 0;
    bool   IsValid() const { return Epoch != 0 && Execution != 0; }
    bool   IsPreviousOf(const RTXPTPreparedSceneToken& Next) const
    {
        return IsValid() && Next.IsValid() && Epoch == Next.Epoch && Execution == Next.PreviousExecution;
    }
};

// CPU correspondence for immutable geometry/layout. Publish only after all Prepare work succeeds.
// A failed attempt invalidates the public token but cannot overwrite the last successful baseline.
// Geometry ownership prevents pointer/asset reuse; current compact TLAS rows never identify past instances.
class RTXPTSceneHistory
{
public:
    RTXPTSceneHistory() { Reset(); }
    RTXPTSceneHistory(const RTXPTSceneHistory&) = delete;
    RTXPTSceneHistory& operator=(const RTXPTSceneHistory&) = delete;
    void               Reset()
    {
        m_Epoch     = s_NextEpoch.fetch_add(1);
        m_Execution = 0;
        m_Token     = {};
        m_Previous.reset();
    }
    void                    BeginPrepare() { m_Token = {}; }
    RTXPTPreparedSceneToken GetToken() const { return m_Token; }
    RTXPTPreparedSceneToken Publish(const RTXPTSceneSnapshotPtr& Snapshot)
    {
        if (!Snapshot) return {};
        m_Token = {m_Epoch, m_Execution + 1, m_Execution};
        ++m_Execution;
        m_Previous = Snapshot;
        return m_Token;
    }
    const RTXPTSceneInstanceState* FindPreviousRigid(const RTXPTSceneInstanceState& Current) const
    {
        return Current.Geometry && !Current.Geometry->IsSkinned() ? FindPreviousSurface(Current) : nullptr;
    }
    // Skinned correspondence additionally requires the builder's published deformed vertex range.
    const RTXPTSceneInstanceState* FindPreviousSurface(const RTXPTSceneInstanceState& Current) const
    {
        if (!m_Previous || !Current.Enabled || !Current.Geometry ||
            Current.Continuity != RTXPTInstanceContinuity::Continuous)
            return nullptr;
        const auto& Instances = m_Previous->Instances;
        const auto  Found     = std::lower_bound(Instances.begin(), Instances.end(), Current.Handle.Slot,
                                            [](const auto& Instance, Uint32 Slot) { return Instance->Handle.Slot < Slot; });
        if (Found == Instances.end()) return nullptr;
        const auto& Previous = **Found;
        if (!(Previous.Handle == Current.Handle) || !Previous.Enabled || Previous.Geometry != Current.Geometry ||
            Previous.MeshIndex != Current.MeshIndex || Previous.HistoryRevision != Current.HistoryRevision ||
            Previous.Continuity == RTXPTInstanceContinuity::Untracked)
            return nullptr;
        // A changed masked/filtered binding may have made this surface absent. Opaque shading and light changes do
        // not destroy geometric correspondence; visibility-changing bindings conservatively reject surface history.
        if (Previous.Materials.size() != Current.Materials.size()) return nullptr;
        for (size_t I = 0; I < Current.Materials.size(); ++I)
            if (Previous.Materials[I] != Current.Materials[I] &&
                (Previous.Materials[I]->NeedsAnyHit() || Current.Materials[I]->NeedsAnyHit()))
                return nullptr;
        return &Previous;
    }

private:
    static inline std::atomic<Uint64> s_NextEpoch{1};
    Uint64                            m_Epoch     = 0;
    Uint64                            m_Execution = 0;
    RTXPTPreparedSceneToken           m_Token;
    RTXPTSceneSnapshotPtr             m_Previous;
};
} // namespace Diligent
