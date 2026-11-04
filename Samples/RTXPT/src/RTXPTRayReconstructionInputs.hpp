// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include <cmath>
#include <cstring>

#include "../../../../DiligentCore/Graphics/RayReconstruction/interface/RayReconstructionFactory.h"
#include "RTXPTFrameConstants.hpp"
#include "RTXPTRenderTargets.hpp"

namespace Diligent
{

// How RR learns the exposure of its scene-linear color input.
enum class RTXPTRayReconstructionExposure
{
    // NGX measures it (the Sample's advanced path).
    Auto,
    // The caller passes a 1x1 R16F/R32F exposure texture to every Execute; PreExposure and ExposureScale stay 1.
    Explicit,
};

// Resource/parameter adapter. Factory ownership, jitter sequence, history and
// Execute scheduling are handled by the RR pass, not by this stateless adapter.
struct RTXPTRayReconstructionInputs
{
    static bool MakeDesc(const RayReconstructionSourceSettingsAttribs& Query,
                         const RayReconstructionSourceSettings&        Source,
                         RayReconstructionDesc&                        Desc,
                         RTXPTRayReconstructionExposure                Exposure = RTXPTRayReconstructionExposure::Auto)
    {
        Desc = {};
        if (Query.VariantId != RayReconstructionVariant_DLSS ||
            Query.Quality >= RAY_RECONSTRUCTION_QUALITY_COUNT ||
            Query.OutputWidth == 0 || Query.OutputHeight == 0 ||
            Source.OptimalInputWidth == 0 || Source.OptimalInputHeight == 0 ||
            Source.OptimalInputWidth > Query.OutputWidth || Source.OptimalInputHeight > Query.OutputHeight ||
            Source.OptimalInputWidth < Source.MinInputWidth || Source.OptimalInputWidth > Source.MaxInputWidth ||
            Source.OptimalInputHeight < Source.MinInputHeight || Source.OptimalInputHeight > Source.MaxInputHeight ||
            (Query.Quality == RAY_RECONSTRUCTION_QUALITY_NATIVE &&
             (Source.OptimalInputWidth != Query.OutputWidth || Source.OptimalInputHeight != Query.OutputHeight)))
            return false;

        Desc.VariantId             = Query.VariantId;
        Desc.InputWidth            = Source.OptimalInputWidth;
        Desc.InputHeight           = Source.OptimalInputHeight;
        Desc.OutputWidth           = Query.OutputWidth;
        Desc.OutputHeight          = Query.OutputHeight;
        Desc.Quality               = Query.Quality;
        Desc.IsHDR                 = True;
        Desc.AutoExposure          = Exposure == RTXPTRayReconstructionExposure::Auto;
        Desc.LinearDepth           = True;
        Desc.DepthInverted         = False;
        Desc.MotionVectorsJittered = False;
        Desc.RoughnessPacked       = True;
        return true;
    }

    static RTXPTRenderTargetCreateInfo MakeRenderTargetCreateInfo(const RayReconstructionDesc& Desc)
    {
        RTXPTRenderTargetCreateInfo Info;
        Info.Dimensions                       = {Desc.InputWidth, Desc.InputHeight, Desc.OutputWidth, Desc.OutputHeight,
                           Desc.InputWidth != Desc.OutputWidth || Desc.InputHeight != Desc.OutputHeight};
        Info.CreateRealtimeResources          = true;
        Info.CreateRayReconstructionResources = true;
        return Info;
    }

    // RR reads positive linear depth along the view's forward axis. A perspective ViewToClip has w = +z for a
    // left-handed view (the Sample's) and w = -z for a right-handed one (the game's, forward = -Z). Returns false for
    // projections without a perspective w, which have no such axis. DLSSRRDenoiserPrepareInputs uses the same sign.
    static bool GetViewForwardSign(const float4x4& ViewToClip, float& Sign)
    {
        Sign = 0;
        if (ViewToClip.m03 != 0 || ViewToClip.m13 != 0 || ViewToClip.m23 == 0 || ViewToClip.m33 != 0)
            return false;
        Sign = ViewToClip.m23 < 0 ? -1.0f : 1.0f;
        return true;
    }

    // A full single-mip 1x1 R16F/R32F SRV, as RR reads an explicit exposure.
    static bool IsExposureView(ITextureView* pExposureSRV)
    {
        if (pExposureSRV == nullptr)
            return false;
        const auto& V = pExposureSRV->GetDesc();
        const auto& T = pExposureSRV->GetTexture()->GetDesc();
        return V.ViewType == TEXTURE_VIEW_SHADER_RESOURCE && T.Type == RESOURCE_DIM_TEX_2D && T.Width == 1 && T.Height == 1 &&
            T.MipLevels == 1 && T.SampleCount == 1 && (V.Format == TEX_FORMAT_R16_FLOAT || V.Format == TEX_FORMAT_R32_FLOAT);
    }

