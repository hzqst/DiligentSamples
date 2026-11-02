// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cmath>

#include "../../../../DiligentCore/Graphics/SuperResolution/interface/SuperResolutionFactory.h"
#include "RTXPTFrameConstants.hpp"
#include "RTXPTRenderTargets.hpp"
#include "RTXPTSuperResolutionInputPass.hpp"

namespace Diligent
{

// Stateless description/parameter adapter of the realtime super resolution inputs. Factory ownership, the feature,
// its jitter sequence and history belong to the view that executes it.
//The upscaler takes linear HDR without auto exposure or sharpening: the converted inputs of
//RTXPTSuperResolutionInputPass, exposure 1, and no pre-exposure, so display exposure stays with tone mapping.
struct RTXPTSuperResolutionInputs
{
    //Desc is the upscaler of the queried output and quality at the input size the query returned. Fails on a query
    //result outside the provider's own range, so an unsupported size is never created.
    static bool MakeDesc(const SuperResolutionSourceSettingsAttribs& Query,
                         const SuperResolutionSourceSettings&        Source,
                         SuperResolutionDesc&                        Desc)
    {
        const RTXPTRenderTargetFormats Formats;
        Desc = {};
        if (Query.OutputWidth == 0 || Query.OutputHeight == 0 ||
            Query.OptimizationType >= SUPER_RESOLUTION_OPTIMIZATION_TYPE_COUNT ||
            Query.Flags != SUPER_RESOLUTION_FLAG_NONE || Query.OutputFormat != Formats.ProcessedOutputColor ||
            Source.OptimalInputWidth == 0 || Source.OptimalInputHeight == 0 ||
            Source.OptimalInputWidth > Query.OutputWidth || Source.OptimalInputHeight > Query.OutputHeight ||
            Source.OptimalInputWidth < Source.MinInputWidth || Source.OptimalInputWidth > Source.MaxInputWidth ||
            Source.OptimalInputHeight < Source.MinInputHeight || Source.OptimalInputHeight > Source.MaxInputHeight)
            return false;

        Desc.Name           = "RTXPT realtime super resolution";
        Desc.VariantId      = Query.VariantId;
        Desc.InputWidth     = Source.OptimalInputWidth;
        Desc.InputHeight    = Source.OptimalInputHeight;
        Desc.OutputWidth    = Query.OutputWidth;
        Desc.OutputHeight   = Query.OutputHeight;
        Desc.OutputFormat   = Formats.ProcessedOutputColor;
        Desc.ColorFormat    = Formats.OutputColor;
        Desc.DepthFormat    = RTXPTSuperResolutionInputPass::DepthFormat;
        Desc.MotionFormat   = RTXPTSuperResolutionInputPass::MotionVectorsFormat;
        Desc.ExposureFormat = RTXPTSuperResolutionInputPass::ExposureFormat;
        Desc.Flags          = SUPER_RESOLUTION_FLAG_NONE;
        return true;
    }

    //Every guide, layer and denoiser target is at input size; the upscaled HDR and display targets at output size.
    static RTXPTRenderTargetDimensions MakeDimensions(const SuperResolutionDesc& Desc)
    {
        return {Desc.InputWidth, Desc.InputHeight, Desc.OutputWidth, Desc.OutputHeight,
                Desc.InputWidth != Desc.OutputWidth || Desc.InputHeight != Desc.OutputHeight};
    }

    //Injects the upscaler's jitter (render pixels, as GetJitterOffset returns it) into the camera exactly once. The
    //camera samples at the pixel center minus the jitter, the convention of a jittered projection. The matrices stay
    //unjittered for motion and depth; the field of view is unchanged.
    static void ApplyJitter(const float2& Jitter, PathTracerCameraData& Camera, PathTracerViewData& View)
    {
        View.PixelOffset = Jitter;
        Camera.Jitter    = Jitter * float2{1, -1};
    }

