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

// Resource/parameter adapter. Factory ownership, jitter sequence, history and
// Execute scheduling are handled by the RR pass, not by this stateless adapter.
struct RTXPTRayReconstructionInputs
{
    static bool MakeDesc(const RayReconstructionSourceSettingsAttribs& Query,
                         const RayReconstructionSourceSettings&        Source,
                         RayReconstructionDesc&                        Desc)
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
        Desc.AutoExposure          = True;
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

    static bool MakeExecuteAttribs(IDeviceContext*                  Context,
                                   const RTXPTRenderTargets&        Targets,
                                   const RayReconstructionDesc&     Desc,
                                   const SampleConstants&           Frame,
                                   bool                             ResetHistory,
                                   ExecuteRayReconstructionAttribs& Attribs)
    {
        // Also clear bindings on failure: never reuse optional guides or stale
        // resources from a previous frame/mode/size.
        Attribs = {};
        if (Context == nullptr || !Targets.HasRayReconstructionRenderTargets() ||
            !(Targets.GetDimensions() == MakeRenderTargetCreateInfo(Desc).Dimensions) ||
            Desc.VariantId != RayReconstructionVariant_DLSS || !Desc.IsHDR || !Desc.AutoExposure ||
            !Desc.LinearDepth || Desc.DepthInverted || Desc.MotionVectorsJittered || !Desc.RoughnessPacked ||
            Frame.ptConsts.imageWidth != Desc.InputWidth || Frame.ptConsts.imageHeight != Desc.InputHeight ||
            Frame.view.ViewportSize != float2{static_cast<float>(Desc.InputWidth), static_cast<float>(Desc.InputHeight)} ||
            !std::isfinite(Frame.view.PixelOffset.x) || !std::isfinite(Frame.view.PixelOffset.y) ||
            Frame.ptConsts.camera.Jitter != Frame.view.PixelOffset * float2{1, -1})
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
        Attribs.pExposureSRV              = nullptr;
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
        std::memcpy(Attribs.WorldToView, Frame.view.MatWorldToView.Data(), sizeof(Attribs.WorldToView));
        std::memcpy(Attribs.ViewToClip, Frame.view.MatViewToClip.Data(), sizeof(Attribs.ViewToClip));
        return true;
    }
};

} // namespace Diligent