    // pExposureSRV is required by an explicit-exposure Desc and must be null for an auto-exposure one.
    static bool MakeExecuteAttribs(IDeviceContext*                  Context,
                                   const RTXPTRenderTargets&        Targets,
                                   const RayReconstructionDesc&     Desc,
                                   const SampleConstants&           Frame,
                                   bool                             ResetHistory,
                                   ITextureView*                    pExposureSRV,
                                   ExecuteRayReconstructionAttribs& Attribs)
    {
        // Also clear bindings on failure: never reuse optional guides or stale
        // resources from a previous frame/mode/size.
        Attribs                   = {};
        const bool bExposureValid = Desc.AutoExposure ? pExposureSRV == nullptr : IsExposureView(pExposureSRV);
        float      ForwardSign    = 0;
        if (Context == nullptr || !Targets.HasRayReconstructionRenderTargets() ||
            !(Targets.GetDimensions() == MakeRenderTargetCreateInfo(Desc).Dimensions) ||
            Desc.VariantId != RayReconstructionVariant_DLSS || !Desc.IsHDR || !bExposureValid ||
            !Desc.LinearDepth || Desc.DepthInverted || Desc.MotionVectorsJittered || !Desc.RoughnessPacked ||
            Frame.ptConsts.imageWidth != Desc.InputWidth || Frame.ptConsts.imageHeight != Desc.InputHeight ||
            Frame.view.ViewportSize != float2{static_cast<float>(Desc.InputWidth), static_cast<float>(Desc.InputHeight)} ||
            !std::isfinite(Frame.view.PixelOffset.x) || !std::isfinite(Frame.view.PixelOffset.y) ||
            Frame.ptConsts.camera.Jitter != Frame.view.PixelOffset * float2{1, -1} ||
            !GetViewForwardSign(Frame.view.MatViewToClip, ForwardSign))
            return false;
        for (size_t Index = 0; Index < 16; ++Index)
            if (!std::isfinite(Frame.view.MatWorldToView.Data()[Index]) ||
                !std::isfinite(Frame.view.MatViewToClip.Data()[Index]))
                return false;

        Attribs.pContext            = Context;
        Attribs.StateTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        // OutputColor is overwritten with noisy radiance by RR prepare. The
        // display-size ProcessedOutputColor is distinct even in native mode.
        Attribs.pColorSRV                 = Targets.GetOutputColorSRV();
        Attribs.pDepthSRV                 = Targets.GetRRDepthSRV();
        Attribs.pMotionVectorsSRV         = Targets.GetRRMotionVectorsSRV();
        Attribs.pDiffuseAlbedoSRV         = Targets.GetRRDiffuseAlbedoSRV();
        Attribs.pSpecularAlbedoSRV        = Targets.GetRRSpecAlbedoSRV();
        Attribs.pNormalsSRV               = Targets.GetRRNormalsAndRoughnessSRV();
        Attribs.pOutputUAV                = Targets.GetProcessedOutputColorUAV();
        Attribs.pRoughnessSRV             = nullptr;
        Attribs.pExposureSRV              = Desc.AutoExposure ? nullptr : pExposureSRV;
        Attribs.pDiffuseHitDistanceSRV    = nullptr;
        Attribs.pSpecularHitDistanceSRV   = nullptr;
        Attribs.pSpecularMotionVectorsSRV = Targets.GetRRSpecMotionVectorsSRV();
        Attribs.JitterX                   = Frame.view.PixelOffset.x;
        Attribs.JitterY                   = Frame.view.PixelOffset.y;
        Attribs.MotionVectorScaleX        = 1;
        Attribs.MotionVectorScaleY        = 1;
        Attribs.PreExposure               = 1;
        Attribs.ExposureScale             = 1;
        Attribs.ResetHistory              = ResetHistory;
        // CPU matrices are already unjittered, row-major Diligent row-vector
        // transforms. The native provider owns the NGX transpose conversion.
        // A right-handed view is handed over in the forward-positive basis of
        // the RR depth: WorldToView * FlipZ and FlipZ * ViewToClip keep the
        // frame's world-to-clip transform.
        const float4x4 FlipZ       = float4x4::Scale(1, 1, ForwardSign);
        const float4x4 WorldToView = Frame.view.MatWorldToView * FlipZ;
        const float4x4 ViewToClip  = FlipZ * Frame.view.MatViewToClip;
        std::memcpy(Attribs.WorldToView, WorldToView.Data(), sizeof(Attribs.WorldToView));
        std::memcpy(Attribs.ViewToClip, ViewToClip.Data(), sizeof(Attribs.ViewToClip));
        return true;
    }
};

} // namespace Diligent