    //Fills the attributes of one Execute from the frame the inputs were prepared for. ColorSRV is the render-size
    //linear HDR, OutputUAV the separate output-size HDR target. Fails, clearing Attribs, unless the frame was traced
    //at input size with the jitter of ApplyJitter alone and the inputs were prepared at that size.
    static bool MakeExecuteAttribs(IDeviceContext*                      Context,
                                   const SuperResolutionDesc&           Desc,
                                   const RTXPTSuperResolutionInputPass& Inputs,
                                   ITextureView*                        ColorSRV,
                                   ITextureView*                        OutputUAV,
                                   const SampleConstants&               Frame,
                                   bool                                 ResetHistory,
                                   float                                TimeDeltaSeconds,
                                   ExecuteSuperResolutionAttribs&       Attribs)
    {
        Attribs            = {};
        const auto& Camera = Frame.ptConsts.camera;
        const auto& View   = Frame.view;
        const float Width = static_cast<float>(Desc.InputWidth), Height = static_cast<float>(Desc.InputHeight);
        if (Context == nullptr || Desc.Flags != SUPER_RESOLUTION_FLAG_NONE ||
            !IsView(ColorSRV, TEXTURE_VIEW_SHADER_RESOURCE, Desc.ColorFormat, Desc.InputWidth, Desc.InputHeight) ||
            !IsView(OutputUAV, TEXTURE_VIEW_UNORDERED_ACCESS, Desc.OutputFormat, Desc.OutputWidth, Desc.OutputHeight) ||
            ColorSRV->GetTexture() == OutputUAV->GetTexture() ||
            Inputs.GetWidth() != Desc.InputWidth || Inputs.GetHeight() != Desc.InputHeight || !Inputs.GetExposureSRV() ||
            Frame.ptConsts.imageWidth != Desc.InputWidth || Frame.ptConsts.imageHeight != Desc.InputHeight ||
            Camera.ViewportWidth != Desc.InputWidth || Camera.ViewportHeight != Desc.InputHeight ||
            View.ViewportSize != float2{Width, Height} || View.ClipToWindowScale != float2{Width * 0.5f, -Height * 0.5f} ||
            !std::isfinite(View.PixelOffset.x) || !std::isfinite(View.PixelOffset.y) ||
            std::abs(View.PixelOffset.x) > 0.5f || std::abs(View.PixelOffset.y) > 0.5f ||
            Camera.Jitter != View.PixelOffset * float2{1, -1} || Frame.ptConsts.perPixelJitterAAScale != 0 ||
            !std::isfinite(Camera.NearZ) || !std::isfinite(Camera.FarZ) || !(Camera.NearZ > 0) || !(Camera.FarZ > Camera.NearZ) ||
            !(length(Camera.CameraW) > 0) || !std::isfinite(TimeDeltaSeconds))
            return false;
        for (size_t Index = 0; Index < 16; ++Index)
            if (!std::isfinite(View.MatWorldToClipNoOffset.Data()[Index]))
                return false;

        Attribs.pContext                     = Context;
        Attribs.pColorTextureSRV             = ColorSRV;
        Attribs.pOutputTextureView           = OutputUAV;
        Attribs.pDepthTextureSRV             = Inputs.GetDepthSRV();
        Attribs.pMotionVectorsSRV            = Inputs.GetMotionVectorsSRV();
        Attribs.pExposureTextureSRV          = Inputs.GetExposureSRV();
        Attribs.pIgnoreHistoryMaskTextureSRV = Inputs.GetIgnoreHistoryMaskSRV();
        Attribs.StateTransitionMode          = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Attribs.JitterX                      = View.PixelOffset.x;
        Attribs.JitterY                      = View.PixelOffset.y;
        Attribs.PreExposure                  = 1;
        Attribs.MotionVectorScaleX           = 1;
        Attribs.MotionVectorScaleY           = 1;
        Attribs.ExposureScale                = 1;
        Attribs.Sharpness                    = 0;
        Attribs.CameraNear                   = Camera.NearZ;
        Attribs.CameraFar                    = Camera.FarZ;
        Attribs.CameraFovAngleVert           = 2.0f * std::atan(length(Camera.CameraV) / length(Camera.CameraW));
        Attribs.TimeDeltaInSeconds           = TimeDeltaSeconds;
        Attribs.ResetHistory                 = ResetHistory;
        return true;
    }

private:
    static bool IsView(ITextureView* View, TEXTURE_VIEW_TYPE Type, TEXTURE_FORMAT Format, Uint32 Width, Uint32 Height)
    {
        if (View == nullptr || View->GetDesc().ViewType != Type || View->GetDesc().Format != Format)
            return false;
        const TextureDesc& Desc = View->GetTexture()->GetDesc();
        return Desc.Width == Width && Desc.Height == Height;
    }
};

} // namespace Diligent
