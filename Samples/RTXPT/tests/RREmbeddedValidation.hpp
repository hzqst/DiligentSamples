/* Copyright 2026 Diligent Graphics LLC
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

// Included by RTXPTSample.cpp after the class definition. RealtimeDLSSRR on the Sample's scene: embedded views of one
// embedded scene, reconstructed by the device's DLSS RR through a test double of its factory. The double forwards to
// the real provider, records every Execute, and injects the query, creation and Execute failures the real provider
// cannot be made to report (NGX accepts out-of-range sizes, M5.0a). RR01 checks the exact query and the execution of
// RR Native, RR02 exposure (R06 and the D4 A/B), RR03 right-handed views, RR04 history and RR05 failures.
// RTXPTSample.cpp includes Float16.hpp, ObjectBase.hpp and RefCountedObjectImpl.hpp for it.

namespace
{
struct RREmbeddedExecuteRecord
{
    float2                Jitter        = float2{0, 0};
    bool                  Reset         = false;
    bool                  Accepted      = false;
    bool                  ExposureBound = false;
    float                 PreExposure   = 0;
    float                 ExposureScale = 0;
    std::array<float, 16> WorldToView   = {};
    std::array<float, 16> ViewToClip    = {};
};

// Forwards to the real reconstruction. FailNext Execute calls fail without reaching it, as a rejected evaluation would.
class RREmbeddedRecordingReconstruction final : public ObjectBase<IRayReconstruction>
{
public:
    using TBase = ObjectBase<IRayReconstruction>;
    RREmbeddedRecordingReconstruction(IReferenceCounters* pRefCounters, IRayReconstruction* pInner) :
        TBase{pRefCounters}, m_Inner{pInner} {}
    IMPLEMENT_QUERY_INTERFACE_IN_PLACE(IID_RayReconstruction, TBase)

    const RayReconstructionDesc& DILIGENT_CALL_TYPE GetDesc() const override final { return m_Inner->GetDesc(); }
    void DILIGENT_CALL_TYPE                         GetJitterOffset(Uint32 Index, float& JitterX, float& JitterY) const override final
    {
        m_Inner->GetJitterOffset(Index, JitterX, JitterY);
    }
    Bool DILIGENT_CALL_TYPE Execute(const ExecuteRayReconstructionAttribs& Attribs) override final
    {
        const bool Fail = FailNext != 0;
        FailNext -= Fail ? 1u : 0u;
        const bool              Accepted = !Fail && m_Inner->Execute(Attribs);
        RREmbeddedExecuteRecord Record;
        Record.Jitter        = float2{Attribs.JitterX, Attribs.JitterY};
        Record.Reset         = Attribs.ResetHistory != False;
        Record.Accepted      = Accepted;
        Record.ExposureBound = Attribs.pExposureSRV != nullptr;
        Record.PreExposure   = Attribs.PreExposure;
        Record.ExposureScale = Attribs.ExposureScale;
        std::copy(std::begin(Attribs.WorldToView), std::end(Attribs.WorldToView), Record.WorldToView.begin());
        std::copy(std::begin(Attribs.ViewToClip), std::end(Attribs.ViewToClip), Record.ViewToClip.begin());
        Records.push_back(Record);
        return Accepted;
    }
    float2 GetJitter(Uint32 Index) const
    {
        float2 Jitter;
        m_Inner->GetJitterOffset(Index, Jitter.x, Jitter.y);
        return Jitter;
    }
    Uint32 GetAccepted() const
    {
        return static_cast<Uint32>(std::count_if(Records.begin(), Records.end(), [](const RREmbeddedExecuteRecord& Record) { return Record.Accepted; }));
    }
    Uint32 GetResets() const
    {
        return static_cast<Uint32>(std::count_if(Records.begin(), Records.end(), [](const RREmbeddedExecuteRecord& Record) { return Record.Reset; }));
    }

    std::vector<RREmbeddedExecuteRecord> Records;
    Uint32                               FailNext = 0;

private:
    RefCntAutoPtr<IRayReconstruction> m_Inner;
};

enum class RREmbeddedFactoryFault
{
    None,
    NoVariants,
    Query,
    //The query succeeds with an optimal width outside the minimum/maximum it reports.
    Range,
    Create,
};

class RREmbeddedTestFactory final : public ObjectBase<IRayReconstructionFactory>
{
public:
    using TBase = ObjectBase<IRayReconstructionFactory>;
    RREmbeddedTestFactory(IReferenceCounters* pRefCounters, IRayReconstructionFactory* pInner, RREmbeddedFactoryFault Fault) :
        TBase{pRefCounters}, m_Inner{pInner}, m_Fault{Fault} {}
    IMPLEMENT_QUERY_INTERFACE_IN_PLACE(IID_RayReconstructionFactory, TBase)

    void DILIGENT_CALL_TYPE EnumerateVariants(Uint32& NumVariants, RayReconstructionInfo* Variants) const override final
    {
        if (m_Fault == RREmbeddedFactoryFault::NoVariants)
            NumVariants = 0;
        else
            m_Inner->EnumerateVariants(NumVariants, Variants);
    }
    Bool DILIGENT_CALL_TYPE GetSourceSettings(const RayReconstructionSourceSettingsAttribs& Attribs, RayReconstructionSourceSettings& Settings) const override final
    {
        ++Queries;
        if (m_Fault == RREmbeddedFactoryFault::Query || !m_Inner->GetSourceSettings(Attribs, Settings))
        {
            Settings = {};
            return False;
        }
        if (m_Fault == RREmbeddedFactoryFault::Range)
            Settings.OptimalInputWidth = Settings.MaxInputWidth + 1;
        return True;
    }
    void DILIGENT_CALL_TYPE CreateRayReconstruction(const RayReconstructionDesc& Desc, IRayReconstruction** ppReconstruction) override final
    {
        *ppReconstruction = nullptr;
        ++CreateAttempts;
        RefCntAutoPtr<IRayReconstruction> Inner;
        if (m_Fault != RREmbeddedFactoryFault::Create)
            m_Inner->CreateRayReconstruction(Desc, &Inner);
        if (!Inner)
            return;
        RefCntAutoPtr<RREmbeddedRecordingReconstruction> Reconstruction{MakeNewRCObj<RREmbeddedRecordingReconstruction>()(Inner.RawPtr())};
        Reconstructions.push_back(Reconstruction);
        *ppReconstruction = Reconstruction.Detach();
    }
    void DILIGENT_CALL_TYPE SetMessageCallback(DebugMessageCallbackType Callback) const override final { m_Inner->SetMessageCallback(Callback); }
    void DILIGENT_CALL_TYPE SetBreakOnError(bool BreakOnError) const override final { m_Inner->SetBreakOnError(BreakOnError); }

    std::vector<RefCntAutoPtr<RREmbeddedRecordingReconstruction>> Reconstructions;
    Uint32                                                        CreateAttempts = 0;
    mutable Uint32                                                Queries        = 0;

private:
    RefCntAutoPtr<IRayReconstructionFactory> m_Inner;
    RREmbeddedFactoryFault                   m_Fault;
};

//The display sizes the views are validated at: the M5.0a query size and an odd one.
constexpr Uint32 kRREmbeddedWidth = 1280, kRREmbeddedHeight = 720;
constexpr Uint32 kRREmbeddedOddWidth = 1281, kRREmbeddedOddHeight = 721;
//Frames a view renders before its counters and output are checked.
constexpr Uint32 kRREmbeddedFrames = 8;
//R06 (frozen in M5.0a, D6): the reconstructed mean luminance stays within 10% of its noisy input's on a static frame.
constexpr double kRREmbeddedLuminanceTolerance = 0.1;
//R01 (D6): the RR depth of a right-handed view matches the equivalent left-handed one within this relative error.
constexpr double kRREmbeddedDepthTolerance = 1e-5;
//R02 and R03 (D6): the matrices RR receives agree within this multiple of max(1, |m|), the motion within 0.25 pixels.
constexpr float  kRREmbeddedMatrixTolerance = 1e-5f;
constexpr double kRREmbeddedMotionTolerance = 0.25;
//RR03: a static frame, then one after a camera move.
constexpr Uint32 kRREmbeddedHandednessFrames = 2;
//The D4 A/B: static frames before the measurement, then still frames whose adjacent display difference is averaged.
constexpr Uint32 kRREmbeddedWarmupFrames = 16;
constexpr Uint32 kRREmbeddedStillFrames  = 8;
//The display exposures of R06; 0.15 is c1a0's.
constexpr float kRREmbeddedExposures[] = {0.15f, 0.5f, 1.0f, 2.0f};
//The D4 decision rule, fixed before the A/B: keep the display exposure unless it fails R06 or its still-frame display
//difference exceeds the unit exposure's by the D6 still-sequence allowance (x1.25 + 0.5/255).
constexpr double kRREmbeddedStabilityScale  = 1.25;
constexpr double kRREmbeddedStabilityOffset = 0.5;
//RR03 and RR04: the camera moves this far along X on each motion frame, which moves the Sample's nearest geometry by
//several render pixels, well above the motion tolerance.
constexpr Uint32 kRREmbeddedMotionFrames = 4;
constexpr float  kRREmbeddedMotionStep   = 0.05f;
//RR05: one past the last RTXPTRayReconstructionQuality, a host jitter the view must reject, and the successful frames
//before each injected failure.
constexpr Uint32 kRREmbeddedUnknownQuality      = 4;
constexpr float  kRREmbeddedHostJitter          = 0.25f;
constexpr Uint32 kRREmbeddedFramesBeforeFailure = 2;
} // namespace

void RTXPTSample::RunEmbeddedReconstructionValidation()
{
    Uint32&    Failures = m_EmbeddedReconstructionValidationFailures;
    const auto Check    = [&](bool Condition, const std::string& What) {
        if (!Condition)
        {
            ++Failures;
            LOG_ERROR_MESSAGE("RR embedded validation failed: ", What);
        }
        return Condition;
    };
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Shaders;
    CreateEmbeddedShaderSource(&Shaders);
    CreateEmbeddedReconstructionFactory();
    IRayReconstructionFactory* const Provider = m_EmbeddedReconstructionFactory;
    RayReconstructionInfo            DLSSRR;
    if (!Check(EnsureEmbeddedScene(Shaders), "the embedded scene could not be created") ||
        !Check(Provider != nullptr && RTXPTRayReconstructionInputs::FindDLSSVariant(*Provider, DLSSRR), "the device has no DLSS RR variant"))
        return;
    RTXPTEmbeddedScene& Scene  = *m_EmbeddedScene;
    FirstPersonCamera   Camera = m_Camera;
    double              Time   = 0;
    //FirstPersonCamera rebuilds its view matrix only in Update; MoveCamera keeps the matrix and the position in step.
    float4x4   CameraView = Camera.GetViewMatrix();
    const auto MoveCamera = [&](const float3& Offset) {
        Camera.SetPos(Camera.GetPos() + Offset);
        CameraView = float4x4::Translation(-Offset) * CameraView;
    };
    const auto RestoreCamera = [&]() {
        Camera     = m_Camera;
        CameraView = Camera.GetViewMatrix();
    };

    const auto MakeFactory = [&](RREmbeddedFactoryFault Fault) {
        return RefCntAutoPtr<RREmbeddedTestFactory>{MakeNewRCObj<RREmbeddedTestFactory>()(Provider, Fault)};
    };
    const auto CreateView = [&](RTXPTViewMode Mode, RTXPTViewSize Display, IRayReconstructionFactory* Factory, RTXPTErrorCode& Code,
                                RTXPTRayReconstructionQuality Quality = RTXPTRayReconstructionQuality::Native, bool DisplayExposure = true, RTXPTViewSize RenderSize = {}) {
        RTXPTEmbeddedViewCreateInfo CI;
        CI.Scene                         = &Scene;
        CI.Shaders                       = Shaders;
        CI.Cache                         = m_pEmbeddedStateCache;
        CI.PSOCache                      = m_pPSOCache;
        CI.Mode                          = Mode;
        CI.DisplaySize                   = Display;
        CI.RenderSize                    = Mode == RTXPTViewMode::RealtimeDLSSRR || RenderSize.Width != 0 ? RenderSize : Display;
        CI.RayReconstructionFactory      = Factory;
        CI.ReconstructionQuality         = Quality;
        CI.ReconstructionDisplayExposure = DisplayExposure;
        auto View                        = std::make_unique<RTXPTEmbeddedView>();
        Code                             = View->Initialize(m_pDevice, m_pImmediateContext, CI);
        return View;
    };
    //Each host frame prepares the scene once; its views then render that frame. The sequence runs within one Sample
    //frame, so it ends each host frame as presentation would, releasing that frame's dynamic descriptors and memory.
    const auto Prepare = [&]() {
        m_pImmediateContext->Flush();
        m_pImmediateContext->FinishFrame();
        Time += 1.0 / 60.0;
        return Check(Scene.Prepare(m_pImmediateContext) == RTXPTErrorCode::Ok, "the scene frame could not be prepared");
    };
    const auto RenderWith = [&](RTXPTEmbeddedView& View, const float4x4& WorldToView, const float4x4& ViewToClip, bool Reset, float Exposure, float2 HostJitter) {
        const RTXPTViewSize R          = View.GetRenderSize();
        const RTXPTViewSize D          = View.GetDisplaySize();
        const auto          CameraData = MakePathTracerCameraData(Camera, R.Width, R.Height, D.Width, D.Height, 1, 0, HostJitter);
        const auto          ViewData   = MakePathTracerViewData(WorldToView, ViewToClip, R.Width, R.Height, HostJitter);
        return View.Render(CameraData, ViewData, Time, Reset, Exposure);
    };
    const auto Render = [&](RTXPTEmbeddedView& View, bool Reset = false, float Exposure = 1.0f, float2 HostJitter = float2{0, 0}) {
        return RenderWith(View, CameraView, Camera.GetProjMatrix(), Reset, Exposure, HostJitter);
    };
    const auto CheckCode = [&](RTXPTErrorCode Expected, RTXPTErrorCode Actual, const std::string& What) {
        return Check(Expected == Actual, What + ": expected '" + GetRTXPTErrorCodeString(Expected) + "', got '" + GetRTXPTErrorCodeString(Actual) + "'");
    };
    const auto CheckOk = [&](RTXPTErrorCode Code, const std::string& What) {
        return CheckCode(RTXPTErrorCode::Ok, Code, What);
    };
    const auto SameSize = [](const RTXPTViewSize& Lhs, const RTXPTViewSize& Rhs) {
        return Lhs.Width == Rhs.Width && Lhs.Height == Rhs.Height;
    };
    //Every Execute requests the jitter of the accepted ones before it, so a failed frame's index repeats.
    const auto CheckJitterSequence = [&](const RREmbeddedRecordingReconstruction& Reconstruction, const std::string& Name) {
        Uint32 Index = 0;
        bool   Match = true;
        for (const RREmbeddedExecuteRecord& Record : Reconstruction.Records)
        {
            Match = Match && Record.Jitter == Reconstruction.GetJitter(Index);
            Index += Record.Accepted ? 1u : 0u;
        }
        return Check(Match, Name + ": the jitter does not follow the successful frame count");
    };
    //Copies the first subresource of Texture to the CPU: Height rows of Stride bytes.
    const auto Readback = [&](ITexture* Texture, std::vector<Uint8>& Data, Uint32& Stride) {
        Data.clear();
        Stride = 0;
        if (Texture == nullptr)
            return false;
        TextureDesc StagingDesc    = Texture->GetDesc();
        StagingDesc.Name           = "RR embedded validation readback";
        StagingDesc.Usage          = USAGE_STAGING;
        StagingDesc.BindFlags      = BIND_NONE;
        StagingDesc.CPUAccessFlags = CPU_ACCESS_READ;
        //The display output is typeless so that its views choose the encoding; the copy keeps the encoded bytes.
        if (StagingDesc.Format == TEX_FORMAT_RGBA8_TYPELESS)
            StagingDesc.Format = TEX_FORMAT_RGBA8_UNORM;
        RefCntAutoPtr<ITexture> Staging;
        m_pDevice->CreateTexture(StagingDesc, nullptr, &Staging);
        if (!Staging)
            return false;
        CopyTextureAttribs Copy;
        Copy.pSrcTexture              = Texture;
        Copy.pDstTexture              = Staging;
        Copy.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        Copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        m_pImmediateContext->CopyTexture(Copy);
        m_pImmediateContext->WaitForIdle();
        MappedTextureSubresource Map;
        m_pImmediateContext->MapTextureSubresource(Staging, 0, 0, MAP_READ, MAP_FLAG_DO_NOT_WAIT, nullptr, Map);
        if (!Map.pData)
            return false;
        Stride            = static_cast<Uint32>(Map.Stride);
        const auto* Begin = static_cast<const Uint8*>(Map.pData);
        Data.assign(Begin, Begin + static_cast<size_t>(Map.Stride) * StagingDesc.Height);
        m_pImmediateContext->UnmapTextureSubresource(Staging, 0, 0);
        return true;
    };
    //Mean Rec.709 luminance of an RGBA16F texture; false if a texel is not finite.
    const auto MeanLuminance = [&](ITexture* Texture, double& Mean) {
        Mean = 0;
        std::vector<Uint8> Data;
        Uint32             Stride = 0;
        if (Texture == nullptr || Texture->GetDesc().Format != TEX_FORMAT_RGBA16_FLOAT || !Readback(Texture, Data, Stride))
            return false;
        bool         Finite = true;
        const Uint32 Width = Texture->GetDesc().Width, Height = Texture->GetDesc().Height;
        for (Uint32 Y = 0; Y < Height; ++Y)
        {
            const auto* Row = reinterpret_cast<const Uint16*>(Data.data() + static_cast<size_t>(Y) * Stride);
            for (Uint32 X = 0; X < Width; ++X)
            {
                const double R = static_cast<double>(Float16{Row[X * 4 + 0]}), G = static_cast<double>(Float16{Row[X * 4 + 1]}),
                             B = static_cast<double>(Float16{Row[X * 4 + 2]});
                Finite         = Finite && std::isfinite(R) && std::isfinite(G) && std::isfinite(B);
                Mean += 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
        }
        Mean /= static_cast<double>(Width) * Height;
        return Finite;
    };
    //The view's encoded display bytes (RGBA8), Width x Height x 4 without row padding.
    const auto ReadDisplay = [&](RTXPTEmbeddedView& View, std::vector<Uint8>& Pixels) {
        Pixels.clear();
        std::vector<Uint8>  Data;
        Uint32              Stride = 0;
        const RTXPTViewSize D      = View.GetDisplaySize();
        if (!View.GetOutputInfo(RTXPTViewOutput::Display).Valid || !Readback(View.GetOutput(), Data, Stride))
            return false;
        for (Uint32 Y = 0; Y < D.Height; ++Y)
        {
            const auto Row = Data.begin() + static_cast<std::ptrdiff_t>(Y) * Stride;
            Pixels.insert(Pixels.end(), Row, Row + D.Width * 4u);
        }
        return true;
    };
    //Mean absolute RGB difference of two display images, in 8-bit units.
    const auto DisplayDifference = [](const std::vector<Uint8>& Lhs, const std::vector<Uint8>& Rhs) {
        if (Lhs.size() != Rhs.size() || Lhs.empty())
            return -1.0;
        double Sum = 0;
        for (size_t Index = 0; Index < Lhs.size(); ++Index)
            Sum += Index % 4 == 3 ? 0.0 : std::abs(static_cast<double>(Lhs[Index]) - Rhs[Index]);
        return Sum / (static_cast<double>(Lhs.size()) / 4 * 3);
    };
    const RTXPTViewSize Display{kRREmbeddedWidth, kRREmbeddedHeight};

    LOG_INFO_MESSAGE("RR embedded validation RR01: exact DLSS RR Native queries and execution");
    for (const RTXPTViewSize& CaseDisplay : {Display, RTXPTViewSize{kRREmbeddedOddWidth, kRREmbeddedOddHeight}})
    {
        const std::string                      Name = "RR01 " + std::to_string(CaseDisplay.Width) + "x" + std::to_string(CaseDisplay.Height);
        RayReconstructionSourceSettingsAttribs Query;
        Query.VariantId    = DLSSRR.VariantId;
        Query.OutputWidth  = CaseDisplay.Width;
        Query.OutputHeight = CaseDisplay.Height;
        Query.Quality      = RAY_RECONSTRUCTION_QUALITY_NATIVE;
        RayReconstructionSourceSettings Expected;
        if (!Check(Provider->GetSourceSettings(Query, Expected), Name + ": DLSS RR does not support the display size"))
            continue;
        auto           Factory = MakeFactory(RREmbeddedFactoryFault::None);
        RTXPTErrorCode Code    = RTXPTErrorCode::Ok;
        auto           View    = CreateView(RTXPTViewMode::RealtimeDLSSRR, CaseDisplay, Factory, Code);
        if (!CheckOk(Code, Name + ": view creation") || !Check(Factory->Reconstructions.size() == 1, Name + ": the view must create one feature"))
            continue;
        const RREmbeddedRecordingReconstruction& Reconstruction = *Factory->Reconstructions[0];
        const RayReconstructionDesc&             Desc           = Reconstruction.GetDesc();
        const RTXPTViewSize                      RenderSize{Expected.OptimalInputWidth, Expected.OptimalInputHeight};
        Check(SameSize(RenderSize, CaseDisplay) && SameSize(View->GetRenderSize(), RenderSize) && SameSize(View->GetDisplaySize(), CaseDisplay),
              Name + ": RR Native must render at the queried size, the display size");
        Check(Desc.VariantId == RayReconstructionVariant_DLSS && Desc.Quality == RAY_RECONSTRUCTION_QUALITY_NATIVE &&
                  Desc.InputWidth == RenderSize.Width && Desc.InputHeight == RenderSize.Height &&
                  Desc.OutputWidth == CaseDisplay.Width && Desc.OutputHeight == CaseDisplay.Height && !Desc.AutoExposure &&
                  Desc.DLSSPreset == RAY_RECONSTRUCTION_DLSS_PRESET_F && Desc.LinearDepth && !Desc.DepthInverted &&
                  !Desc.MotionVectorsJittered && Desc.RoughnessPacked,
              Name + ": the feature does not match the query, explicit exposure (D4) and preset F (D5)");
        const RTXPTViewRayReconstructionInfo& Info = View->GetRayReconstructionInfo();
        Check(Info.Variant == DLSSRR.Name && Info.Quality == RTXPTRayReconstructionQuality::Native &&
                  SameSize(Info.RenderSize, RenderSize) && SameSize(Info.DisplaySize, CaseDisplay),
              Name + ": the reported variant, quality or size is wrong");
        for (Uint32 Frame = 0; Frame < kRREmbeddedFrames; ++Frame)
            if (Prepare())
                CheckOk(Render(*View), Name + ": frame " + std::to_string(Frame));
        const auto& Records = Reconstruction.Records;
        Check(Records.size() == kRREmbeddedFrames && Reconstruction.GetAccepted() == kRREmbeddedFrames, Name + ": one accepted Execute per frame");
        Check(!Records.empty() && Records.front().Reset && Reconstruction.GetResets() == 1, Name + ": only the first frame may restart history");
        Check(std::all_of(Records.begin(), Records.end(), [](const RREmbeddedExecuteRecord& Record) {
                  return Record.ExposureBound && Record.PreExposure == 1.0f && Record.ExposureScale == 1.0f;
              }),
              Name + ": RR must read the explicit exposure, with PreExposure and ExposureScale 1");
        CheckJitterSequence(Reconstruction, Name);
        Check(Info.Executes == kRREmbeddedFrames && Info.Resets == 1 && Info.Failures == 0 && Info.Exposure == 1.0f,
              Name + ": wrong execute/reset/failure counters or exposure");
        const RTXPTRealtimeStats& Stats = View->GetRealtimeStats();
        Check(Stats.NrdInstances == 0 && Stats.DenoiseDispatches == 0 && Stats.MergeDispatches == 0 && Stats.SuperResolutionExecutes == 0 &&
                  Stats.RayReconstructionPrepares == kRREmbeddedFrames && Stats.RayReconstructionExecutes == kRREmbeddedFrames,
              Name + ": RR must replace NRD, the merge and super resolution (R08)");
        Check(View->GetSuperResolutionInfo().Executes == 0 && View->GetSuperResolutionInfo().Variant.empty(), Name + ": RR created a super resolution feature");
        const RTXPTViewOutputInfo Output = View->GetOutputInfo(RTXPTViewOutput::Display);
        Check(Output.Valid && SameSize(Output.Size, CaseDisplay) && Output.FrameIndex == kRREmbeddedFrames, Name + ": the display output is not published at the display size");
        Check(View->GetOutputInfo(RTXPTViewOutput::DenoisedRadiance).Texture == nullptr && View->GetOutputInfo(RTXPTViewOutput::UpscaledRadiance).Texture == nullptr &&
                  View->GetOutputInfo(RTXPTViewOutput::LayerHistoryConfidence).Texture == nullptr,
              Name + ": RR published NRD or super resolution outputs");
        const auto OutputMatches = [&](RTXPTViewOutput Kind, const RTXPTViewSize& Size, TEXTURE_FORMAT Format) {
            const RTXPTViewOutputInfo Texture = View->GetOutputInfo(Kind);
            return Texture.Valid && Texture.Texture != nullptr && SameSize(Texture.Size, Size) && Texture.ResourceFormat == Format;
        };
        Check(OutputMatches(RTXPTViewOutput::ReconstructedRadiance, CaseDisplay, TEX_FORMAT_RGBA16_FLOAT) &&
                  OutputMatches(RTXPTViewOutput::RayReconstructionColor, RenderSize, TEX_FORMAT_RGBA16_FLOAT) &&
                  OutputMatches(RTXPTViewOutput::RayReconstructionDepth, RenderSize, TEX_FORMAT_R32_FLOAT) &&
                  OutputMatches(RTXPTViewOutput::RayReconstructionMotion, RenderSize, TEX_FORMAT_RG16_FLOAT) &&
                  OutputMatches(RTXPTViewOutput::RayReconstructionSpecularMotion, RenderSize, TEX_FORMAT_RG16_FLOAT) &&
                  OutputMatches(RTXPTViewOutput::RayReconstructionNormalRoughness, RenderSize, TEX_FORMAT_RGBA16_FLOAT) &&
                  OutputMatches(RTXPTViewOutput::RayReconstructionDiffuseAlbedo, RenderSize, TEX_FORMAT_RGBA16_FLOAT) &&
                  OutputMatches(RTXPTViewOutput::RayReconstructionSpecularAlbedo, RenderSize, TEX_FORMAT_RGBA16_FLOAT) &&
                  OutputMatches(RTXPTViewOutput::RayReconstructionExposure, RTXPTViewSize{1, 1}, TEX_FORMAT_R32_FLOAT),
              Name + ": RR inputs must be at the render size and the reconstructed HDR at the display size");
        LOG_INFO_MESSAGE(Name, ": variant '", Info.Variant, "', render ", RenderSize.Width, 'x', RenderSize.Height,
                         " (min ", Expected.MinInputWidth, 'x', Expected.MinInputHeight, ", max ", Expected.MaxInputWidth, 'x', Expected.MaxInputHeight, ")");
    }

    LOG_INFO_MESSAGE("RR embedded validation RR02: exposure (R06) and the D4 A/B");
    {
        struct ExposureResult
        {
            double Ratio  = 0;
            double Still  = -1;
            bool   Finite = false;
        };
        //Renders the warm-up frames, then the still frames, and returns their mean adjacent display difference.
        const auto MeasureStill = [&](RTXPTEmbeddedView& View, float Exposure, const std::string& Name) {
            for (Uint32 Frame = 0; Frame < kRREmbeddedWarmupFrames; ++Frame)
                if (!Prepare() || !CheckOk(Render(View, false, Exposure), Name + ": warm-up frame"))
                    return -1.0;
            std::vector<Uint8> Previous, Current;
            if (!Check(ReadDisplay(View, Previous), Name + ": the display output could not be read"))
                return -1.0;
            double Sum = 0;
            for (Uint32 Frame = 0; Frame < kRREmbeddedStillFrames; ++Frame)
            {
                if (!Prepare() || !CheckOk(Render(View, false, Exposure), Name + ": still frame") ||
                    !Check(ReadDisplay(View, Current), Name + ": the display output could not be read"))
                    return -1.0;
                Sum += DisplayDifference(Previous, Current);
                Previous.swap(Current);
            }
            return Sum / kRREmbeddedStillFrames;
        };
        auto Factory         = MakeFactory(RREmbeddedFactoryFault::None);
        bool DisplayExposure = true;
        for (const float Exposure : kRREmbeddedExposures)
        {
            const std::string ExposureName = "RR02 exposure " + std::to_string(Exposure);
            ExposureResult    Results[2];
            for (const bool bDisplayExposure : {true, false})
            {
                const std::string Name   = ExposureName + (bDisplayExposure ? " (display exposure)" : " (exposure 1)");
                ExposureResult&   Result = Results[bDisplayExposure ? 0 : 1];
                RTXPTErrorCode    Code   = RTXPTErrorCode::Ok;
                auto              View   = CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, Code, RTXPTRayReconstructionQuality::Native, bDisplayExposure);
                if (!CheckOk(Code, Name + ": view creation"))
                    continue;
                Result.Still = MeasureStill(*View, Exposure, Name);
                if (Result.Still < 0)
                    continue;
                //R06: RR received the exposure the scheme sets, as the texture it read holds, and kept the HDR level.
                const float        Expected = bDisplayExposure ? Exposure : 1.0f;
                const float        Received = View->GetRayReconstructionInfo().Exposure;
                std::vector<Uint8> ExposureData;
                Uint32             ExposureStride = 0;
                float              Stored         = 0;
                if (Readback(View->GetOutputInfo(RTXPTViewOutput::RayReconstructionExposure).Texture, ExposureData, ExposureStride) && ExposureData.size() >= sizeof(Stored))
                    std::memcpy(&Stored, ExposureData.data(), sizeof(Stored));
                Check(std::abs(Received - Expected) <= Expected * 1e-6f && Stored == Received, Name + ": RR received or stored the wrong exposure");
                double Input = 0, Reconstructed = 0;
                Result.Finite = MeanLuminance(View->GetOutputInfo(RTXPTViewOutput::RayReconstructionColor).Texture, Input) &&
                    MeanLuminance(View->GetOutputInfo(RTXPTViewOutput::ReconstructedRadiance).Texture, Reconstructed);
                Result.Ratio = Input > 0 ? Reconstructed / Input : 0;
                Check(Result.Finite, Name + ": the noisy input or the reconstructed HDR is not finite");
                LOG_INFO_MESSAGE(Name, ": RR exposure ", Received, ", mean luminance noisy input ", Input, ", reconstructed ", Reconstructed,
                                 ", ratio ", Result.Ratio, ", still display difference ", Result.Still, "/255");
            }
            //The RealtimeNative reference of the D6 stability guard, on the same frames and exposure.
            {
                RTXPTErrorCode Code   = RTXPTErrorCode::Ok;
                auto           Native = CreateView(RTXPTViewMode::RealtimeNative, Display, nullptr, Code);
                if (CheckOk(Code, ExposureName + " Native: view creation"))
                    LOG_INFO_MESSAGE(ExposureName, " (RealtimeNative): still display difference ", MeasureStill(*Native, Exposure, ExposureName + " Native"), "/255");
            }
            const bool bLevelKept  = Results[0].Finite && std::abs(Results[0].Ratio - 1.0) <= kRREmbeddedLuminanceTolerance;
            const bool bStableKept = Results[0].Still >= 0 && Results[1].Still >= 0 &&
                Results[0].Still <= Results[1].Still * kRREmbeddedStabilityScale + kRREmbeddedStabilityOffset;
            Check(bLevelKept, ExposureName + ": the reconstruction changed the HDR level (R06)");
            DisplayExposure = DisplayExposure && bLevelKept && bStableKept;
        }
        LOG_INFO_MESSAGE("RR embedded validation D4: ", DisplayExposure ? "the display exposure meets the decision rule" : "the display exposure fails the decision rule");
        Check(DisplayExposure, "RR02: the display exposure fails the D4 decision rule");
    }

    LOG_INFO_MESSAGE("RR embedded validation RR03: a right-handed view of the same world");
    {
        auto           Factory = MakeFactory(RREmbeddedFactoryFault::None);
        RTXPTErrorCode CodeL = RTXPTErrorCode::Ok, CodeR = RTXPTErrorCode::Ok;
        auto           Left  = CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, CodeL);
        auto           Right = CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, CodeR);
        //The game's views look along -Z: View * FlipZ and FlipZ * Projection keep the world, rays and clip space. The
        //second frame moves the camera, so the motion RR receives is not zero.
        const float4x4 FlipZ    = float4x4::Scale(1, 1, -1);
        bool           Rendered = CheckOk(CodeL, "RR03 left-handed view") && CheckOk(CodeR, "RR03 right-handed view");
        for (Uint32 Frame = 0; Frame < kRREmbeddedHandednessFrames && Rendered; ++Frame)
        {
            if (Frame != 0)
                MoveCamera(float3{kRREmbeddedMotionStep, 0, 0});
            Rendered = Prepare() && CheckOk(Render(*Left), "RR03 left-handed frame") &&
                CheckOk(RenderWith(*Right, CameraView * FlipZ, FlipZ * Camera.GetProjMatrix(), false, 1.0f, float2{0, 0}), "RR03 right-handed frame");
        }
        RestoreCamera();
        if (Rendered && Check(Factory->Reconstructions.size() == 2, "RR03: each view must own a feature"))
        {
            //Both views hand RR the same unjittered matrices and jitter (R02).
            const auto& LeftRecords  = Factory->Reconstructions[0]->Records;
            const auto& RightRecords = Factory->Reconstructions[1]->Records;
            bool        SameExecute  = LeftRecords.size() == kRREmbeddedHandednessFrames && RightRecords.size() == kRREmbeddedHandednessFrames;
            for (size_t Index = 0; SameExecute && Index < LeftRecords.size(); ++Index)
            {
                SameExecute = LeftRecords[Index].Jitter == RightRecords[Index].Jitter;
                for (size_t Element = 0; Element < LeftRecords[Index].WorldToView.size(); ++Element)
                {
                    const float LW = LeftRecords[Index].WorldToView[Element], LC = LeftRecords[Index].ViewToClip[Element];
                    SameExecute = SameExecute &&
                        std::abs(RightRecords[Index].WorldToView[Element] - LW) <= kRREmbeddedMatrixTolerance * std::max(1.0f, std::abs(LW)) &&
                        std::abs(RightRecords[Index].ViewToClip[Element] - LC) <= kRREmbeddedMatrixTolerance * std::max(1.0f, std::abs(LC));
                }
            }
            Check(SameExecute, "RR03: the right-handed view hands RR other matrices or jitter (R02)");
            //R01 and R03: the same forward-positive depth, foreground included, and the same render-pixel motion.
            std::vector<Uint8> LeftDepth, RightDepth, LeftMotion, RightMotion;
            Uint32             DepthStride = 0, RightDepthStride = 0, MotionStride = 0, RightMotionStride = 0;
            if (Check(Readback(Left->GetOutputInfo(RTXPTViewOutput::RayReconstructionDepth).Texture, LeftDepth, DepthStride) &&
                          Readback(Right->GetOutputInfo(RTXPTViewOutput::RayReconstructionDepth).Texture, RightDepth, RightDepthStride) &&
                          Readback(Left->GetOutputInfo(RTXPTViewOutput::RayReconstructionMotion).Texture, LeftMotion, MotionStride) &&
                          Readback(Right->GetOutputInfo(RTXPTViewOutput::RayReconstructionMotion).Texture, RightMotion, RightMotionStride) &&
                          DepthStride == RightDepthStride && MotionStride == RightMotionStride,
                      "RR03: the RR depth or motion could not be read"))
            {
                //The RR prepare's depth of sky and invalid paths (kEnvironmentMapSceneDistance).
                constexpr float kSkyDepth    = 5000000.0f;
                Uint32          Foreground   = 0;
                bool            DepthsMatch  = true;
                double          MaxMotion    = 0;
                double          MotionOffset = 0;
                for (Uint32 Y = 0; Y < Display.Height; ++Y)
                {
                    for (Uint32 X = 0; X < Display.Width; ++X)
                    {
                        float      L = 0, R = 0;
                        const auto DepthOffset = static_cast<size_t>(Y) * DepthStride + X * sizeof(float);
                        std::memcpy(&L, LeftDepth.data() + DepthOffset, sizeof(float));
                        std::memcpy(&R, RightDepth.data() + DepthOffset, sizeof(float));
                        Foreground += L < kSkyDepth ? 1u : 0u;
                        DepthsMatch    = DepthsMatch && std::abs(static_cast<double>(R) - L) <= kRREmbeddedDepthTolerance * std::abs(L);
                        const auto* LM = reinterpret_cast<const Uint16*>(LeftMotion.data() + static_cast<size_t>(Y) * MotionStride) + X * 2;
                        const auto* RM = reinterpret_cast<const Uint16*>(RightMotion.data() + static_cast<size_t>(Y) * MotionStride) + X * 2;
                        for (Uint32 Axis = 0; Axis < 2; ++Axis)
                        {
                            const double LV = static_cast<double>(Float16{LM[Axis]}), RV = static_cast<double>(Float16{RM[Axis]});
                            MaxMotion    = std::max(MaxMotion, std::abs(LV));
                            MotionOffset = std::max(MotionOffset, std::abs(RV - LV));
                        }
                    }
                }
                LOG_INFO_MESSAGE("RR03: ", Foreground, " foreground pixels of ", Display.Width * Display.Height, ", largest motion ", MaxMotion,
                                 " px, largest right-handed motion difference ", MotionOffset, " px");
                Check(Foreground != 0 && DepthsMatch, "RR03: the right-handed RR depth differs from the left-handed one or is all sky (R01)");
                Check(MaxMotion > kRREmbeddedMotionTolerance && MotionOffset <= kRREmbeddedMotionTolerance, "RR03: the right-handed motion differs from the left-handed one (R03)");
            }
            double     Input = 0, Reconstructed = 0;
            const bool Finite = MeanLuminance(Right->GetOutputInfo(RTXPTViewOutput::RayReconstructionColor).Texture, Input) &&
                MeanLuminance(Right->GetOutputInfo(RTXPTViewOutput::ReconstructedRadiance).Texture, Reconstructed);
            LOG_INFO_MESSAGE("RR03: right-handed mean luminance noisy input ", Input, ", reconstructed ", Reconstructed);
            Check(Finite && Input > 0 && std::abs(Reconstructed / Input - 1.0) <= kRREmbeddedLuminanceTolerance,
                  "RR03: the right-handed reconstruction is not finite or changed the HDR level (R06)");
        }
    }

    LOG_INFO_MESSAGE("RR embedded validation RR04: history across motion, cuts and missing frames");
    {
        auto           Factory = MakeFactory(RREmbeddedFactoryFault::None);
        RTXPTErrorCode Code    = RTXPTErrorCode::Ok;
        auto           View    = CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, Code);
        if (CheckOk(Code, "RR04 view"))
        {
            const RREmbeddedRecordingReconstruction& Reconstruction = *Factory->Reconstructions[0];
            const auto                               Step           = [&](bool Reset, bool ExpectReset, const std::string& What) {
                if (!Prepare() || !CheckOk(Render(*View, Reset), What))
                    return;
                Check(Reconstruction.Records.back().Reset == ExpectReset && View->GetRayReconstructionInfo().LastReset == ExpectReset,
                      What + (ExpectReset ? ": RR history must restart" : ": RR history must continue"));
            };
            Step(false, true, "first frame");
            Step(false, false, "static frame");
            Step(false, false, "static frame");
            for (Uint32 Frame = 0; Frame < kRREmbeddedMotionFrames; ++Frame)
            {
                MoveCamera(float3{kRREmbeddedMotionStep, 0, 0});
                Step(false, false, "camera motion");
            }
            Step(true, true, "cut");
            Step(false, false, "after the cut");
            Prepare();
            Step(false, true, "after a missed host frame");
            Step(false, false, "after the recovery");
            CheckJitterSequence(Reconstruction, "RR04");
            Check(View->GetOutputInfo(RTXPTViewOutput::Display).FrameIndex == Reconstruction.GetAccepted(), "RR04: successful frames and the jitter sequence diverged");
        }
        RestoreCamera();
    }

    LOG_INFO_MESSAGE("RR embedded validation RR05: unavailable, unsupported and failed reconstruction");
    {
        RTXPTErrorCode Code = RTXPTErrorCode::Ok;
        CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, nullptr, Code);
        CheckCode(RTXPTErrorCode::RayReconstructionUnavailable, Code, "no factory");
        const std::tuple<RREmbeddedFactoryFault, RTXPTErrorCode, const char*> Faults[] = {
            {RREmbeddedFactoryFault::NoVariants, RTXPTErrorCode::RayReconstructionUnavailable, "no DLSS RR variant"},
            {RREmbeddedFactoryFault::Query, RTXPTErrorCode::RayReconstructionQueryFailed, "unsupported quality"},
            {RREmbeddedFactoryFault::Range, RTXPTErrorCode::RayReconstructionQueryFailed, "query outside its own range"},
            {RREmbeddedFactoryFault::Create, RTXPTErrorCode::RayReconstructionCreateFailed, "feature creation"},
        };
        for (const auto& [Fault, Expected, Name] : Faults)
        {
            auto Factory = MakeFactory(Fault);
            CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, Code);
            CheckCode(Expected, Code, Name);
            Check(Factory->CreateAttempts == (Fault == RREmbeddedFactoryFault::Create ? 1u : 0u), std::string{Name} + ": a feature was created for an unsupported query");
        }
        auto Factory = MakeFactory(RREmbeddedFactoryFault::None);
        CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, Code, static_cast<RTXPTRayReconstructionQuality>(kRREmbeddedUnknownQuality));
        CheckCode(RTXPTErrorCode::RayReconstructionQueryFailed, Code, "unknown quality");
        Check(Factory->Queries == 0 && Factory->CreateAttempts == 0, "unknown quality: queried or created");
        CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, Code, RTXPTRayReconstructionQuality::Native, true, Display);
        CheckCode(RTXPTErrorCode::UnsupportedViewConfiguration, Code, "a host render size");
        auto View = CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, Code);
        if (CheckOk(Code, "jitter view") && Prepare())
        {
            CheckCode(RTXPTErrorCode::InvalidArgument, Render(*View, false, 1.0f, float2{kRREmbeddedHostJitter, 0}), "host jitter");
            Check(!View->GetOutputInfo(RTXPTViewOutput::Display).Valid && Factory->Reconstructions.back()->Records.empty(), "host jitter: executed or published");
        }
    }
    {
        //The RR depth has no forward axis for a projection without a perspective w; the inputs fail before Execute.
        auto           Factory = MakeFactory(RREmbeddedFactoryFault::None);
        RTXPTErrorCode Code    = RTXPTErrorCode::Ok;
        auto           View    = CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, Code);
        if (CheckOk(Code, "RR05 orthographic view"))
        {
            const RREmbeddedRecordingReconstruction& Reconstruction = *Factory->Reconstructions[0];
            for (Uint32 Frame = 0; Frame < kRREmbeddedFramesBeforeFailure; ++Frame)
                if (Prepare())
                    CheckOk(Render(*View), "RR05 frame before the orthographic one");
            const float4x4 Orthographic = float4x4::OrthoOffCenter(-1, 1, -1, 1, Camera.GetProjAttribs().NearClipPlane, Camera.GetProjAttribs().FarClipPlane, false);
            if (Prepare())
                CheckCode(RTXPTErrorCode::RayReconstructionInputsFailed, RenderWith(*View, CameraView, Orthographic, false, 1.0f, float2{0, 0}), "RR05 orthographic frame");
            Check(!View->GetOutputInfo(RTXPTViewOutput::Display).Valid && Reconstruction.Records.size() == kRREmbeddedFramesBeforeFailure && View->GetRayReconstructionInfo().Failures == 1,
                  "RR05: an orthographic frame must fail before Execute without output");
            if (Prepare())
                CheckOk(Render(*View), "RR05 frame after the orthographic one");
            Check(Reconstruction.Records.size() == kRREmbeddedFramesBeforeFailure + 1 && Reconstruction.Records.back().Reset &&
                      View->GetOutputInfo(RTXPTViewOutput::Display).FrameIndex == kRREmbeddedFramesBeforeFailure + 1,
                  "RR05: the frame after the failed inputs must restart history");
            CheckJitterSequence(Reconstruction, "RR05 orthographic");
        }
    }
    {
        auto           Factory = MakeFactory(RREmbeddedFactoryFault::None);
        RTXPTErrorCode CodeA = RTXPTErrorCode::Ok, CodeC = RTXPTErrorCode::Ok;
        auto           A = CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, CodeA);
        auto           C = CreateView(RTXPTViewMode::RealtimeDLSSRR, Display, Factory, CodeC);
        if (CheckOk(CodeA, "RR05 view") && CheckOk(CodeC, "RR05 neighbour") && Factory->Reconstructions.size() == 2)
        {
            RREmbeddedRecordingReconstruction&       RA = *Factory->Reconstructions[0];
            const RREmbeddedRecordingReconstruction& RC = *Factory->Reconstructions[1];
            RA.FailNext                                 = 1;
            if (Prepare())
            {
                CheckCode(RTXPTErrorCode::RayReconstructionCreateFailed, Render(*A), "RR05 first Execute failure");
                CheckOk(Render(*C), "RR05 neighbour");
            }
            Check(!A->GetOutputInfo(RTXPTViewOutput::Display).Valid && A->GetOutputInfo(RTXPTViewOutput::Display).Texture == nullptr &&
                      A->GetRayReconstructionInfo().Failures == 1 && A->GetRayReconstructionInfo().Executes == 0,
                  "RR05: a failed first frame must not be published or counted");
            if (Prepare())
            {
                CheckOk(Render(*A), "RR05 retry");
                CheckOk(Render(*C), "RR05 neighbour");
            }
            Check(RA.Records.size() == 2 && RA.Records[1].Reset && RA.Records[1].Jitter == RA.Records[0].Jitter, "RR05: the retry must restart with the failed frame's jitter index");
            for (Uint32 Frame = 0; Frame < kRREmbeddedFramesBeforeFailure; ++Frame)
                if (Prepare())
                    CheckOk(Render(*A), "RR05 A") && CheckOk(Render(*C), "RR05 neighbour");
            const Uint32 FramesBefore = A->GetOutputInfo(RTXPTViewOutput::Display).FrameIndex;
            RA.FailNext               = 1;
            if (Prepare())
            {
                CheckCode(RTXPTErrorCode::RayReconstructionExecuteFailed, Render(*A), "RR05 evaluation failure");
                CheckOk(Render(*C), "RR05 neighbour during the failure");
            }
            const RTXPTViewOutputInfo Failed = A->GetOutputInfo(RTXPTViewOutput::Display);
            Check(!Failed.Valid && Failed.Texture == nullptr && Failed.FrameIndex == 0 && A->GetRealtimeLayerBuffer() == nullptr &&
                      !A->GetOutputInfo(RTXPTViewOutput::ReconstructedRadiance).Valid && !A->GetOutputInfo(RTXPTViewOutput::RayReconstructionColor).Valid,
                  "RR05: a failed frame must not publish any output");
            Check(A->GetRayReconstructionInfo().Failures == 2 && A->GetRayReconstructionInfo().Executes == FramesBefore, "RR05: a failure must not count as a success");
            if (Prepare())
            {
                CheckOk(Render(*A), "RR05 recovery");
                CheckOk(Render(*C), "RR05 neighbour after the failure");
            }
            Check(RA.Records.size() >= 2 && RA.Records.back().Reset && A->GetRayReconstructionInfo().LastReset &&
                      A->GetOutputInfo(RTXPTViewOutput::Display).FrameIndex == FramesBefore + 1,
                  "RR05: the recovery must restart RR history");
            Check(RA.Records.size() >= 2 && RA.Records.back().Jitter == RA.Records[RA.Records.size() - 2].Jitter, "RR05: the recovery must reuse the failed frame's jitter index");
            Check(RC.GetResets() == 1 && RC.GetAccepted() == RC.Records.size(), "RR05: the neighbour must keep its history");
            CheckJitterSequence(RA, "RR05 A");
            CheckJitterSequence(RC, "RR05 neighbour");
        }
    }
    m_pImmediateContext->WaitForIdle();
    LOG_INFO_MESSAGE("RR embedded validation finished: failures=", Failures);
    std::fflush(stdout);
}
