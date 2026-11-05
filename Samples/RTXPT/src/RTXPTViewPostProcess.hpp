#pragma once

#include "RTXPTAutoExposure.hpp"
#include "RTXPTBloomPass.hpp"
#include "RTXPTDisplayBloom.hpp"
#include "RTXPTGpuProfiler.hpp"
#include "RTXPTSceneRuntime.hpp"
#include "RTXPTToneMappingPass.hpp"

namespace Diligent
{
struct IRenderStateCache;
struct IPipelineStateCache;

//The display exposure range of the tone mapper, [-32, 32] stops, and the range of its white points. At 1/64 the
//Uncharted 2 white normalization still divides by about 0.0056 (its toe cancels less than 3 bits) and ReinhardModified
//by White^2 = 2^-12; at 65536 neither curve differs visibly from its infinite-white limit.
constexpr float RTXPTMinDisplayExposure  = 1.0f / 4294967296.0f;
constexpr float RTXPTMaxDisplayExposure  = 4294967296.0f;
constexpr float RTXPTMinToneMappingWhite = 1.0f / 64.0f;
constexpr float RTXPTMaxToneMappingWhite = 65536.0f;
//The Bloom range of the Sample's controls: a radius of [0, 64] display pixels, whose quarter clamped to [1, 100] is the
//sigma of the blur, and an intensity of [0, 0.1], the weight of the blurred image in the mix.
constexpr float RTXPTMaxBloomRadius    = 64.0f;
constexpr float RTXPTMaxBloomIntensity = 0.1f;

//The display transform of one frame of a view. Exposure is the linear multiplier tone mapping applies to the
//scene-linear HDR before ToneMapOperator, and the exposure ray reconstruction and the realtime firefly and brightness
//clamps scale with. WhiteMaxLuminance is the white point of ReinhardModified and WhiteScale that of HableUc2; each
//operator ignores the other's. Bloom is the Sample's Bloom (RTXPTBloomPass) on the display-size HDR before tone mapping:
//(1 - Intensity) * HDR + Intensity * HDR blurred at a quarter of the display width and height. It is off by default,
//unlike the Sample's controls; a zero radius or intensity bypasses it as Enabled = false does. AutoExposure, when
//enabled, replaces Exposure: tone mapping and ray reconstruction read the exposure the view's RTXPTAutoExposure adapts on
//the GPU, metered from the HDR before Bloom, and the realtime clamps use RTXPTAutoExposureTraceClampExposure. Values
//outside the ranges above (IsValidRTXPTAutoExposure for AutoExposure, whatever Enabled) or an unknown operator are
//rejected instead of clamped (IsValidRTXPTViewDisplaySettings). None of them rescales or blurs the HDR they read, and
//changing them restarts no history.
struct RTXPTViewDisplaySettings
{
    float                       Exposure          = 1.0f;
    RTXPTToneMapperOperator     ToneMapOperator   = RTXPTToneMapperOperator::Reinhard;
    float                       WhiteMaxLuminance = 1.0f;
    float                       WhiteScale        = 5.1f;
    RTXPTBloomParameters        Bloom{false};
    RTXPTAutoExposureParameters AutoExposure;
};

//Whether Display is within the ranges above with a known operator.
bool IsValidRTXPTViewDisplaySettings(const RTXPTViewDisplaySettings& Display);

//The tone mapping parameters of Display: manual exposure with the default camera (ISO 100, f/1, 1 s, a factor of 1), so
//ExposureCompensation = log2(Exposure) is the only exposure the tone mapper, ray reconstruction and the realtime clamps
//derive. With auto exposure the exposure is resolved on the GPU instead, and ExposureCompensation is 0. Display must be
//valid.
RTXPTToneMappingParameters MakeRTXPTToneMappingParameters(const RTXPTViewDisplaySettings& Display);

//Creates the display target of a view at Width x Height: an RGBA8 texture whose sRGB RTV (*RTV) encodes the tone mapped
//colors once and whose default SRV, UNORM, copies the encoded bytes without decoding them again.
bool CreateRTXPTDisplayTarget(IRenderDevice* Device, Uint32 Width, Uint32 Height, const char* Name, ITexture** Texture, ITextureView** RTV);

/* The display post processing of one view at one display size, which every embedded mode and a host's own HDR share, in
 * one order:
 *  1. The view's auto exposure, when the frame uses it, meters the scene-linear HDR (RTXPTAutoExposure::Meter).
 *  2. The display Bloom blurs and mixes a copy of it (RTXPTDisplayBloom); the HDR itself is left as it is.
 *  3. Tone mapping writes the display RTV, with the exposure the frame's auto exposure resolved or the manual exposure
 *     of the tone mapping parameters, encoding to sRGB once on the RTV.
 * The HDR is display-size kRTXPTDisplayHDRFormat, neither tone mapped nor encoded. The resources of Bloom are created by
 * its first frame and kept for the size; the tone mapper's once. Use on one ordered context.
 */
class RTXPTViewPostProcess
{
public:
    //Creates the tone mapper with the shader source of the calling scope (RTXPTShaderSourceScope) for a DisplayFormat
    //RTV. Width and Height are the display size.
    bool Initialize(IRenderDevice* Device, IRenderStateCache* Cache, IPipelineStateCache* PSOCache, Uint32 Width, Uint32 Height, TEXTURE_FORMAT DisplayFormat);
    //Steps 1 to 3 for a frame whose auto exposure, AutoExposure when not null, has begun (RTXPTAutoExposure::BeginFrame).
    //ToneParameters must request no auto exposure of the tone mapper. Fails with InvalidArgument for an HDR or RTV of
    //another size or format, with the meter's or Bloom's error, or with ToneMappingFailed.
    RTXPTErrorCode Apply(IDeviceContext* Context, ITexture* HDR, const RTXPTToneMappingParameters& ToneParameters, const RTXPTBloomParameters& Bloom, RTXPTAutoExposure* AutoExposure, ITextureView* DisplayRTV, RTXPTGpuProfiler* Profiler = nullptr);
    //The whole frame of a host that produces the HDR itself: checks Display (InvalidArgument, also for auto exposure
    //without a ready AutoExposure), begins the frame of AutoExposure at Time when Display enables it, and applies
    //Display. A frame that fails after it began aborts it; the host commits a successful one (RTXPTAutoExposure::Commit).
    RTXPTErrorCode Render(IDeviceContext* Context, ITexture* HDR, const RTXPTViewDisplaySettings& Display, RTXPTAutoExposure* AutoExposure, double Time, ITextureView* DisplayRTV, RTXPTGpuProfiler* Profiler = nullptr);
    bool           IsReady() const { return m_Ready; }
    Uint32         GetWidth() const { return m_Width; }
    Uint32         GetHeight() const { return m_Height; }
    //The display Bloom of the latest frame (RTXPTDisplayBloom::GetOutput) and since Initialize.
    const RTXPTDisplayBloom& GetBloom() const { return m_Bloom; }
    //The tone mapper, whose exposure the realtime clamps and ray reconstruction derive with it.
    const RTXPTToneMappingPass& GetToneMapping() const { return m_ToneMapping; }

private:
    RTXPTDisplayBloom    m_Bloom;
    RTXPTToneMappingPass m_ToneMapping;
    TEXTURE_FORMAT       m_DisplayFormat = TEX_FORMAT_UNKNOWN;
    Uint32               m_Width         = 0;
    Uint32               m_Height        = 0;
    bool                 m_Ready         = false;
};
} // namespace Diligent
