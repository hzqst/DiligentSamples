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

#include "BasicMath.hpp"
#include "RTXPTRealtimeSettings.hpp"
#include <cmath>

#if RTXPT_HAS_NRD

#    include <string>

#    include "NRD.h"
#    include "NRDSettings.h"

#endif

namespace Diligent
{

// The renderer uses seconds; NRD uses milliseconds and selects internal timing for nonpositive values.
inline float RTXPTNrdFrameTimeMilliseconds(float Seconds)
{
    constexpr float kMillisecondsPerSecond = 1000.0f;
    if (!std::isfinite(Seconds) || Seconds <= 0.0f)
        return 0.0f;
    const float Milliseconds = Seconds * kMillisecondsPerSecond;
    return std::isfinite(Milliseconds) ? Milliseconds : 0.0f;
}

const char* RTXPTGetNrdUnavailableReason();
bool        RTXPTIsNrdAvailable();

#if RTXPT_HAS_NRD

nrd::Denoiser       RTXPTToNrdDenoiser(RTXPTNrdMethod Method);
nrd::RelaxSettings  RTXPTMakeRelaxSettings(const RTXPTNrdRelaxUiSettings& Ui);
nrd::ReblurSettings RTXPTMakeReblurSettings(const RTXPTNrdReblurUiSettings& Ui);
// SunDirection points from the surfaces to the sun.
nrd::SigmaSettings RTXPTMakeSigmaSettings(const RTXPTNrdSigmaUiSettings& Ui, const float3& SunDirection);

// Appends the default search directories of the NRD shaders and headers. They are used only without a host
// shader source (see RTXPTShaderSource.hpp): first shaders/NRD, where rtxpt_stage_shaders() deploys them,
// then the source-tree directories a host may define for running from its source directory.
void RTXPTAppendNrdShaderSearchDirs(std::string& SearchDirs);

#endif

} // namespace Diligent
