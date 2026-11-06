/* Copyright 2026 Diligent Graphics LLC
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

// Included by RTXPTSample.cpp after the class definition. S05-S08 of RealtimeDLSSSR on the Sample's scene: embedded
// views of one embedded scene, upscaled by the device's DLSS through a test double of its factory. The double forwards
// to the real provider, records every Execute, and injects the query, creation and Execute failures the real provider
// cannot be made to report (NGX accepts out-of-range sizes, M4.1). A failure after an accepted Execute (tone mapping)
// has no injection point; it shares the Render failure path that invalidates the output and resets the next frame.
// RTXPTSample.cpp includes Float16.hpp, ObjectBase.hpp and RefCountedObjectImpl.hpp for it.

namespace
{
struct SRExecuteRecord
{
    float2            Jitter         = float2{0, 0};
    bool              Reset          = false;
    bool              Accepted       = false;
    bool              TemporalInputs = false;
    TEXTURE_VIEW_TYPE OutputType     = TEXTURE_VIEW_UNDEFINED;
};

// Forwards to the real upscaler. FailNext Execute calls fail without reaching it, as a rejected evaluation would.
class SRRecordingUpscaler final : public ObjectBase<ISuperResolution>
{
public:
    using TBase = ObjectBase<ISuperResolution>;
    SRRecordingUpscaler(IReferenceCounters* pRefCounters, ISuperResolution* pInner) :
        TBase{pRefCounters}, m_Inner{pInner} {}
    IMPLEMENT_QUERY_INTERFACE_IN_PLACE(IID_SuperResolution, TBase)

    const SuperResolutionDesc& DILIGENT_CALL_TYPE GetDesc() const override final { return m_Inner->GetDesc(); }
    void DILIGENT_CALL_TYPE                       GetJitterOffset(Uint32 Index, float& JitterX, float& JitterY) const override final
    {
        m_Inner->GetJitterOffset(Index, JitterX, JitterY);
    }
    Bool DILIGENT_CALL_TYPE Execute(const ExecuteSuperResolutionAttribs& Attribs) override final
    {
        const bool Fail = FailNext != 0;
        FailNext -= Fail ? 1u : 0u;
        const bool Accepted = !Fail && m_Inner->Execute(Attribs);
        Records.push_back({float2{Attribs.JitterX, Attribs.JitterY}, Attribs.ResetHistory != False, Accepted,
                           Attribs.pDepthTextureSRV || Attribs.pMotionVectorsSRV || Attribs.pIgnoreHistoryMaskTextureSRV,
                           Attribs.pOutputTextureView->GetDesc().ViewType});
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
        return static_cast<Uint32>(std::count_if(Records.begin(), Records.end(), [](const SRExecuteRecord& Record) { return Record.Accepted; }));
    }

    std::vector<SRExecuteRecord> Records;
    Uint32                       FailNext = 0;

private:
    RefCntAutoPtr<ISuperResolution> m_Inner;
};

enum class SRFactoryFault
{
    None,
    NoVariants,
    Query,
    //The query succeeds with an optimal width outside the minimum/maximum it reports.
    Range,
    Create,
};

class SRTestFactory final : public ObjectBase<ISuperResolutionFactory>
{
public:
    using TBase = ObjectBase<ISuperResolutionFactory>;
    SRTestFactory(IReferenceCounters* pRefCounters, ISuperResolutionFactory* pInner, SRFactoryFault Fault, SUPER_RESOLUTION_PROVIDER OnlyProvider = SUPER_RESOLUTION_PROVIDER_UNKNOWN) :
        TBase{pRefCounters}, m_Inner{pInner}, m_Fault{Fault}, m_OnlyProvider{OnlyProvider} {}
    IMPLEMENT_QUERY_INTERFACE_IN_PLACE(IID_SuperResolutionFactory, TBase)

    void DILIGENT_CALL_TYPE EnumerateVariants(Uint32& NumVariants, SuperResolutionInfo* Variants) override final
    {
        if (m_Fault == SRFactoryFault::NoVariants)
            NumVariants = 0;
        else if (m_OnlyProvider == SUPER_RESOLUTION_PROVIDER_UNKNOWN)
            m_Inner->EnumerateVariants(NumVariants, Variants);
        else
        {
            Uint32 Count = 0;
            m_Inner->EnumerateVariants(Count, nullptr);
            std::vector<SuperResolutionInfo> Filtered(Count);
            m_Inner->EnumerateVariants(Count, Filtered.data());
            Filtered.resize(Count);
            Filtered.erase(std::remove_if(Filtered.begin(), Filtered.end(), [&](const auto& Info) { return Info.Provider != m_OnlyProvider; }), Filtered.end());
            const auto Available = static_cast<Uint32>(Filtered.size());
            if (Variants)
                std::copy_n(Filtered.begin(), std::min(NumVariants, Available), Variants);
            NumVariants = Available;
        }
    }
    SUPER_RESOLUTION_SOURCE_SETTINGS_STATUS DILIGENT_CALL_TYPE GetSourceSettings(const SuperResolutionSourceSettingsAttribs& Attribs, SuperResolutionSourceSettings& Settings) const override final
    {
        ++Queries;
        const SUPER_RESOLUTION_SOURCE_SETTINGS_STATUS Status = m_Fault == SRFactoryFault::Query ? SUPER_RESOLUTION_SOURCE_SETTINGS_STATUS_UNSUPPORTED : m_Inner->GetSourceSettings(Attribs, Settings);
        if (Status != SUPER_RESOLUTION_SOURCE_SETTINGS_STATUS_SUCCESS)
        {
            Settings = {};
            return Status;
        }
        if (m_Fault == SRFactoryFault::Range)
            Settings.OptimalInputWidth = Settings.MaxInputWidth + 1;
        return Status;
    }
    void DILIGENT_CALL_TYPE CreateSuperResolution(const SuperResolutionDesc& Desc, ISuperResolution** ppUpscaler) override final
    {
        *ppUpscaler = nullptr;
        ++CreateAttempts;
        RefCntAutoPtr<ISuperResolution> Inner;
        if (m_Fault != SRFactoryFault::Create)
            m_Inner->CreateSuperResolution(Desc, &Inner);
        if (!Inner)
            return;
        RefCntAutoPtr<SRRecordingUpscaler> Upscaler{MakeNewRCObj<SRRecordingUpscaler>()(Inner.RawPtr())};
        Upscalers.push_back(Upscaler);
        *ppUpscaler = Upscaler.Detach();
    }
    void DILIGENT_CALL_TYPE SetMessageCallback(DebugMessageCallbackType Callback) const override final { m_Inner->SetMessageCallback(Callback); }
    void DILIGENT_CALL_TYPE SetBreakOnError(bool BreakOnError) const override final { m_Inner->SetBreakOnError(BreakOnError); }
    void DILIGENT_CALL_TYPE SetMemoryAllocator(IMemoryAllocator* pAllocator) const override final { m_Inner->SetMemoryAllocator(pAllocator); }

    std::vector<RefCntAutoPtr<SRRecordingUpscaler>> Upscalers;
    Uint32                                          CreateAttempts = 0;
    mutable Uint32                                  Queries        = 0;
    void                                            SetFault(SRFactoryFault Fault) { m_Fault = Fault; }

private:
    RefCntAutoPtr<ISuperResolutionFactory> m_Inner;
    SRFactoryFault                         m_Fault;
    SUPER_RESOLUTION_PROVIDER              m_OnlyProvider;
};

//The display sizes the views are validated at: the M4.0 query size and an odd one (S04).
constexpr Uint32 kSRValidationWidth = 1280, kSRValidationHeight = 720;
constexpr Uint32 kSRValidationOddWidth = 1281, kSRValidationOddHeight = 721;
//One past the last RTXPTUpscaleQuality, which the view must reject without querying.
constexpr RTXPTUpscaleQuality kSRValidationUnknownQuality = static_cast<RTXPTUpscaleQuality>(static_cast<int>(RTXPTUpscaleQuality::Performance) + 1);
//The upscaled mean luminance relative to its input's. Applying the display exposure (0.5 or 2) inside the upscaler
//would move it by 2x; the allowance covers DLSS reconstruction differences of a static scene.
constexpr double kSRValidationLuminanceTolerance = 0.1;
//Frames a view renders before its counters and output are checked.
constexpr Uint32 kSRValidationFrames = 8;
} // namespace

void RTXPTSample::RunSuperResolutionValidation()
{
    Uint32&       Failures = m_SuperResolutionValidationFailures;
    std::ofstream Report{"sr-validation.log"};
    const auto    Check = [&](bool Condition, const std::string& What) {
        if (!Condition)
        {
            ++Failures;
            LOG_ERROR_MESSAGE("SR validation failed: ", What);
            Report << "FAILED: " << What << '\n';
        }
        return Condition;
    };
    Check(Report.is_open(), "the validation report could not be opened");
    RefCntAutoPtr<IShaderSourceInputStreamFactory> Shaders;
    CreateEmbeddedShaderSource(&Shaders);
    SuperResolutionFactoryCreateInfo FactoryCI;
    FactoryCI.pDevice = m_pDevice;
    RefCntAutoPtr<ISuperResolutionFactory> Provider;
    LoadAndCreateSuperResolutionFactory(FactoryCI, &Provider);
    SuperResolutionInfo DLSS;
    if (!Check(EnsureEmbeddedScene(Shaders), "the embedded scene could not be created") ||
        !Check(Provider && RTXPTSuperResolutionInputs::FindDLSSVariant(*Provider, DLSS), "the device has no DLSS variant"))
        return;
    RTXPTEmbeddedScene& Scene  = *m_EmbeddedScene;
    FirstPersonCamera   Camera = m_Camera;
    double              Time   = 0;

    const auto MakeFactory = [&](SRFactoryFault Fault) {
        return RefCntAutoPtr<SRTestFactory>{MakeNewRCObj<SRTestFactory>()(Provider.RawPtr(), Fault)};
    };
    const auto CreateView = [&](RTXPTViewMode Mode, RTXPTViewSize Display, RTXPTUpscaleQuality Quality, ISuperResolutionFactory* Factory, RTXPTErrorCode& Code, RTXPTViewSize RenderSize = {}, RTXPTSuperResolutionProvider RequestedProvider = RTXPTSuperResolutionProvider::DLSS, bool DeferredFrameCommit = false, RTXPTEmbeddedViewCreateInfo* OutputCreateInfo = nullptr) {
        RTXPTEmbeddedViewCreateInfo CI;
        CI.Scene                   = &Scene;
        CI.Shaders                 = Shaders;
        CI.Cache                   = m_pEmbeddedStateCache;
        CI.PSOCache                = m_pPSOCache;
        CI.Mode                    = Mode;
        CI.DisplaySize             = Display;
        CI.RenderSize              = Mode == RTXPTViewMode::RealtimeDLSSSR || RenderSize.Width != 0 ? RenderSize : Display;
        CI.SuperResolutionFactory  = Factory;
        CI.UpscaleQuality          = Quality;
        CI.SuperResolutionProvider = RequestedProvider;
        CI.DeferredFrameCommit     = DeferredFrameCommit;
        if (OutputCreateInfo)
            *OutputCreateInfo = CI;
        auto View = std::make_unique<RTXPTEmbeddedView>();
        Code      = View->Initialize(m_pDevice, m_pImmediateContext, CI);
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
    const auto Render = [&](RTXPTEmbeddedView& View, bool Reset = false, float Exposure = 1.0f, float2 HostJitter = float2{0, 0}) {
        const RTXPTViewSize      R          = View.GetRenderSize();
        const RTXPTViewSize      D          = View.GetDisplaySize();
        const auto               CameraData = MakePathTracerCameraData(Camera, R.Width, R.Height, D.Width, D.Height, 1, 0, HostJitter);
        const auto               ViewData   = MakePathTracerViewData(Camera.GetViewMatrix(), Camera.GetProjMatrix(), R.Width, R.Height, HostJitter);
        RTXPTViewDisplaySettings Display;
        Display.Exposure = Exposure;
        return View.Render(CameraData, ViewData, Time, Reset, Display);
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
    const auto Resets = [](const SRRecordingUpscaler& Upscaler) {
        return static_cast<Uint32>(std::count_if(Upscaler.Records.begin(), Upscaler.Records.end(), [](const SRExecuteRecord& Record) { return Record.Reset; }));
    };
    //Every Execute requests the jitter of the accepted ones before it, so a failed frame's index repeats.
    const auto CheckJitterSequence = [&](const SRRecordingUpscaler& Upscaler, const std::string& Name) {
        Uint32 Index = 0;
        bool   Match = true;
        for (const SRExecuteRecord& Record : Upscaler.Records)
        {
            Match = Match && Record.Jitter == Upscaler.GetJitter(Index);
            Index += Record.Accepted ? 1u : 0u;
        }
        return Check(Match, Name + ": the jitter does not follow the successful frame count");
    };
    //Mean Rec.709 luminance of an RGBA16F texture; false if a texel is not finite.
    const auto MeanLuminance = [&](ITexture* Texture, double& Mean) {
        Mean = 0;
        if (Texture == nullptr || Texture->GetDesc().Format != TEX_FORMAT_RGBA16_FLOAT)
            return false;
        TextureDesc StagingDesc    = Texture->GetDesc();
        StagingDesc.Name           = "SR validation readback";
        StagingDesc.Usage          = USAGE_STAGING;
        StagingDesc.BindFlags      = BIND_NONE;
        StagingDesc.CPUAccessFlags = CPU_ACCESS_READ;
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
        bool         Finite = true;
        const Uint32 Width = StagingDesc.Width, Height = StagingDesc.Height;
        for (Uint32 Y = 0; Y < Height; ++Y)
        {
            const auto* Row = reinterpret_cast<const Uint16*>(static_cast<const Uint8*>(Map.pData) + Y * Map.Stride);
            for (Uint32 X = 0; X < Width; ++X)
            {
                const double R = static_cast<double>(Float16{Row[X * 4 + 0]}), G = static_cast<double>(Float16{Row[X * 4 + 1]}),
                             B = static_cast<double>(Float16{Row[X * 4 + 2]});
                Finite         = Finite && std::isfinite(R) && std::isfinite(G) && std::isfinite(B);
                Mean += 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
        }
        m_pImmediateContext->UnmapTextureSubresource(Staging, 0, 0);
        Mean /= static_cast<double>(Width) * Height;
        return Finite;
    };
    const RTXPTViewSize Display{kSRValidationWidth, kSRValidationHeight};

    LOG_INFO_MESSAGE("SR validation S05: exact DLSS queries and execution");
    struct QualityCase
    {
        RTXPTUpscaleQuality                Quality;
        SUPER_RESOLUTION_OPTIMIZATION_TYPE Type;
        const char*                        Name;
        RTXPTViewSize                      Display;
    };
    const QualityCase Qualities[] = {
        {RTXPTUpscaleQuality::Quality, SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_QUALITY, "Quality", Display},
        {RTXPTUpscaleQuality::Balanced, SUPER_RESOLUTION_OPTIMIZATION_TYPE_BALANCED, "Balanced", Display},
        {RTXPTUpscaleQuality::Performance, SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_PERFORMANCE, "Performance", Display},
        {RTXPTUpscaleQuality::Quality, SUPER_RESOLUTION_OPTIMIZATION_TYPE_HIGH_QUALITY, "Quality (odd display)", {kSRValidationOddWidth, kSRValidationOddHeight}},
        {RTXPTUpscaleQuality::Native, SUPER_RESOLUTION_OPTIMIZATION_TYPE_NATIVE, "Native", Display},
    };
    for (const QualityCase& Case : Qualities)
    {
        const std::string                    Name = Case.Name;
        SuperResolutionSourceSettingsAttribs Query;
        Query.VariantId        = DLSS.VariantId;
        Query.OutputWidth      = Case.Display.Width;
        Query.OutputHeight     = Case.Display.Height;
        Query.OutputFormat     = RTXPTRenderTargetFormats{}.ProcessedOutputColor;
        Query.OptimizationType = Case.Type;
        SuperResolutionSourceSettings Expected;
        if (!Check(Provider->GetSourceSettings(Query, Expected) == SUPER_RESOLUTION_SOURCE_SETTINGS_STATUS_SUCCESS, Name + ": DLSS does not support the display size"))
            continue;
        auto           Factory = MakeFactory(SRFactoryFault::None);
        RTXPTErrorCode Code    = RTXPTErrorCode::Ok;
        auto           View    = CreateView(RTXPTViewMode::RealtimeDLSSSR, Case.Display, Case.Quality, Factory, Code);
        if (!CheckOk(Code, Name + ": view creation") || !Check(Factory->Upscalers.size() == 1, Name + ": the view must create one feature"))
            continue;
        const SRRecordingUpscaler& Upscaler = *Factory->Upscalers[0];
        const SuperResolutionDesc& Desc     = Upscaler.GetDesc();
        const RTXPTViewSize        RenderSize{Expected.OptimalInputWidth, Expected.OptimalInputHeight};
        Check(SameSize(View->GetRenderSize(), RenderSize) && SameSize(View->GetDisplaySize(), Case.Display), Name + ": the render size is not the query's");
        Check(Desc.VariantId == VariantId_DLSS && Desc.InputWidth == RenderSize.Width && Desc.InputHeight == RenderSize.Height &&
                  Desc.OutputWidth == Case.Display.Width && Desc.OutputHeight == Case.Display.Height && Desc.Flags == SUPER_RESOLUTION_FLAG_NONE &&
                  Desc.OptimizationType == Case.Type,
              Name + ": the feature does not match the query");
        Check(View->GetSuperResolutionInfo().Variant == DLSS.Name && View->GetSuperResolutionInfo().Quality == Case.Quality &&
                  SameSize(View->GetSuperResolutionInfo().RenderSize, RenderSize),
              Name + ": the reported variant, quality or size is wrong");
        for (Uint32 Frame = 0; Frame < kSRValidationFrames; ++Frame)
            if (Prepare())
                CheckOk(Render(*View), Name + ": frame " + std::to_string(Frame));
        const RTXPTViewSuperResolutionInfo& Info = View->GetSuperResolutionInfo();
        Check(Upscaler.Records.size() == kSRValidationFrames && Upscaler.GetAccepted() == kSRValidationFrames, Name + ": one accepted Execute per frame");
        Check(!Upscaler.Records.empty() && Upscaler.Records.front().Reset && Resets(Upscaler) == 1, Name + ": only the first frame may restart history");
        CheckJitterSequence(Upscaler, Name);
        Check(Info.Executes == kSRValidationFrames && Info.Resets == 1 && Info.Failures == 0, Name + ": wrong execute/reset/failure counters");
        const RTXPTViewOutputInfo Output = View->GetOutputInfo(RTXPTViewOutput::Display);
        Check(Output.Valid && SameSize(Output.Size, Case.Display) && Output.FrameIndex == kSRValidationFrames, Name + ": the display output is not published at the display size");
        Check(SameSize(View->GetOutputInfo(RTXPTViewOutput::DenoisedRadiance).Size, RenderSize) &&
                  SameSize(View->GetOutputInfo(RTXPTViewOutput::UpscaledRadiance).Size, Case.Display) &&
                  SameSize(View->GetOutputInfo(RTXPTViewOutput::SuperResolutionDepth).Size, RenderSize) &&
                  SameSize(View->GetOutputInfo(RTXPTViewOutput::SuperResolutionMotion).Size, RenderSize) &&
                  SameSize(View->GetOutputInfo(RTXPTViewOutput::SuperResolutionIgnoreHistoryMask).Size, RenderSize),
              Name + ": inputs must be at the render size and the upscaled HDR at the display size");
        LOG_INFO_MESSAGE("SR validation ", Name, ": display ", Case.Display.Width, 'x', Case.Display.Height, ", render ", RenderSize.Width, 'x', RenderSize.Height,
                         " (min ", Expected.MinInputWidth, 'x', Expected.MinInputHeight, ", max ", Expected.MaxInputWidth, 'x', Expected.MaxInputHeight, ")");
        Report << "DLSS " << Name << ": render " << RenderSize.Width << 'x' << RenderSize.Height << ", display " << Case.Display.Width << 'x' << Case.Display.Height << '\n';
        if (&Case != &Qualities[0])
            continue;
        //The upscaler applies exposure 1 whatever the display exposure, which only tone mapping applies.
        for (const float Exposure : {1.0f, 0.5f, 2.0f})
        {
            if (!Prepare() || !CheckOk(Render(*View, false, Exposure), "exposure frame"))
                continue;
            double     Upscaled = 0, Denoised = 0;
            const bool Finite = MeanLuminance(View->GetOutputInfo(RTXPTViewOutput::UpscaledRadiance).Texture, Upscaled) &&
                MeanLuminance(View->GetOutputInfo(RTXPTViewOutput::DenoisedRadiance).Texture, Denoised);
            const double Ratio = Denoised > 0 ? Upscaled / Denoised : 0;
            LOG_INFO_MESSAGE("SR validation exposure ", Exposure, ": mean luminance denoised ", Denoised, ", upscaled ", Upscaled, ", ratio ", Ratio);
            Check(Finite, "the upscaled or denoised HDR is not finite");
            Check(Denoised > 0 && std::abs(Ratio - 1.0) <= kSRValidationLuminanceTolerance, "the upscaler changed the HDR level");
        }
    }
    {
        RTXPTErrorCode Code = RTXPTErrorCode::Ok;
        CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, nullptr, Code);
        CheckCode(RTXPTErrorCode::SuperResolutionFactoryUnavailable, Code, "no factory");
        const std::tuple<SRFactoryFault, RTXPTErrorCode, const char*> Faults[] = {
            {SRFactoryFault::NoVariants, RTXPTErrorCode::SuperResolutionUnavailable, "no DLSS variant"},
            {SRFactoryFault::Query, RTXPTErrorCode::SuperResolutionQueryFailed, "unsupported quality"},
            {SRFactoryFault::Range, RTXPTErrorCode::SuperResolutionQueryFailed, "query outside its own range"},
            {SRFactoryFault::Create, RTXPTErrorCode::SuperResolutionCreateFailed, "feature creation"},
        };
        for (const auto& [Fault, Expected, Name] : Faults)
        {
            auto Factory = MakeFactory(Fault);
            CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, Code);
            CheckCode(Expected, Code, Name);
            Check(Factory->CreateAttempts == (Fault == SRFactoryFault::Create ? 1u : 0u), std::string{Name} + ": a feature was created for an unsupported query");
        }
        auto Factory = MakeFactory(SRFactoryFault::None);
        CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, kSRValidationUnknownQuality, Factory, Code);
        CheckCode(RTXPTErrorCode::SuperResolutionQueryFailed, Code, "unknown quality");
        Check(Factory->Queries == 0 && Factory->CreateAttempts == 0, "unknown quality: queried or created");
        CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, Code, Display);
        CheckCode(RTXPTErrorCode::UnsupportedViewConfiguration, Code, "a host render size");
        auto View = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, Code);
        if (CheckOk(Code, "jitter view") && Prepare())
        {
            CheckCode(RTXPTErrorCode::InvalidArgument, Render(*View, false, 1.0f, float2{0.25f, 0}), "host jitter");
            Check(!View->GetOutputInfo(RTXPTViewOutput::Display).Valid && Factory->Upscalers.back()->Records.empty(), "host jitter: executed or published");
        }
    }

    LOG_INFO_MESSAGE("SR validation S06: history across motion, cuts, missing frames and configuration changes");
    {
        auto           Factory = MakeFactory(SRFactoryFault::None);
        RTXPTErrorCode Code    = RTXPTErrorCode::Ok;
        auto           View    = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, Code);
        if (CheckOk(Code, "S06 view"))
        {
            const SRRecordingUpscaler& Upscaler = *Factory->Upscalers[0];
            const auto                 Step     = [&](bool Reset, bool ExpectReset, const std::string& What) {
                if (!Prepare() || !CheckOk(Render(*View, Reset), What))
                    return;
                Check(Upscaler.Records.back().Reset == ExpectReset && View->GetRealtimeStats().LastHistoryReset == ExpectReset &&
                          View->GetSuperResolutionInfo().LastReset == ExpectReset,
                      What + (ExpectReset ? ": DLSS and NRD history must restart" : ": DLSS and NRD history must continue"));
            };
            Step(false, true, "first frame");
            Step(false, false, "static frame");
            Step(false, false, "static frame");
            for (Uint32 Frame = 0; Frame < 4; ++Frame)
            {
                Camera.SetPos(Camera.GetPos() + float3{0.01f, 0, 0});
                Step(false, false, "camera motion");
            }
            Step(true, true, "cut");
            Step(false, false, "after the cut");
            Prepare();
            Step(false, true, "after a missed host frame");
            Step(false, false, "after the recovery");
            CheckJitterSequence(Upscaler, "S06");
            Check(View->GetOutputInfo(RTXPTViewOutput::Display).FrameIndex == Upscaler.GetAccepted(), "S06: successful frames and the jitter sequence diverged");
        }
        Camera = m_Camera;
        //Size, quality and mode changes create a new view with a new feature and history.
        auto Switched = CreateView(RTXPTViewMode::RealtimeDLSSSR, {kSRValidationOddWidth, kSRValidationOddHeight}, RTXPTUpscaleQuality::Balanced, Factory, Code);
        if (CheckOk(Code, "S06 switched view") && Prepare() && CheckOk(Render(*Switched), "S06 switched frame"))
        {
            const SRRecordingUpscaler& Upscaler = *Factory->Upscalers.back();
            Check(Factory->Upscalers.size() == 2 && Upscaler.Records.size() == 1 && Upscaler.Records[0].Reset && Upscaler.Records[0].Jitter == Upscaler.GetJitter(0),
                  "S06: a new configuration must start a new feature history at jitter index 0");
        }
        auto Native = CreateView(RTXPTViewMode::RealtimeNative, Display, RTXPTUpscaleQuality::Quality, nullptr, Code);
        if (CheckOk(Code, "S06 Native view") && Prepare() && CheckOk(Render(*Native), "S06 Native frame"))
            Check(Native->GetRealtimeStats().SuperResolutionExecutes == 0 && Native->GetOutputInfo(RTXPTViewOutput::UpscaledRadiance).Texture == nullptr &&
                      SameSize(Native->GetRenderSize(), Display),
                  "S06: Native must not upscale");
    }

    LOG_INFO_MESSAGE("SR validation S07: independent views of one scene");
    {
        auto           Factory = MakeFactory(SRFactoryFault::None);
        RTXPTErrorCode CodeA = RTXPTErrorCode::Ok, CodeB = RTXPTErrorCode::Ok, CodeN = RTXPTErrorCode::Ok;
        auto           A = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, CodeA);
        auto           B = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Performance, Factory, CodeB);
        auto           N = CreateView(RTXPTViewMode::RealtimeNative, {kSRValidationWidth / 2, kSRValidationHeight / 2}, RTXPTUpscaleQuality::Quality, nullptr, CodeN);
        if (CheckOk(CodeA, "S07 view A") && CheckOk(CodeB, "S07 view B") && CheckOk(CodeN, "S07 Native view") &&
            Check(Factory->Upscalers.size() == 2, "S07: each view must own a feature"))
        {
            const Uint64 PreparesBefore = Scene.GetFrameStats().PrepareCount;
            for (Uint32 Frame = 0; Frame < 4; ++Frame)
                if (Prepare())
                    CheckOk(Render(*A), "S07 A") && CheckOk(Render(*B), "S07 B") && CheckOk(Render(*N), "S07 N");
            if (Prepare())
                CheckOk(Render(*A), "S07 A while B misses") && CheckOk(Render(*N), "S07 N while B misses");
            if (Prepare())
                CheckOk(Render(*B), "S07 B first") && CheckOk(Render(*A), "S07 A second") && CheckOk(Render(*N), "S07 N third");
            const SRRecordingUpscaler& UA = *Factory->Upscalers[0];
            const SRRecordingUpscaler& UB = *Factory->Upscalers[1];
            Check(Scene.GetFrameStats().PrepareCount - PreparesBefore == 6, "S07: the scene must be prepared once per host frame");
            Check(UA.Records.size() == 6 && UB.Records.size() == 5, "S07: each view must execute its own feature");
            Check(Resets(UA) == 1 && UA.Records.front().Reset, "S07: A must keep its history when B misses a frame or the order changes");
            Check(Resets(UB) == 2 && UB.Records.front().Reset && UB.Records.back().Reset, "S07: B must restart only after its missed frame");
            CheckJitterSequence(UA, "S07 A");
            CheckJitterSequence(UB, "S07 B");
            Check(N->GetRealtimeStats().HistoryResets == 1, "S07: the Native neighbour must keep its history");
            Check(!SameSize(A->GetRenderSize(), B->GetRenderSize()), "S07: Quality and Performance must render at their own sizes");
        }
    }

    LOG_INFO_MESSAGE("SR validation S08: injected upscaler failures");
    {
        auto           Factory = MakeFactory(SRFactoryFault::None);
        RTXPTErrorCode CodeA = RTXPTErrorCode::Ok, CodeC = RTXPTErrorCode::Ok;
        auto           A = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, CodeA);
        auto           C = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, CodeC);
        if (CheckOk(CodeA, "S08 view") && CheckOk(CodeC, "S08 neighbour") && Factory->Upscalers.size() == 2)
        {
            SRRecordingUpscaler&       UA = *Factory->Upscalers[0];
            const SRRecordingUpscaler& UC = *Factory->Upscalers[1];
            UA.FailNext                   = 1;
            if (Prepare())
            {
                CheckCode(RTXPTErrorCode::SuperResolutionCreateFailed, Render(*A), "S08 first Execute failure");
                CheckOk(Render(*C), "S08 neighbour");
            }
            Check(!A->GetOutputInfo(RTXPTViewOutput::Display).Valid && A->GetOutputInfo(RTXPTViewOutput::Display).Texture == nullptr &&
                      A->GetSuperResolutionInfo().Failures == 1 && A->GetSuperResolutionInfo().Executes == 0,
                  "S08: a failed first frame must not be published or counted");
            if (Prepare())
            {
                CheckOk(Render(*A), "S08 retry");
                CheckOk(Render(*C), "S08 neighbour");
            }
            Check(UA.Records.size() == 2 && UA.Records[1].Reset && UA.Records[1].Jitter == UA.Records[0].Jitter, "S08: the retry must restart with the failed frame's jitter index");
            for (Uint32 Frame = 0; Frame < 3; ++Frame)
                if (Prepare())
                    CheckOk(Render(*A), "S08 A") && CheckOk(Render(*C), "S08 neighbour");
            const Uint32 FramesBefore = A->GetOutputInfo(RTXPTViewOutput::Display).FrameIndex;
            UA.FailNext               = 1;
            if (Prepare())
            {
                CheckCode(RTXPTErrorCode::SuperResolutionExecuteFailed, Render(*A), "S08 evaluation failure");
                CheckOk(Render(*C), "S08 neighbour during the failure");
            }
            const RTXPTViewOutputInfo Failed = A->GetOutputInfo(RTXPTViewOutput::Display);
            Check(!Failed.Valid && Failed.Texture == nullptr && Failed.FrameIndex == 0 && A->GetRealtimeLayerBuffer() == nullptr &&
                      !A->GetOutputInfo(RTXPTViewOutput::UpscaledRadiance).Valid,
                  "S08: a failed frame must not publish any output");
            Check(A->GetSuperResolutionInfo().Failures == 2 && A->GetSuperResolutionInfo().Executes == FramesBefore, "S08: a failure must not count as a success");
            if (Prepare())
            {
                CheckOk(Render(*A), "S08 recovery");
                CheckOk(Render(*C), "S08 neighbour after the failure");
            }
            Check(UA.Records.size() >= 2 && UA.Records.back().Reset && A->GetRealtimeStats().LastHistoryReset &&
                      A->GetOutputInfo(RTXPTViewOutput::Display).FrameIndex == FramesBefore + 1,
                  "S08: the recovery must restart NRD and DLSS history");
            Check(UA.Records.size() >= 2 && UA.Records.back().Jitter == UA.Records[UA.Records.size() - 2].Jitter, "S08: the recovery must reuse the failed frame's jitter index");
            Check(Resets(UC) == 1 && UC.GetAccepted() == UC.Records.size(), "S08: the neighbour must keep its history");
            CheckJitterSequence(UA, "S08 A");
            CheckJitterSequence(UC, "S08 neighbour");
        }
    }
    LOG_INFO_MESSAGE("SR validation: creation retries retain Auto selection");
    {
        auto                        Factory = MakeFactory(SRFactoryFault::Create);
        RTXPTViewUpscaler           Upscaler;
        RTXPTViewUpscalerCreateInfo CI;
        CI.Factory     = Factory;
        CI.DisplaySize = Display;
        CheckCode(RTXPTErrorCode::SuperResolutionCreateFailed, Upscaler.Initialize(CI), "cached Auto create failure");
        const auto Queries    = Factory->Queries;
        const auto RenderSize = Upscaler.GetInfo().RenderSize;
        Check(Upscaler.HasSelection() && !Upscaler.IsReady() && Upscaler.GetInfo().Failures == 1, "A failed creation retains its exact selected query");
        Factory->SetFault(SRFactoryFault::None);
        CheckOk(Upscaler.RetryCreate(), "cached Auto creation retry");
        Check(Queries == Factory->Queries && Upscaler.GetInfo().ActualProvider == SUPER_RESOLUTION_PROVIDER_DLSS && SameSize(RenderSize, Upscaler.GetInfo().RenderSize),
              "Creation recovery must not reevaluate Auto or change input dimensions");
    }
    {
        auto                        Factory = MakeFactory(SRFactoryFault::Create);
        RTXPTErrorCode              Code{};
        RTXPTEmbeddedViewCreateInfo CI;
        auto                        View = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, Code, {}, RTXPTSuperResolutionProvider::Auto, false, &CI);
        CheckCode(RTXPTErrorCode::SuperResolutionCreateFailed, Code, "embedded Auto create failure");
        const auto Queries = Factory->Queries;
        Factory->SetFault(SRFactoryFault::None);
        if (CheckOk(View->RetryInitialize(m_pDevice, m_pImmediateContext, CI), "embedded Auto creation retry") && Prepare())
        {
            CheckOk(Render(*View), "embedded Auto recovered frame");
            const auto& Info = View->GetSuperResolutionInfo();
            Check(Queries == Factory->Queries && Info.Failures == 1 && Info.Executes == 1 && Info.LastReset && Info.ActualProvider == SUPER_RESOLUTION_PROVIDER_DLSS,
                  "Embedded creation recovery preserves selection, failure count and reset");
        }
    }
    LOG_INFO_MESSAGE("SR validation: whole host frame commit and submission failure");
    {
        auto           Factory = MakeFactory(SRFactoryFault::None);
        RTXPTErrorCode Code{};
        auto           View = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, Code, {}, RTXPTSuperResolutionProvider::DLSS, true);
        if (CheckOk(Code, "deferred view") && !Factory->Upscalers.empty())
        {
            auto& Upscaler = *Factory->Upscalers.back();
            if (Prepare())
                CheckOk(Render(*View), "deferred first frame");
            View->InvalidateOutput();
            if (Prepare())
                CheckOk(Render(*View), "deferred submission retry");
            Check(Upscaler.Records.size() == 2 && Upscaler.Records[0].Jitter == Upscaler.Records[1].Jitter && Upscaler.Records[1].Reset &&
                      View->GetOutputInfo(RTXPTViewOutput::Display).FrameIndex == 1,
                  "A rejected host frame must reset and reuse its successful-frame index and jitter");
            View->CommitFrame();
            View->CommitFrame();
            if (Prepare())
                CheckOk(Render(*View), "deferred committed successor");
            Check(Upscaler.Records.size() == 3 && Upscaler.Records.back().Jitter == Upscaler.GetJitter(1) && !Upscaler.Records.back().Reset &&
                      View->GetOutputInfo(RTXPTViewOutput::Display).FrameIndex == 2,
                  "Only the first host commit advances the sequence; its successor retains history");
            View->CommitFrame();
        }
    }
    LOG_INFO_MESSAGE("SR provider validation: Auto preference and DirectSR temporal execution");
    {
        RTXPTErrorCode Code = RTXPTErrorCode::Ok;
        auto           Auto = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Provider, Code, {}, RTXPTSuperResolutionProvider::Auto);
        if (CheckOk(Code, "Auto DLSS view"))
            Check(Auto->GetSuperResolutionInfo().ActualProvider == SUPER_RESOLUTION_PROVIDER_DLSS, "Auto must prefer DLSS");
        Uint32 Count = 0;
        Provider->EnumerateVariants(Count, nullptr);
        std::vector<SuperResolutionInfo> Variants(Count);
        Provider->EnumerateVariants(Count, Variants.data());
        Variants.resize(Count);
        const auto DirectSR = std::find_if(Variants.begin(), Variants.end(), [](const auto& Variant) {
            return Variant.Provider == SUPER_RESOLUTION_PROVIDER_DIRECTSR && Variant.Type == SUPER_RESOLUTION_TYPE_TEMPORAL;
        });
        auto       Direct   = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Provider, Code, {}, RTXPTSuperResolutionProvider::DirectSR);
        if (DirectSR == Variants.end())
        {
            Report << "DirectSR: unavailable (negative case)\n";
            CheckCode(RTXPTErrorCode::DirectSRUnavailable, Code, "DirectSR unavailable on this device");
        }
        else if (CheckOk(Code, "DirectSR view"))
        {
            Report << "DirectSR: " << Direct->GetSuperResolutionInfo().Variant << '\n';
            Check(Direct->GetSuperResolutionInfo().ActualProvider == SUPER_RESOLUTION_PROVIDER_DIRECTSR && Direct->GetSuperResolutionInfo().Variant == DirectSR->Name,
                  "DirectSR must select its first temporal variant");
            for (Uint32 Frame = 0; Frame < kSRValidationFrames; ++Frame)
                if (Prepare())
                    CheckOk(Render(*Direct), "DirectSR frame");
            Check(Direct->GetSuperResolutionInfo().Executes == kSRValidationFrames && Direct->GetSuperResolutionInfo().Resets == 1, "DirectSR must preserve history");
            CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Native, Provider, Code, {}, RTXPTSuperResolutionProvider::DirectSR);
            CheckCode(RTXPTErrorCode::SuperResolutionQueryFailed, Code, "DirectSR Native is unsupported");
        }
    }
    LOG_INFO_MESSAGE("SR provider validation: spatial FSR, Auto and explicit provider isolation");
    {
        RefCntAutoPtr<SRTestFactory> Factory{MakeNewRCObj<SRTestFactory>()(Provider.RawPtr(), SRFactoryFault::None, SUPER_RESOLUTION_PROVIDER_FSR)};
        for (auto Requested : {RTXPTSuperResolutionProvider::FSR, RTXPTSuperResolutionProvider::Auto})
        {
            RTXPTErrorCode Code = RTXPTErrorCode::Ok;
            auto           View = CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, Code, {}, Requested);
            if (!CheckOk(Code, "FSR view creation"))
                continue;
            Report << GetRTXPTSuperResolutionProviderString(Requested) << ": " << View->GetSuperResolutionInfo().Variant << '\n';
            const auto  Queries  = Factory->Queries;
            const auto& Upscaler = *Factory->Upscalers.back();
            Check(Upscaler.GetDesc().DepthFormat == TEX_FORMAT_UNKNOWN && Upscaler.GetDesc().MotionFormat == TEX_FORMAT_UNKNOWN,
                  "spatial features must not declare depth or motion");
            Check(View->GetSuperResolutionInfo().ActualProvider == SUPER_RESOLUTION_PROVIDER_FSR, "Auto must select the available FSR provider");
            for (Uint32 Frame = 0; Frame < kSRValidationFrames; ++Frame)
                if (Prepare())
                    CheckOk(Render(*View), "FSR frame");
            Check(Queries == Factory->Queries, "Auto must not query or select a provider each frame");
            Check(Upscaler.Records.size() == kSRValidationFrames && Upscaler.GetAccepted() == kSRValidationFrames, "FSR must execute once per frame");
            for (const auto& Record : Upscaler.Records)
                Check(Record.Jitter == float2{0, 0} && !Record.TemporalInputs && Record.OutputType == TEXTURE_VIEW_RENDER_TARGET,
                      "FSR must receive zero jitter, no temporal inputs and an RTV output");
            Check(View->GetOutputInfo(RTXPTViewOutput::SuperResolutionDepth).Texture == nullptr, "spatial views must not prepare temporal SR inputs");
            double Mean = 0;
            Check(MeanLuminance(View->GetOutputInfo(RTXPTViewOutput::UpscaledRadiance).Texture, Mean) && Mean > 0, "FSR must publish finite scene-linear HDR");
        }
        const auto     Creates = Factory->CreateAttempts;
        RTXPTErrorCode Code    = RTXPTErrorCode::Ok;
        CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Native, Factory, Code, {}, RTXPTSuperResolutionProvider::FSR);
        CheckCode(RTXPTErrorCode::SuperResolutionQueryFailed, Code, "FSR Native is unsupported");
        CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Native, Factory, Code, {}, RTXPTSuperResolutionProvider::Auto);
        CheckCode(RTXPTErrorCode::SuperResolutionQueryFailed, Code, "Auto Native cannot substitute Quality");
        CreateView(RTXPTViewMode::RealtimeDLSSSR, Display, RTXPTUpscaleQuality::Quality, Factory, Code, {}, RTXPTSuperResolutionProvider::DLSS);
        CheckCode(RTXPTErrorCode::SuperResolutionUnavailable, Code, "explicit DLSS must not fall back to FSR");
        Check(Creates == Factory->CreateAttempts, "unsupported or missing providers must not create a feature");
    }
    m_pImmediateContext->WaitForIdle();
    LOG_INFO_MESSAGE("SR scene validation finished: failures=", Failures);
    Report << "failures=" << Failures << '\n';
    std::fflush(stdout);
}
