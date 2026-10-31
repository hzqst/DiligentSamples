// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once

#include "BasicTypes.h"

namespace Diligent
{
// Two ranges in one ordered-queue arena. A changed pose writes only the range that is not the last successful
// scene's surface. GPU reads from earlier frames precede subsequent writes/copies; no CPU completion wait is needed.
// PoseRevision is a skin+BLAS cache revision, deliberately separate from whole-scene history publication.
struct RTXPTSkinnedVertexHistory
{
    static constexpr Uint32 RangeCount     = 2;
    Uint32                  AllocationBase = 0;
    Uint32                  VertexCount    = 0;
    Uint32                  OutputBase     = 0;
    Uint64                  PoseRevision   = 0;

    RTXPTSkinnedVertexHistory() = default;
    RTXPTSkinnedVertexHistory(Uint32 Base, Uint32 Count) :
        AllocationBase{Base}, VertexCount{Count}, OutputBase{Base} {}

    bool PreparePose(Uint64 Revision)
    {
        if (PoseRevision == Revision)
            return false;
        if (HasPrevious() && OutputBase == m_PreviousBase)
            OutputBase = AllocationBase + (OutputBase == AllocationBase ? VertexCount : 0);
        PoseRevision = 0;
        return true;
    }
    // Call only when the containing Prepare has succeeded, after rows have captured GetPreviousBase().
    void Publish()
    {
        m_PreviousBase         = OutputBase;
        m_PreviousPoseRevision = PoseRevision;
    }
    void   InvalidatePrevious() { m_PreviousPoseRevision = 0; }
    bool   HasPrevious() const { return m_PreviousPoseRevision != 0; }
    bool   HasPreviousPose(Uint64 Revision) const { return HasPrevious() && Revision == m_PreviousPoseRevision; }
    Uint32 GetPreviousBase() const { return m_PreviousBase; }

private:
    Uint32 m_PreviousBase         = 0;
    Uint64 m_PreviousPoseRevision = 0;
};
} // namespace Diligent
