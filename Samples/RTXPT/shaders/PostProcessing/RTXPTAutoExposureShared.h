#ifndef __RTXPT_AUTO_EXPOSURE_SHARED_H__
#define __RTXPT_AUTO_EXPOSURE_SHARED_H__

// Exposures are log2 of the linear multiplier tone mapping applies (higher is brighter), not photographic EV100.
// The luminance and its black floor are those of the Sample's metering (ToneMapper/Luminance.psh).
#define RTXPT_AUTO_EXPOSURE_LUMINANCE_WEIGHTS float3(0.299, 0.587, 0.114)
#define RTXPT_AUTO_EXPOSURE_MIN_LUMINANCE     0.0001

// MeterLuminance sums one tile of this many pixels per thread group; Adapt reduces the tiles with one group.
#define RTXPT_AUTO_EXPOSURE_TILE_SIZE    16
#define RTXPT_AUTO_EXPOSURE_REDUCE_GROUP 256

// The state texture (RGBA32F, RTXPT_AUTO_EXPOSURE_STATE_TEXELS x 1): texel 0 holds the base exposure (without the
// compensation), whether a meter has initialized it since its seed, the target base exposure of that meter and the
// RTXPT_AUTO_EXPOSURE_EVENT_* of the frame; texel 1 the mean log2 luminance and valid samples of the meter, the
// adaptation weight and the exposure the frame used.
#define RTXPT_AUTO_EXPOSURE_STATE_TEXELS 2

#define RTXPT_AUTO_EXPOSURE_EVENT_ADAPTED     0
#define RTXPT_AUTO_EXPOSURE_EVENT_INITIALIZED 1
#define RTXPT_AUTO_EXPOSURE_EVENT_NO_SAMPLES  2
#define RTXPT_AUTO_EXPOSURE_EVENT_TIME_HELD   3

struct RTXPTAutoExposureConstants
{
    float SeedEV;
    float CompensationEV;
    float MinEV;
    float MaxEV;

    float Log2Key;
    float BrightenSeconds;
    float DarkenSeconds;
    float DeltaSeconds;

    // Nonzero: the frame starts from SeedEV, uninitialized, instead of the committed state.
    uint Seed;
    // Nonzero: DeltaSeconds advances the adaptation; zero holds it.
    uint ValidDelta;
    uint Width;
    uint Height;

    uint TilesX;
    uint TileCount;
    uint _Padding0;
    uint _Padding1;
};

// The sum of the log2 luminances of the valid pixels of a tile, and their count.
struct RTXPTAutoExposureTile
{
    float Sum;
    uint  Count;
};

#endif // __RTXPT_AUTO_EXPOSURE_SHARED_H__
