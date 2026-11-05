// Copyright 2026 Diligent Graphics LLC. Licensed under Apache-2.0.
#pragma once

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <vector>
#include "RTXPTEmbeddedRenderer.hpp"
#include "RTXPTRealtimePipeline.hpp"
#include "RTXPTRealtimeSettings.hpp"
#include "GraphicsAccessories.hpp"
#include "RTXPTGpuProfiler.hpp"
#include "json.hpp"

namespace Diligent
{
//Explicit development readback. The caller supplies a NEW directory and may add host metadata there afterwards.
//Only a successful current realtime output can be captured. No manifest is published for an incomplete readback/write.
//Texture bytes are tightly packed, little endian, top row first, array slices in ascending order. No color conversion.
//The layer buffer retains the shader's packed representation; inactive records/padding are not valid surface guides.
//RealtimeDLSSRR exports the render-size inputs ray reconstruction received, its exposure texture and its display-size
//HDR; it has no denoised or upscaled HDR. A frame that applied Bloom also exports the display-size HDR after Bloom; the
//HDR of its mode stays the one Bloom read.
class RTXPTDebugCapture
{
public:
    static bool Capture(IRenderDevice* Device, IDeviceContext* Context, RTXPTEmbeddedView& View, const std::filesystem::path& Directory)
    {
        const bool bSR     = View.GetMode() == RTXPTViewMode::RealtimeDLSSSR;
        const bool bRR     = View.GetMode() == RTXPTViewMode::RealtimeDLSSRR;
        const bool bNative = View.GetMode() == RTXPTViewMode::RealtimeNative || bSR;
        const bool bBloom  = View.GetBloomStats().LastApplied;
        //The render-size HDR of the frame: RR's noisy input, the denoised layers of Native and SR, or Raw's noisy HDR.
        const auto  ColorOutput = bRR ? RTXPTViewOutput::RayReconstructionColor : bNative ? RTXPTViewOutput::DenoisedRadiance : RTXPTViewOutput::NoisyRadiance;
        const char* ColorName   = bRR ? "rr_color" : bNative ? "denoised_radiance" : "noisy_radiance";
        const auto  Color       = View.GetOutputInfo(ColorOutput);
        if (!Device || !Context || !Color.Valid || !View.GetRealtimeLayerBuffer())
            return false;
        try
        {
            if (!std::filesystem::create_directories(Directory))
                return false;
            std::ofstream Manifest{Directory / "manifest.json.pending"};
            const auto&   History     = View.GetHistoryInfo();
            const auto&   Stats       = View.GetRealtimeStats();
            const auto    DisplaySize = View.GetDisplaySize();
            const auto&   SR          = View.GetSuperResolutionInfo();
            const auto&   RR          = View.GetRayReconstructionInfo();
            const float2  Jitter      = bRR ? RR.Jitter : SR.Jitter;
            Manifest << std::setprecision(17)
                     << "{\n\"schema\":2,\"mode\":\"" << (bRR ? "RealtimeDLSSRR" : bSR ? "RealtimeDLSSSR" : bNative ? "RealtimeNative" : "RealtimeRaw")
                     << "\",\"frame_index\":" << Color.FrameIndex
                     << ",\"render_size\":[" << Color.Size.Width << ',' << Color.Size.Height
                     << "],\"display_size\":[" << DisplaySize.Width << ',' << DisplaySize.Height
                     << "],\"spp\":1,\"sample_base_index\":" << Color.FrameIndex - 1
                     << ",\"jitter\":[" << Jitter.x << ',' << Jitter.y << "],\"dynamic_motion_validated\":" << (Stats.DynamicMotionValidated ? "true" : "false")
                     << ",\"previous_surface_storage_bytes\":" << History.PreviousTransformStorageBytes + History.PreviousSkinnedRangeBytes
                     << ",\"previous_skinned_range_bytes\":" << History.PreviousSkinnedRangeBytes
                     << ",\"skinned_arena_storage_bytes\":" << History.SkinnedVertexStorageBytes
                     << ",\"prepared_epoch\":" << History.Scene.Epoch << ",\"prepared_execution\":" << History.Scene.Execution
                     << ",\"previous_prepared_execution\":" << History.Scene.PreviousExecution
                     << ",\"previous_view_execution\":" << History.PreviousViewScene.Execution
                     << ",\"camera_correspondence_valid\":" << (History.CameraCorrespondenceValid ? "true" : "false")
                     << ",\"light_feedback_reset\":" << (History.LightFeedbackReset ? "true" : "false")
                     << ",\"valid_previous_rigid_instances\":" << History.ValidPreviousRigidInstances
                     << ",\"valid_previous_skinned_instances\":" << History.ValidPreviousSkinnedInstances
                     << ",\"invalid_history_instances\":" << History.InvalidHistoryInstances
                     << ",\"denoiser\":" << (bNative ? "\"NRD_RELAX\"" : "null")
                     << ",\"denoiser_history_reset\":" << (bNative ? (Stats.LastHistoryReset ? "true" : "false") : "null")
                     << ",\"denoiser_history_confidence\":" << (bNative ? (Stats.HistoryConfidence ? "true" : "false") : "null")
                     << ",\"nrd_instances\":" << Stats.NrdInstances << ",\"nrd_pool_bytes\":" << Stats.NrdPoolBytes << ','
                     << "\"valid_correspondence_count\":null,\"total_gpu_memory_bytes\":null,"
                        "\"byte_order\":\"little\",\"row_order\":\"top_to_bottom\","
                        "\"primary_motion_convention\":\"current_to_previous; xy=render_local_pixels_without_jitter; z=previous_clip_w-current_clip_w; w=exported_surface_and_all_retained_paths_valid\","
                        "\"specular_motion_convention\":\"current_to_previous; xy=render_local_pixels_without_jitter; z=previous_clip_w-current_clip_w; w=one_reflected_path_with_valid_history\","
                        "\"virtual_motion_convention\":\"packed layer world-space current_to_previous\",";
            if (bSR)
            {
                const char* Quality = SR.Quality == RTXPTUpscaleQuality::Quality ? "Quality" :
                                                                                   SR.Quality == RTXPTUpscaleQuality::Balanced ? "Balanced" :
                                                                                                                                 "Performance";
                const nlohmann::json Metadata = {
                    {"provider", SR.Variant}, {"quality", Quality}, {"executes", SR.Executes}, {"resets", SR.Resets}, {"failures", SR.Failures}, {"last_reset", SR.LastReset}, {"depth_encoding", "forward_projection_0_1_far_1"}, {"motion_convention", "current_to_previous_render_pixels_without_jitter"}, {"exposure_policy", "explicit_1_preexposure_1_scale_1"}};
                Manifest << "\"super_resolution\":" << Metadata.dump() << ',';
            }
            if (bRR)
            {
                //The embedded realtime pipeline traces RR's camera jitter alone (D5), so there is no micro-jitter.
                const nlohmann::json Metadata = {
                    {"provider", RR.Variant}, {"quality", GetQualityName(RR.Quality)}, {"preset", RR.Preset}, {"executes", RR.Executes}, {"resets", RR.Resets}, {"failures", RR.Failures}, {"last_reset", RR.LastReset}, {"exposure", RR.Exposure}, {"view_forward_sign", RR.ForwardSign}, {"micro_jitter", 0}, {"depth_encoding", "forward_positive_linear_view_depth_sky_5000000"}, {"motion_convention", "current_to_previous_render_pixels_without_jitter"}, {"exposure_policy", "explicit_exposure_texture_preexposure_1_scale_1"}};
                Manifest << "\"ray_reconstruction\":" << Metadata.dump() << ',';
            }
            Manifest << "\"resources\":[\n";
            struct Output
            {
                RTXPTViewOutput Kind;
                const char*     Name;
                Uint32          Layer = 0;
            };
            std::vector<Output> Outputs = {
                {ColorOutput, ColorName},
                {RTXPTViewOutput::Display, "display"},
                {RTXPTViewOutput::RayDistance, "ray_distance_not_view_z"},
                {RTXPTViewOutput::PrimaryMotion, "primary_motion"},
                {RTXPTViewOutput::SpecularMotion, "specular_motion"},
                {RTXPTViewOutput::SpecularHitDistance, "specular_hit_distance"},
                {RTXPTViewOutput::AverageLayerRadiance, "average_layer_radiance"},
                {RTXPTViewOutput::LayerHeader, "layer_header"}};
            static constexpr const char* ConfidenceNames[kRTXPTDenoisingLayerCount] = {"history_confidence_layer0", "history_confidence_layer1", "history_confidence_layer2"};
            if (bNative)
            {
                for (Uint32 Layer = 0; Layer < kRTXPTDenoisingLayerCount; ++Layer)
                    Outputs.push_back({RTXPTViewOutput::LayerHistoryConfidence, ConfidenceNames[Layer], Layer});
            }
            if (bSR)
            {
                Outputs.push_back({RTXPTViewOutput::UpscaledRadiance, "upscaled_radiance"});
                Outputs.push_back({RTXPTViewOutput::SuperResolutionDepth, "sr_depth"});
                Outputs.push_back({RTXPTViewOutput::SuperResolutionMotion, "sr_motion"});
                Outputs.push_back({RTXPTViewOutput::SuperResolutionIgnoreHistoryMask, "sr_ignore_history"});
            }
            if (bRR)
            {
                Outputs.push_back({RTXPTViewOutput::ReconstructedRadiance, "reconstructed_radiance"});
                Outputs.push_back({RTXPTViewOutput::RayReconstructionDepth, "rr_depth"});
                Outputs.push_back({RTXPTViewOutput::RayReconstructionMotion, "rr_motion"});
                Outputs.push_back({RTXPTViewOutput::RayReconstructionSpecularMotion, "rr_specular_motion"});
                Outputs.push_back({RTXPTViewOutput::RayReconstructionNormalRoughness, "rr_normal_roughness"});
                Outputs.push_back({RTXPTViewOutput::RayReconstructionDiffuseAlbedo, "rr_diffuse_albedo"});
                Outputs.push_back({RTXPTViewOutput::RayReconstructionSpecularAlbedo, "rr_specular_albedo"});
                Outputs.push_back({RTXPTViewOutput::RayReconstructionExposure, "rr_exposure"});
            }
            if (bBloom)
                Outputs.push_back({RTXPTViewOutput::BloomRadiance, "bloom_radiance"});
            bool First = true;
            for (const auto& Output : Outputs)
            {
                const auto Info = View.GetOutputInfo(Output.Kind, Output.Layer);
                if (!Info.Valid || Info.FrameIndex != Color.FrameIndex)
                    return false;
                const auto&  Desc     = Info.Texture->GetDesc();
                const auto&  Format   = GetTextureFormatAttribs(Desc.Format);
                const Uint64 RowBytes = Uint64{Desc.Width} * Format.ComponentSize * Format.NumComponents;
                if (!WriteTexture(Device, Context, Info.Texture, Directory / (std::string{Output.Name} + ".bin"), RowBytes))
                    return false;
                if (!First) Manifest << ",\n";
                First = false;
                Manifest << "{\"name\":\"" << Output.Name << "\",\"file\":\"" << Output.Name
                         << ".bin\",\"format\":\"" << Format.Name << "\",\"width\":" << Desc.Width
                         << ",\"height\":" << Desc.Height << ",\"slices\":" << Desc.ArraySize
                         << ",\"row_bytes\":" << RowBytes << ",\"bytes\":" << RowBytes * Desc.Height * Desc.ArraySize
                         << ",\"encoding\":\"" << (Info.Encoding == RTXPTOutputEncoding::SRGB ? "sRGB" : "linear_or_packed")
                         << "\",\"frame_index\":" << Info.FrameIndex << ",\"valid\":true}";
            }
            auto* Buffer = View.GetRealtimeLayerBuffer();
            if (!WriteBuffer(Device, Context, Buffer, Directory / "layers.bin"))
                return false;
            Manifest << "],\n\"layers\":{\"file\":\"layers.bin\",\"bytes\":" << Buffer->GetDesc().Size
                     << ",\"record_bytes\":" << Buffer->GetDesc().ElementByteStride << ",\"count\":" << kRTXPTDenoisingLayerCount
                     << ",\"line_stride\":" << ComputeRTXPTLayerLineStride(Color.Size.Width)
                     << ",\"plane_stride\":" << ComputeRTXPTLayerPlaneStride(Color.Size.Width, Color.Size.Height)
                     << ",\"layout\":\"shaders/PathTracer/DenoisingLayers.hlsli:DenoisingLayer\","
                        "\"validity\":\"layer header and VertexIndexAndRoughness high 16 bits; PreviousVirtualPosition.w is path history validity; inactive/padding undefined\"},\n";
            View.GetProfiler().Poll();
            WriteTiming(Manifest, View.GetProfiler());
            Manifest << ",\"unavailable_stages\":[" << (bNative ? "" : "\"NRD\",") << (bSR ? "" : "\"SR\",") << "\"TAA\"" << (bRR ? "" : ",\"RR\"") << (bBloom ? "" : ",\"Bloom\"") << "]\n}\n";
            Manifest.flush();
            if (!Manifest) return false;
            Manifest.close();
            std::filesystem::rename(Directory / "manifest.json.pending", Directory / "manifest.json");
            return true;
        }
        catch (const std::exception& Error)
        {
            LOG_ERROR_MESSAGE("RTXPT capture failed: ", Error.what());
            return false;
        }
    }

