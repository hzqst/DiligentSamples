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

// This hash is for debug coloring/validation only.
// History matching must continue to compare the complete source key.
inline Uint32 GetLightDebugID(const RTXPTLightSourceRange& Source, Uint32 PrimitiveIndex = 0)
{
    auto Combine = [](Uint32 Seed, Uint32 Value) {
        Value ^= Value >> 16u;
        Value *= 0x21f0aaadu;
        Value ^= Value >> 15u;
        Value *= 0xf35a2d97u;
        Value ^= Value >> 15u;
        return Seed ^ (Value + 0x9e3779b9u + (Seed << 6u) + (Seed >> 2u));
    };
    Uint32 Hash = 0;
    for (Uint64 Part : Source.Key)
    {
        Hash = Combine(Hash, static_cast<Uint32>(Part >> 32u));
        Hash = Combine(Hash, static_cast<Uint32>(Part));
    }
    return Combine(Hash, PrimitiveIndex);
}

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
