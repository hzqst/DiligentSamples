// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once

#include "HDRValidationCapture.hpp"
#include "../src/RTXPTFrameConstants.hpp"
#include "../src/RTXPTRenderTargets.hpp"
#include "../src/RTXPTToneMappingPass.hpp"
#include "json.hpp"

namespace Diligent
{

// An opt-in frozen Reference frame. Each file contains one GPU result: in particular, the
// accumulated radiance is not averaged a second time by the readback helper. The display file
// contains the stored pre-blit UNORM values, not linear radiance or a swap-chain screenshot.
class ReferenceValidationCapture
{
public:
    std::string Prefix;
    Uint32      Samples    = 256;
    Uint32      SeedOffset = 0;
    double      Time       = 0.0;

    bool Enabled() const { return !Prefix.empty(); }

    void CaptureHDR(IRenderDevice* Device, IDeviceContext* Context, const RTXPTRenderTargets& Targets, const SampleConstants& Constants)
    {
        if (!Enabled() || m_Attempted || Constants.ptConsts.sampleIndex != Samples)
            return;
        m_Attempted = true;
        CaptureOne(m_Noisy, Device, Context, Targets.GetOutputColorSRV(), ".noisy.pfm");
        CaptureOne(m_Accumulated, Device, Context, Targets.GetAccumulatedRadianceSRV(), ".accumulated.pfm");
    }

    void CaptureDisplay(IRenderDevice* Device, IDeviceContext* Context, const RTXPTRenderTargets& Targets, const SampleConstants& Constants, const RTXPTToneMappingParameters& ToneMapping, const std::string& Scene)
    {
        if (!Enabled() || m_Finished || !m_Attempted || Constants.ptConsts.sampleIndex != Samples)
            return;
        m_Finished        = true;
        m_Display.Display = true;
        CaptureOne(m_Display, Device, Context, Targets.GetPresentationSRV(), ".display.pfm");
        if (!m_Noisy.Succeeded() || !m_Accumulated.Succeeded() || !m_Display.Succeeded())
        {
            LOG_ERROR_MESSAGE("Reference capture incomplete: ", Prefix);
            return;
        }

        nlohmann::json Metadata;
        Metadata["scene"]              = Scene;
        Metadata["scene_time"]         = Time;
        Metadata["sample_index"]       = Constants.ptConsts.sampleIndex;
        Metadata["sample_base_index"]  = Constants.ptConsts.sampleBaseIndex;
        Metadata["seed_offset"]        = SeedOffset;
        Metadata["frame_index"]        = Constants.ptConsts.frameIndex;
        Metadata["reset_accumulation"] = Constants.ptConsts.resetAccumulation;
        Metadata["render_size"]        = {Targets.GetRenderWidth(), Targets.GetRenderHeight()};
        Metadata["display_size"]       = {Targets.GetDisplayWidth(), Targets.GetDisplayHeight()};
        Metadata["camera_position"]    = {Constants.cameraPositionAndTime.x, Constants.cameraPositionAndTime.y, Constants.cameraPositionAndTime.z};
        Metadata["world_to_clip"]      = nlohmann::json::array();
        for (Uint32 Row = 0; Row < 4; ++Row)
            for (Uint32 Column = 0; Column < 4; ++Column)
                Metadata["world_to_clip"].push_back(Constants.view.MatWorldToClipNoOffset[Row][Column]);
        Metadata["auto_exposure"]         = ToneMapping.AutoExposure;
        Metadata["exposure_compensation"] = ToneMapping.ExposureCompensation;
        Metadata["manual_exposure"]       = {ToneMapping.ExposureValue, ToneMapping.FilmSpeed, ToneMapping.FNumber, ToneMapping.Shutter};
        Metadata["tone_mapper"]           = static_cast<Uint32>(ToneMapping.ToneMapOperator);
        Metadata["bounce_count"]          = Constants.ptConsts.bounceCount;
        Metadata["diffuse_bounce_count"]  = Constants.ptConsts.diffuseBounceCount;
        Metadata["inverse_spp"]           = Constants.ptConsts.invSubSampleCount;
        Metadata["noisy"]                 = "current reference dispatch, scene-linear RGB";
        Metadata["accumulated"]           = "GPU progressive mean through sample_index, scene-linear RGB";
        Metadata["display"]               = "stored pre-blit RGBA8 UNORM RGB after bloom/tone mapping; no readback decoding";
        Metadata["capture_complete"]      = true;
        std::ofstream File{Prefix + ".json"};
        File << Metadata.dump(2) << '\n';
        File.flush();
        if (!File)
            LOG_ERROR_MESSAGE("Reference metadata write failed: ", Prefix);
        else
            LOG_INFO_MESSAGE("Reference capture complete: ", Prefix, " (", Samples, " samples)");
    }

private:
    void CaptureOne(HDRValidationCapture& Capture, IRenderDevice* Device, IDeviceContext* Context, ITextureView* View, const char* Suffix)
    {
        Capture.Path       = Prefix + Suffix;
        Capture.StartFrame = 0;
        Capture.FrameCount = 1;
        Capture.Capture(Device, Context, View ? View->GetTexture() : nullptr, 0);
    }

    bool                 m_Attempted = false;
    bool                 m_Finished  = false;
    HDRValidationCapture m_Noisy;
    HDRValidationCapture m_Accumulated;
    HDRValidationCapture m_Display;
};
} // namespace Diligent
