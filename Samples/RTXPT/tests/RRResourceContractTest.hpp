// Copyright 2026 Diligent Graphics LLC
// Licensed under the Apache License, Version 2.0.
#pragma once

#include "../src/RTXPTRayReconstructionInputs.hpp"
#include "../src/RTXPTPostProcessPass.hpp"
#include <filesystem>

namespace
{
void CheckRRPrepareWrapper(IRenderDevice* Device, IDeviceContext* Context, IEngineFactory* Factory, const char* ShaderDirectory)
{
    SampleConstants Frame;
    Frame.ptConsts._activeDenoisingLayerCount = 3;
    Frame.ptConsts.genericTSLineStride        = 8;
    Frame.ptConsts.genericTSPlaneStride       = 64;
    Frame.ptConsts.DLSSRRBrightnessClampK     = 0;
    Frame.ptConsts.perPixelJitterAAScale      = 0;
    Frame.ptConsts.camera.CameraW             = float3{0, 0, 1};
    Frame.ptConsts.camera.FarZ                = 100;
    BufferDesc CBDesc;
    CBDesc.Size      = sizeof(Frame);
    CBDesc.BindFlags = BIND_UNIFORM_BUFFER;
    BufferData             CBData{&Frame, sizeof(Frame)};
    RefCntAutoPtr<IBuffer> Constants;
    Device->CreateBuffer(CBDesc, &CBData, &Constants);
    RTXPTPostProcessPass Pass;
    const auto           OldDirectory = std::filesystem::current_path();
    std::filesystem::current_path(std::filesystem::path{ShaderDirectory}.parent_path());
    const bool Ready = Pass.Initialize(Device, Factory, nullptr, Constants, true);
    std::filesystem::current_path(OldDirectory);
    Require(Ready, "Production post-process initialization failed");

    RTXPTRenderTargets          Targets;
    RTXPTRenderTargetCreateInfo Info;
    Info.CreateRealtimeResources          = true;
    Info.CreateRayReconstructionResources = true;
    Require(Targets.Resize(Device, Info), "Wrapper RR resources unavailable");
    const Uint16 Stable[] = {Float16{8.0f}.Raw(), 0, 0, Float16{1.0f}.Raw()};
    const float  Depth    = 2;
    const Uint16 Motion[] = {Float16{2.0f}.Raw(), Float16{-3.0f}.Raw(), 0, 0};
    const Uint32 Header   = 0xffffffffu;
    const auto   Upload   = [&](ITextureView* View, const void* Data, Uint64 Size, Uint32 Slice = 0) {
        Context->UpdateTexture(View->GetTexture(), 0, Slice, Box{0, 1, 0, 1}, TextureSubResData{Data, Size},
                               RESOURCE_STATE_TRANSITION_MODE_TRANSITION, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    };
    Upload(Targets.GetStableRadianceUAV(), Stable, sizeof(Stable));
    // Keep eight-byte texels together so each tiny fixture upload stays aligned.
    Upload(Targets.GetScreenMotionVectorsUAV(), Motion, sizeof(Motion));
    Upload(Targets.GetDepthUAV(), &Depth, sizeof(Depth));
    for (Uint32 Slice = 0; Slice < 4; ++Slice)
        Upload(Targets.GetDenoisingLayersHeaderUAV(), &Header, sizeof(Header), Slice);
    std::array<Plane, 192> Planes = {};
    Context->UpdateBuffer(Targets.GetDenoisingLayersBuffer(), 0, sizeof(Planes), Planes.data(), RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    for (int Iteration = 0; Iteration < 2; ++Iteration)
    {
        Require(Pass.RunDLSSRRDenoiserPrepareInputs(Context, Targets), "Production RR prepare dispatch failed");
        Require(Targets.GetOutputColorSRV()->GetTexture()->GetState() == RESOURCE_STATE_SHADER_RESOURCE &&
                    Targets.GetRRDepthSRV()->GetTexture()->GetState() == RESOURCE_STATE_SHADER_RESOURCE,
                "Prepare did not transition outputs to RR read state");
        TextureDesc ReadDesc    = Targets.GetOutputColorSRV()->GetTexture()->GetDesc();
        ReadDesc.BindFlags      = BIND_NONE;
        ReadDesc.Usage          = USAGE_STAGING;
        ReadDesc.CPUAccessFlags = CPU_ACCESS_READ;
        RefCntAutoPtr<ITexture> Readback;
        Device->CreateTexture(ReadDesc, nullptr, &Readback);
        CopyTextureAttribs Copy;
        Copy.pSrcTexture              = Targets.GetOutputColorSRV()->GetTexture();
        Copy.pDstTexture              = Readback;
        Copy.SrcTextureTransitionMode = Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Context->CopyTexture(Copy);
        Context->WaitForIdle();
        MappedTextureSubresource Mapped;
        Context->MapTextureSubresource(Readback, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Mapped);
        Require(Mapped.pData != nullptr, "Wrapper readback failed");
        Near(8, Float16::HalfBitsToFloat(*static_cast<const Uint16*>(Mapped.pData)));
        Context->UnmapTextureSubresource(Readback, 0, 0);
    }
    // Explicit output aliasing must fail before resource binding/dispatch.
    RTXPTDenoiserPostProcessAttribs Bad;
    Bad.pRenderTargets  = &Targets;
    Bad.pMergeOutputUAV = Bad.pRRDiffuseAlbedoUAV = Targets.GetOutputColorUAV();
    Bad.pRRSpecAlbedoUAV                          = Targets.GetRRSpecAlbedoUAV();
    Bad.pRRNormalsAndRoughnessUAV                 = Targets.GetRRNormalsAndRoughnessUAV();
    Bad.pRRDepthUAV                               = Targets.GetRRDepthUAV();
    Bad.pRRMotionVectorsUAV                       = Targets.GetRRMotionVectorsUAV();
    Require(!Pass.RunDLSSRRDenoiserPrepareInputs(Context, Bad), "Aliased RR prepare accepted");
    Pass.Reset();
    Context->Flush();
    Context->FinishFrame();
    Context->WaitForIdle();
    std::cout << "Production RR prepare wrapper, rebind and alias rejection passed\n";
}

void CheckRRResourceContract(IRenderDevice* Device, IDeviceContext* Context, const char* FactoryPath)
{
    HMODULE Library = LoadLibraryA(FactoryPath);
    Require(Library != nullptr, "Cannot load RR factory DLL");
    auto CreateFactory = reinterpret_cast<void (*)(IRenderDevice*, IRayReconstructionFactory**)>(
        GetProcAddress(Library, "CreateRayReconstructionFactory"));
    Require(CreateFactory != nullptr, "Missing RR factory export");
    RefCntAutoPtr<IRayReconstructionFactory> Factory;
    CreateFactory(Device, &Factory);
    Require(Factory != nullptr, "Cannot create RR factory");

    RTXPTRenderTargets      Targets;
    RefCntAutoPtr<ITexture> PreviousGuide;
    // Native -> Quality -> resized Quality -> native, then RR -> NRD -> RR.
    for (Uint32 Iteration = 0; Iteration < 4; ++Iteration)
    {
        RayReconstructionSourceSettingsAttribs Query;
        Query.OutputWidth  = Iteration == 2 ? 768 : 640;
        Query.OutputHeight = Iteration == 2 ? 432 : 360;
        Query.Quality      = Iteration == 0 || Iteration == 3 ? RAY_RECONSTRUCTION_QUALITY_NATIVE : RAY_RECONSTRUCTION_QUALITY_QUALITY;
        RayReconstructionSourceSettings Source;
        Require(Factory->GetSourceSettings(Query, Source), "Native RR source size query failed");
        RayReconstructionDesc Desc;
        Require(RTXPTRayReconstructionInputs::MakeDesc(Query, Source, Desc), "Valid RR source settings rejected");
        const auto Info = RTXPTRayReconstructionInputs::MakeRenderTargetCreateInfo(Desc);
        if (!Targets.Resize(Device, Info))
            throw std::runtime_error{Targets.GetLastFailureReason()};
        Require(Targets.HasRayReconstructionRenderTargets(), "Missing RR render targets");
        Require(!Targets.GetDenoiserNormalRoughnessSRV() && !Targets.GetDenoiserOutDiffRadianceHitDistSRV(0),
                "RR unnecessarily allocated NRD-only resources");
        Require(Targets.GetRRNormalsAndRoughnessSRV()->GetTexture() != PreviousGuide, "Resize reused obsolete guides");
        PreviousGuide = Targets.GetRRNormalsAndRoughnessSRV()->GetTexture();
        Require(Targets.Resize(Device, Info), "Unchanged RR resize failed");
        Require(Targets.GetRRNormalsAndRoughnessSRV()->GetTexture() == PreviousGuide, "Unchanged RR resize recreated resources");

        SampleConstants Frame;
        Frame.ptConsts.imageWidth    = Desc.InputWidth;
        Frame.ptConsts.imageHeight   = Desc.InputHeight;
        Frame.view.ViewportSize      = float2{static_cast<float>(Desc.InputWidth), static_cast<float>(Desc.InputHeight)};
        Frame.view.PixelOffset       = float2{0.25f, -0.375f};
        Frame.ptConsts.camera.Jitter = float2{0.25f, 0.375f};
        Frame.view.MatWorldToView    = float4x4::Translation(2, 3, 4);
        Frame.view.MatViewToClip     = float4x4::Scale(2, 3, 4);
        ExecuteRayReconstructionAttribs Execute;
        Execute.pExposureSRV = Execute.pRoughnessSRV = Execute.pDiffuseHitDistanceSRV =
            Execute.pSpecularHitDistanceSRV = Execute.pSpecularMotionVectorsSRV = Targets.GetOutputColorSRV();
        Require(RTXPTRayReconstructionInputs::MakeExecuteAttribs(Context, Targets, Desc, Frame, true, Execute), "RR execute adapter failed");
        Require(!Execute.pExposureSRV && !Execute.pRoughnessSRV && !Execute.pDiffuseHitDistanceSRV &&
                    !Execute.pSpecularHitDistanceSRV && Execute.pSpecularMotionVectorsSRV == Targets.GetRRSpecMotionVectorsSRV(),
                "Optional guides retained previous bindings");
        Near(0.25f, Execute.JitterX);
        Near(-0.375f, Execute.JitterY);
        Near(1, Execute.MotionVectorScaleX);
        Near(1, Execute.MotionVectorScaleY);
        Near(1, Execute.PreExposure);
        Near(1, Execute.ExposureScale);
        Require(Execute.ResetHistory && Desc.RoughnessPacked && Desc.LinearDepth && !Desc.MotionVectorsJittered && !Desc.DepthInverted, "RR flags mismatch");
        for (size_t Index = 0; Index < 16; ++Index)
        {
            Near(Frame.view.MatWorldToView.Data()[Index], Execute.WorldToView[Index]);
            Near(Frame.view.MatViewToClip.Data()[Index], Execute.ViewToClip[Index]);
        }
        const std::array<ITextureView*, 8>  Views   = {{Execute.pColorSRV, Execute.pDepthSRV, Execute.pMotionVectorsSRV,
                                                     Execute.pDiffuseAlbedoSRV, Execute.pSpecularAlbedoSRV, Execute.pNormalsSRV, Execute.pOutputUAV, Execute.pSpecularMotionVectorsSRV}};
        const std::array<TEXTURE_FORMAT, 8> Formats = {{TEX_FORMAT_RGBA16_FLOAT, TEX_FORMAT_R32_FLOAT, TEX_FORMAT_RG16_FLOAT,
                                                        TEX_FORMAT_RGBA16_FLOAT, TEX_FORMAT_RGBA16_FLOAT, TEX_FORMAT_RGBA16_FLOAT, TEX_FORMAT_RGBA16_FLOAT, TEX_FORMAT_RG16_FLOAT}};
        for (size_t Index = 0; Index < Views.size(); ++Index)
        {
            Require(Views[Index] != nullptr, "Missing required RR view");
            const auto& V      = Views[Index]->GetDesc();
            const auto& T      = Views[Index]->GetTexture()->GetDesc();
            const bool  Output = Index == 6;
            Require(T.Type == RESOURCE_DIM_TEX_2D && T.MipLevels == 1 && T.SampleCount == 1 &&
                        V.MostDetailedMip == 0 && V.NumMipLevels == 1 && V.FirstArraySlice == 0 &&
                        T.Width == (Output ? Desc.OutputWidth : Desc.InputWidth) &&
                        T.Height == (Output ? Desc.OutputHeight : Desc.InputHeight) &&
                        V.Format == Formats[Index] && T.Format == Formats[Index] &&
                        V.ViewType == (Output ? TEXTURE_VIEW_UNORDERED_ACCESS : TEXTURE_VIEW_SHADER_RESOURCE),
                    "RR texture contract mismatch");
            for (size_t Previous = 0; Previous < Index; ++Previous)
                Require(Views[Previous]->GetTexture() != Views[Index]->GetTexture(), "Aliased RR resources");
        }
        Frame.ptConsts.imageWidth++;
        Require(!RTXPTRayReconstructionInputs::MakeExecuteAttribs(Context, Targets, Desc, Frame, false, Execute) &&
                    !Execute.pColorSRV && !Execute.pOutputUAV,
                "Stale frame accepted or bindings retained");
        std::cout << "RR resource contract " << Desc.InputWidth << 'x' << Desc.InputHeight << " -> "
                  << Desc.OutputWidth << 'x' << Desc.OutputHeight << " passed\n";

        if (Iteration == 3)
        {
            auto NrdInfo                             = Info;
            NrdInfo.CreateRayReconstructionResources = false;
            const bool NrdNormalSupported            = (Device->GetTextureFormatInfoExt(TEX_FORMAT_RGB10A2_UNORM).BindFlags & BIND_UNORDERED_ACCESS) != 0;
            if (!NrdNormalSupported)
            {
                NrdInfo.CreateRealtimeResources = false;
                std::cout << "NRD mode switch skipped: existing RGB10A2 UAV unsupported; testing reference mode switch\n";
            }
            Require(Targets.Resize(Device, NrdInfo), "Switch to NRD resources failed");
            Require(!Targets.HasRayReconstructionRenderTargets() && !Targets.GetRRDepthSRV() &&
                        !Targets.GetRRDiffuseAlbedoSRV() && !Targets.GetRRSpecAlbedoSRV() &&
                        !Targets.GetRRNormalsAndRoughnessSRV() && !Targets.GetRRMotionVectorsSRV() && !Targets.GetRRSpecMotionVectorsSRV(),
                    "RR resources retained after mode switch");
            if (NrdNormalSupported)
                Require(Targets.HasRealtimeRenderTargets() &&
                            Targets.GetDenoiserNormalRoughnessSRV()->GetDesc().Format == TEX_FORMAT_RGB10A2_UNORM,
                        "NRD encoding changed");
            Require(Targets.Resize(Device, Info) && Targets.HasRayReconstructionRenderTargets(), "Switch back to RR failed");
        }
    }
    Targets.Reset();
    Require(!Targets.HasRayReconstructionRenderTargets() && !Targets.GetRRNormalsAndRoughnessSRV(), "RR reset retained resources");
    RayReconstructionSourceSettingsAttribs Query;
    RayReconstructionSourceSettings        Source;
    RayReconstructionDesc                  Desc;
    Require(!RTXPTRayReconstructionInputs::MakeDesc(Query, Source, Desc) && Desc.InputWidth == 0, "Invalid query settings accepted");
    Context->WaitForIdle();
}
} // namespace
