// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <array>
#include <map>
#include <vector>
#include "BasicTypes.h"

namespace Diligent
{

// Identity belongs to the source, never to its position, intensity or packed-table offset.
// A mesh primitive owns a contiguous range of triangle lights, as in upstream LightLink.
struct RTXPTLightSourceRange
{
    std::array<Uint64, 4> Key   = {};
    Uint32                Base  = 0;
    Uint32                Count = 0;
};

inline void BuildLightHistoryMaps(const std::vector<RTXPTLightSourceRange>& Current,
                                  const std::vector<RTXPTLightSourceRange>& Previous,
                                  Uint32                                    CurrentCount,
                                  Uint32                                    PreviousCount,
                                  std::vector<Uint32>&                      CurrentToPast,
                                  std::vector<Uint32>&                      PastToCurrent)
{
    constexpr Uint32 Invalid = ~0u;
    CurrentToPast.assign(CurrentCount, Invalid);
    PastToCurrent.assign(PreviousCount, Invalid);
    std::map<std::array<Uint64, 4>, const RTXPTLightSourceRange*> Sources;
    for (const auto& Range : Previous)
        Sources.emplace(Range.Key, &Range);
    for (const auto& Range : Current)
    {
        const auto It = Sources.find(Range.Key);
        if (It == Sources.end()) continue;
        const auto& Past = *It->second;
        // A changed triangle count invalidates the primitive rather than reusing a partial range.
        if (Past.Count != Range.Count) continue;
        for (Uint32 I = 0; I < Range.Count; ++I)
        {
            if (Range.Base + I >= CurrentCount || Past.Base + I >= PreviousCount) break;
            CurrentToPast[Range.Base + I] = Past.Base + I;
            PastToCurrent[Past.Base + I]  = Range.Base + I;
        }
    }
}

} // namespace Diligent
