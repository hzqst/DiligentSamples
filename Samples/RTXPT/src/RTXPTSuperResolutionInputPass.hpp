// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "Buffer.h"
#include "DeviceContext.h"
#include "EngineFactory.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "Texture.h"

namespace Diligent
{
struct IRenderStateCache;

//Converts the realtime guides of one view into the render-size inputs of a temporal super resolution upscaler:
// - Depth: the dominant layer's ray distance along this frame's camera ray (jitter included), projected with the
//   view's MatWorldToClipNoOffset to forward [0,1] depth (near < far). Sky, misses and invalid distances are far (1).
//   The distance is the one the motion vectors used, so a replaced primary surface yields its virtual point.
// - Motion: the xy of the primary motion, current-to-previous render pixels without jitter (scale 1). Non-finite is 0.
// - Ignore-history mask: 1 on a surface whose primary motion w reports no valid correspondence, 0 elsewhere. w is
//   cleared when any retained layer lacks history, so the mask is conservative, not exact primary validity. Sky keeps
//   its camera motion and its history.
// - Exposure: 1x1 with 1, so the upscaler applies no exposure; display exposure belongs to tone mapping.
//DLSS receives the mask as its current-color bias mask, preferably R8 UNORM with 1 for the current color. Diligent
//documents R8_UINT for the ignore-history mask, but no provider checks its format.
class RTXPTSuperResolutionInputPass
{
public:
    static constexpr TEXTURE_FORMAT DepthFormat             = TEX_FORMAT_R32_FLOAT;
    static constexpr TEXTURE_FORMAT MotionVectorsFormat     = TEX_FORMAT_RG16_FLOAT;
    static constexpr TEXTURE_FORMAT IgnoreHistoryMaskFormat = TEX_FORMAT_R8_UNORM;
    static constexpr TEXTURE_FORMAT ExposureFormat          = TEX_FORMAT_R16_FLOAT;

    void Reset();
    //FrameConstants holds the SampleConstants of the frame whose guides are converted. The cache is optional.
    bool Initialize(IRenderDevice* Device, IEngineFactory* Factory, IRenderStateCache* Cache, IBuffer* FrameConstants);
    //Allocates the converted inputs at render size; the previous ones are released.
    bool Resize(IRenderDevice* Device, Uint32 Width, Uint32 Height);
    //RayDistanceSRV and MotionVectorsSRV are the R32F ray distance and RGBA16F primary motion of the realtime targets,
    //at least as large as the converted inputs.
    bool Prepare(IDeviceContext* Context, ITextureView* RayDistanceSRV, ITextureView* MotionVectorsSRV);

    bool          IsReady() const { return m_PSO && m_Depth; }
    Uint32        GetWidth() const { return m_Width; }
    Uint32        GetHeight() const { return m_Height; }
    ITextureView* GetDepthSRV() const;
    ITextureView* GetMotionVectorsSRV() const;
    ITextureView* GetIgnoreHistoryMaskSRV() const;
    ITextureView* GetExposureSRV() const;

private:
    RefCntAutoPtr<IPipelineState>         m_PSO;
    RefCntAutoPtr<IShaderResourceBinding> m_SRB;
    RefCntAutoPtr<ITexture>               m_Depth;
    RefCntAutoPtr<ITexture>               m_MotionVectors;
    RefCntAutoPtr<ITexture>               m_IgnoreHistoryMask;
    RefCntAutoPtr<ITexture>               m_Exposure;
    Uint32                                m_Width  = 0;
    Uint32                                m_Height = 0;
};
} // namespace Diligent