    static void WriteTiming(std::ostream& Stream, const RTXPTGpuProfiler& Profiler)
    {
        const auto& Result = Profiler.GetLatest();
        Stream << "\"gpu_timing\":{\"available\":" << (Profiler.IsAvailable() ? "true" : "false")
               << ",\"source_frame\":" << Result.SourceFrame << ",\"dropped\":" << Profiler.GetDroppedCount() << ",\"milliseconds\":{";
        for (size_t I = 0; I < RTXPTGpuProfiler::StageCount; ++I)
        {
            if (I) Stream << ',';
            Stream << '"' << RTXPTGpuProfiler::GetStageName(I) << "\":";
            if (Result.Milliseconds[I] < 0) Stream << "null";
            else
                Stream << Result.Milliseconds[I];
        }
        Stream << "}}";
    }

private:
    static const char* GetQualityName(RTXPTRayReconstructionQuality Quality)
    {
        switch (Quality)
        {
            case RTXPTRayReconstructionQuality::Native: return "Native";
            case RTXPTRayReconstructionQuality::Quality: return "Quality";
            case RTXPTRayReconstructionQuality::Balanced: return "Balanced";
            case RTXPTRayReconstructionQuality::Performance: return "Performance";
        }
        return "unknown";
    }

    static bool WriteTexture(IRenderDevice* Device, IDeviceContext* Context, ITexture* Texture, const std::filesystem::path& Path, Uint64 RowBytes)
    {
        auto Desc = Texture->GetDesc();
        if (!RowBytes || Desc.MipLevels != 1 || Desc.SampleCount != 1) return false;
        Desc.Name           = "RTXPT explicit debug capture";
        Desc.Usage          = USAGE_STAGING;
        Desc.BindFlags      = BIND_NONE;
        Desc.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<ITexture> Staging;
        Device->CreateTexture(Desc, nullptr, &Staging);
        if (!Staging) return false;
        for (Uint32 Slice = 0; Slice < Desc.ArraySize; ++Slice)
        {
            CopyTextureAttribs Copy;
            Copy.pSrcTexture = Texture;
            Copy.pDstTexture = Staging;
            Copy.SrcSlice = Copy.DstSlice = Slice;
            Copy.SrcTextureTransitionMode = Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            Context->CopyTexture(Copy);
        }
        Context->Flush();
        Context->WaitForIdle();
        std::ofstream File{Path, std::ios::binary};
        for (Uint32 Slice = 0; Slice < Desc.ArraySize; ++Slice)
        {
            MappedTextureSubresource Map;
            Context->MapTextureSubresource(Staging, 0, Slice, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Map);
            if (!Map.pData) return false;
            for (Uint32 Y = 0; Y < Desc.Height; ++Y)
                File.write(static_cast<const char*>(Map.pData) + Y * Map.Stride, static_cast<std::streamsize>(RowBytes));
            Context->UnmapTextureSubresource(Staging, 0, Slice);
        }
        File.flush();
        return bool(File);
    }

    static bool WriteBuffer(IRenderDevice* Device, IDeviceContext* Context, IBuffer* Buffer, const std::filesystem::path& Path)
    {
        BufferDesc Desc;
        Desc.Name           = "RTXPT layer debug readback";
        Desc.Size           = Buffer->GetDesc().Size;
        Desc.Usage          = USAGE_STAGING;
        Desc.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<IBuffer> Staging;
        Device->CreateBuffer(Desc, nullptr, &Staging);
        if (!Staging) return false;
        Context->CopyBuffer(Buffer, 0, RESOURCE_STATE_TRANSITION_MODE_TRANSITION, Staging, 0, Desc.Size, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        Context->Flush();
        Context->WaitForIdle();
        void* Data = nullptr;
        Context->MapBuffer(Staging, MAP_READ, MAP_FLAG_DO_NOT_WAIT, Data);
        if (!Data) return false;
        std::ofstream File{Path, std::ios::binary};
        File.write(static_cast<const char*>(Data), static_cast<std::streamsize>(Desc.Size));
        Context->UnmapBuffer(Staging, MAP_READ);
        File.flush();
        return bool(File);
    }
};
} // namespace Diligent
