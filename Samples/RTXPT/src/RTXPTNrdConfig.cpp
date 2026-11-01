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

#include "RTXPTNrdConfig.hpp"

#include "DebugUtilities.hpp"

namespace Diligent
{

const char* RTXPTGetNrdUnavailableReason()
{
#if RTXPT_HAS_NRD
    return "Standalone NRD is available.";
#else
    return "Standalone denoiser disabled: NRD/NRI/ShaderMake/MathLib submodules are missing or DILIGENT_*_DIR variables were cleared.";
#endif
}

bool RTXPTIsNrdAvailable()
{
#if RTXPT_HAS_NRD
    return true;
#else
    return false;
#endif
}

#if RTXPT_HAS_NRD

namespace
{

nrd::HitDistanceReconstructionMode ToNrdHitDistanceMode(RTXPTNrdHitDistanceReconstructionMode Mode)
{
    switch (Mode)
    {
        case RTXPTNrdHitDistanceReconstructionMode::Off:
            return nrd::HitDistanceReconstructionMode::OFF;
        case RTXPTNrdHitDistanceReconstructionMode::Area3x3:
            return nrd::HitDistanceReconstructionMode::AREA_3X3;
        case RTXPTNrdHitDistanceReconstructionMode::Area5x5:
            return nrd::HitDistanceReconstructionMode::AREA_5X5;
        default:
            return nrd::HitDistanceReconstructionMode::OFF;
    }
}

void AppendShaderSearchDir(std::string& SearchDirs, const char* Path)
{
    if (Path == nullptr || Path[0] == '\0')
        return;

    if (!SearchDirs.empty())
        SearchDirs += ';';
    SearchDirs += Path;
}

} // namespace

void RTXPTAppendNrdShaderSearchDirs(std::string& SearchDirs)
{
    AppendShaderSearchDir(SearchDirs, "shaders/NRD");
#    ifdef RTXPT_NRD_SHADER_SOURCE_DIR
    AppendShaderSearchDir(SearchDirs, RTXPT_NRD_SHADER_SOURCE_DIR);
#    endif
#    ifdef RTXPT_NRD_SHADER_INCLUDE_DIR
    AppendShaderSearchDir(SearchDirs, RTXPT_NRD_SHADER_INCLUDE_DIR);
#    endif
#    ifdef RTXPT_NRD_SHADER_CONFIG_DIR
    AppendShaderSearchDir(SearchDirs, RTXPT_NRD_SHADER_CONFIG_DIR);
#    endif
#    ifdef RTXPT_NRD_MATHLIB_INCLUDE_DIR
    AppendShaderSearchDir(SearchDirs, RTXPT_NRD_MATHLIB_INCLUDE_DIR);
#    endif
}

nrd::Denoiser RTXPTToNrdDenoiser(RTXPTNrdMethod Method)
{
    switch (Method)
    {
        case RTXPTNrdMethod::REBLUR:
            return nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
        case RTXPTNrdMethod::RELAX:
            return nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
        default:
            UNEXPECTED("Unknown RTXPT NRD method");
            return nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
    }
}

nrd::RelaxSettings RTXPTMakeRelaxSettings(const RTXPTNrdRelaxUiSettings& Ui)
{
    nrd::RelaxSettings Settings;
    Settings.enableAntiFirefly                  = Ui.EnableAntiFirefly;
    Settings.hitDistanceReconstructionMode      = ToNrdHitDistanceMode(Ui.HitDistanceReconstructionMode);
    Settings.diffusePrepassBlurRadius           = Ui.DiffusePrepassBlurRadius;
    Settings.specularPrepassBlurRadius          = Ui.SpecularPrepassBlurRadius;
    Settings.diffuseMaxAccumulatedFrameNum      = Ui.DiffuseMaxAccumulatedFrameNum;
    Settings.specularMaxAccumulatedFrameNum     = Ui.SpecularMaxAccumulatedFrameNum;
    Settings.diffuseMaxFastAccumulatedFrameNum  = Ui.DiffuseMaxFastAccumulatedFrameNum;
    Settings.specularMaxFastAccumulatedFrameNum = Ui.SpecularMaxFastAccumulatedFrameNum;
    Settings.historyFixFrameNum                 = Ui.HistoryFixFrameNum;
    Settings.atrousIterationNum                 = Ui.AtrousIterationNum;
    Settings.lobeAngleFraction                  = Ui.LobeAngleFraction;
    Settings.specularLobeAngleSlack             = Ui.SpecularLobeAngleSlack;
    Settings.depthThreshold                     = Ui.DepthThreshold;
    Settings.antilagSettings.accelerationAmount = Ui.AntilagAccelerationAmount;
    Settings.antilagSettings.spatialSigmaScale  = Ui.AntilagSpatialSigmaScale;
    Settings.antilagSettings.temporalSigmaScale = Ui.AntilagTemporalSigmaScale;
    Settings.antilagSettings.resetAmount        = Ui.AntilagResetAmount;
    return Settings;
}

nrd::ReblurSettings RTXPTMakeReblurSettings(const RTXPTNrdReblurUiSettings& Ui)
{
    nrd::ReblurSettings Settings;
    Settings.enableAntiFirefly             = Ui.EnableAntiFirefly;
    Settings.hitDistanceReconstructionMode = ToNrdHitDistanceMode(Ui.HitDistanceReconstructionMode);
    Settings.maxAccumulatedFrameNum        = Ui.MaxAccumulatedFrameNum;
    Settings.maxFastAccumulatedFrameNum    = Ui.MaxFastAccumulatedFrameNum;
    Settings.historyFixFrameNum            = Ui.HistoryFixFrameNum;
    Settings.diffusePrepassBlurRadius      = Ui.DiffusePrepassBlurRadius;
    Settings.specularPrepassBlurRadius     = Ui.SpecularPrepassBlurRadius;
    return Settings;
}

#endif

} // namespace Diligent
