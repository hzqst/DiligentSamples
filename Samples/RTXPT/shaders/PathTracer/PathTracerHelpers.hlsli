#ifndef __PATH_TRACER_HELPERS_HLSLI__
#define __PATH_TRACER_HELPERS_HLSLI__

#include "PathTracerShared.h"

#ifndef HLF_MAX
#    define HLF_MAX 6.5504e+4F
#endif

#ifndef kMaxSceneDistance
#    define kMaxSceneDistance 50000.0
#endif

#ifndef kEnvironmentMapSceneDistance
#    define kEnvironmentMapSceneDistance (kMaxSceneDistance * 100.0)
#endif

#ifndef kMaxRayTravel
#    define kMaxRayTravel (1e15f)
#endif

#ifndef cDenoisingLayerCount
#    define cDenoisingLayerCount (3u)
#endif

#ifndef PT_USE_RESTIR_DI
#    define PT_USE_RESTIR_DI 0
#endif

#ifndef PT_USE_RESTIR_GI
#    define PT_USE_RESTIR_GI 0
#endif

#ifndef RTXPT_USE_APPROXIMATE_MIS
#    define RTXPT_USE_APPROXIMATE_MIS 0
#endif

#ifndef RTXPT_NESTED_DIELECTRICS_QUALITY
#    define RTXPT_NESTED_DIELECTRICS_QUALITY 1
#endif

#ifndef RTXPT_LP_TYPES_USE_16BIT_PRECISION
#    define RTXPT_LP_TYPES_USE_16BIT_PRECISION 0
#endif

#if RTXPT_LP_TYPES_USE_16BIT_PRECISION
typedef float16_t    lpfloat;
typedef float16_t2   lpfloat2;
typedef float16_t3   lpfloat3;
typedef float16_t4   lpfloat4;
typedef float16_t3x3 lpfloat3x3;
typedef uint16_t     lpuint;
typedef uint16_t2    lpuint2;
typedef uint16_t3    lpuint3;
typedef uint16_t4    lpuint4;
#else
typedef float    lpfloat;
typedef float2   lpfloat2;
typedef float3   lpfloat3;
typedef float4   lpfloat4;
typedef float3x3 lpfloat3x3;
typedef uint     lpuint;
typedef uint2    lpuint2;
typedef uint3    lpuint3;
typedef uint4    lpuint4;
#endif

float3 ComputeRayOrigin(float3 worldPosition, float3 faceNormal, float positionErrorScale)
{
    // A single error magnitude avoids sliding sideways across triangle creases.
    const float kOrigin = 1.0 / 1024.0;
    const float kOffsetScale = 4.0 / 8388608.0;
    return worldPosition + faceNormal * (max(kOrigin, positionErrorScale) * kOffsetScale);
}

float3 ComputeRayOrigin(float3 worldPosition, float3 faceNormal)
{
    return ComputeRayOrigin(worldPosition, faceNormal, max(abs(worldPosition.x), max(abs(worldPosition.y), abs(worldPosition.z))));
}

float ComputeLowGrazingAngleFalloff(float3 lightDirection, float3 interpolatedGeometryNormal, float falloffFrom, float falloffRange)
{
    return saturate((dot(lightDirection, interpolatedGeometryNormal) - falloffFrom) / max(falloffRange, 1e-6));
}

struct CameraRay
{
    float3 origin;
    float3 dir;
    float  tMin;
    float  tMax;
};

float2 SampleConcentricDisk(float2 sample)
{
    const float2 p = 2.0 * sample - 1.0;
    if (dot(p, p) == 0.0)
        return float2(0.0, 0.0);

    float r;
    float theta;
    if (abs(p.x) > abs(p.y))
    {
        r     = p.x;
        theta = 0.7853981633974483 * (p.y / p.x);
    }
    else
    {
        r     = p.y;
        theta = 1.5707963267948966 - 0.7853981633974483 * (p.x / p.y);
    }

    return r * float2(cos(theta), sin(theta));
}

float3 ComputeNonNormalizedRayDirPinhole(PathTracerCameraData data, uint2 pixel, float2 jitter)
{
    const float2 p   = (float2(pixel) + float2(0.5, 0.5) + float2(-jitter.x, jitter.y)) / float2(data.ViewportSize);
    const float2 ndc = float2(2.0, -2.0) * p + float2(-1.0, 1.0);
    return ndc.x * data.CameraU + ndc.y * data.CameraV + data.CameraW;
}

CameraRay ComputeRayThinlens(PathTracerCameraData data, uint2 pixel, float2 jitter, float2 sample2D)
{
    CameraRay ray;
    ray.origin = data.PosW;

    ray.dir = ComputeNonNormalizedRayDirPinhole(data, pixel, jitter);

    const float2 apertureSample = SampleConcentricDisk(sample2D);
    const float3 target         = ray.origin + ray.dir;
    if (data.ApertureRadius > 0.0)
    {
        ray.origin += data.ApertureRadius *
            (apertureSample.x * normalize(data.CameraU) + apertureSample.y * normalize(data.CameraV));
    }
    ray.dir = normalize(target - ray.origin);

    const float invCos = 1.0 / max(dot(normalize(data.CameraW), ray.dir), 1e-6);
    ray.tMin = data.NearZ * invCos;
    ray.tMax = data.FarZ * invCos;

    ray.origin += ray.dir * ray.tMin;
    ray.tMax   = max(ray.tMax - ray.tMin, 0.0);
    ray.tMin   = 0.0;
    return ray;
}

// Power heuristic for MIS (Veach). Matches RTXPT-fork PathTracerHelpers.hlsli signature.
float PowerHeuristic(float nf, float fPdf, float ng, float gPdf)
{
    const float f  = nf * fPdf;
    const float g  = ng * gPdf;
    const float f2 = f * f;
    const float g2 = g * g;
    return f2 / max(f2 + g2, 1e-7);
}

// Mean of the RGB channels. RTXPT uses Average() (not luminance) for the firefly soft cap to
// avoid a hue shift toward blue when clamping. (Self-contained: pi literals avoid coupling to
// BxDF.hlsli's K_PI, which is included after this header.)
float Average(float3 v)
{
    return (v.x + v.y + v.z) * 0.3333333333333333;
}

#if RTXPT_LP_TYPES_USE_16BIT_PRECISION
lpfloat Average(lpfloat3 v)
{
    return (v.x + v.y + v.z) / 3.0;
}
#endif

// Ray-cone spread angle implied by a scatter pdf (RTXPT ComputeRayConeSpreadAngleExpansionByScatterPDF,
// growthFactor folded to 1.0). A delta lobe (pdf==0 sentinel) has zero spread.
float ComputeRayConeSpreadAngleExpansionByScatterPDF(float bsdfScatterPdf)
{
    const float twoPi = 6.28318530717958647692;
    return 2.0 * acos(max(-1.0, 1.0 - (1.0 / bsdfScatterPdf) / twoPi));
}

// Adaptive firefly-filter K (RTXPT ComputeNewScatterFireflyFilterK): K shrinks as the path
// spreads. currentK is the path's running K, bouncePdf the scatter pdf (0 == delta event),
// lobeP the probability of the lobe that was sampled.
float ComputeNewScatterFireflyFilterK(float currentK, float bouncePdf, float lobeP)
{
    const float minK  = 0.00001;
    const float angle = (bouncePdf == 0.0) ? 0.0 : ComputeRayConeSpreadAngleExpansionByScatterPDF(bouncePdf);
    const float k     = 32.0; // empirical
    float       p     = k / (k + angle * angle);
    p *= sqrt(lobeP); // sqrt behaves better empirically
    return max(minK, currentK * p);
}

// Soft-cap a vector signal to threshold*K of its own average (RTXPT FireflyFilter).
float3 FireflyFilter(float3 signalIn, float threshold, float fireflyFilterK)
{
    const float fft  = threshold * fireflyFilterK;
    const float maxR = Average(signalIn);
    if (maxR > fft)
        signalIn = signalIn / maxR * fft;
    return signalIn;
}

// Scalar dampening factor for the same soft cap (RTXPT FireflyFilterShort); multiply a radiance by it.
float FireflyFilterShort(float signalAverage, float threshold, float fireflyFilterK)
{
    const float fft = threshold * fireflyFilterK;
    return (signalAverage > fft) ? (fft / signalAverage) : 1.0;
}

struct Ray
{
    float3 origin;
    float  tMin;
    float3 dir;
    float  tMax;

    static Ray make(float3 origin, float3 dir, float tMin = 0.0, float tMax = kMaxRayTravel)
    {
        Ray ray;
        ray.origin = origin;
        ray.dir    = dir;
        ray.tMin   = tMin;
        ray.tMax   = tMax;
        return ray;
    }

    RayDesc toRayDesc()
    {
        RayDesc rayDesc;
        rayDesc.Origin    = origin;
        rayDesc.TMin      = tMin;
        rayDesc.Direction = dir;
        rayDesc.TMax      = tMax;
        return rayDesc;
    }
};

struct RayCone
{
    uint widthSpreadAngleFP16;

    static RayCone make(float width, float spreadAngle)
    {
        RayCone cone;
        cone.widthSpreadAngleFP16 = (f32tof16(width) << 16) | f32tof16(spreadAngle);
        return cone;
    }

    float getWidth() { return f16tof32(widthSpreadAngleFP16 >> 16); }
    float getSpreadAngle() { return f16tof32(widthSpreadAngleFP16 & 0xffff); }
};

uint Pack_R11G11B10_FLOAT(float3 rgb)
{
    rgb    = min(rgb, asfloat(0x477C0000));
    uint r = ((f32tof16(rgb.x) + 8) >> 4) & 0x000007FF;
    uint g = ((f32tof16(rgb.y) + 8) << 7) & 0x003FF800;
    uint b = ((f32tof16(rgb.z) + 16) << 17) & 0xFFC00000;
    return r | g | b;
}

uint PackTwoFp32ToFp16(float a, float b)
{
    return (f32tof16(clamp(a, -HLF_MAX, HLF_MAX)) << 16) | f32tof16(clamp(b, -HLF_MAX, HLF_MAX));
}

void UnpackTwoFp32ToFp16(uint packed, out float a, out float b)
{
    a = f16tof32(packed >> 16);
    b = f16tof32(packed & 0xffff);
}

uint3 PackTwoFp32ToFp16(float3 a, float3 b)
{
    return (f32tof16(clamp(a, -HLF_MAX, HLF_MAX)) << 16) | f32tof16(clamp(b, -HLF_MAX, HLF_MAX));
}

void UnpackTwoFp32ToFp16(uint3 packed, out float3 a, out float3 b)
{
    a = f16tof32(packed >> 16);
    b = f16tof32(packed & 0xffff);
}

uint Fp32ToFp16(float2 v)
{
    const uint2 r = f32tof16(clamp(v, -HLF_MAX, HLF_MAX));
    return (r.y << 16) | (r.x & 0xffff);
}

uint Fp32ToFp16NoClamp(float2 v)
{
    const uint2 r = f32tof16(v);
    return (r.y << 16) | (r.x & 0xffff);
}

float2 Fp16ToFp32(uint r)
{
    return f16tof32(uint2(r & 0xffff, r >> 16));
}

uint2 Fp32ToFp16(float4 v)
{
    return uint2(Fp32ToFp16(v.xy), Fp32ToFp16(v.zw));
}

uint2 Fp32ToFp16NoClamp(float4 v)
{
    return uint2(Fp32ToFp16NoClamp(v.xy), Fp32ToFp16NoClamp(v.zw));
}

float4 Fp16ToFp32(uint2 d)
{
    const float2 d0 = Fp16ToFp32(d.x);
    const float2 d1 = Fp16ToFp32(d.y);
    return float4(d0, d1);
}

float2 OctWrap(float2 v)
{
    return (1.0 - abs(v.yx)) * float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

float2 Encode_Oct(float3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-7);
    n.xy = n.z >= 0.0 ? n.xy : OctWrap(n.xy);
    return n.xy;
}

float3 Decode_Oct(float2 f)
{
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float  t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

uint NDirToOctUnorm32(float3 n)
{
    const float2 p = saturate(Encode_Oct(n) * 0.5 + 0.5);
    return uint(p.x * 0xfffe) | (uint(p.y * 0xfffe) << 16);
}

float3 OctToNDirUnorm32(uint pUnorm)
{
    float2 p;
    p.x = saturate(float(pUnorm & 0xffff) / 0xfffe);
    p.y = saturate(float(pUnorm >> 16) / 0xfffe);
    return Decode_Oct(p * 2.0 - 1.0);
}

uint NDirToOctUnorm30(float3 n)
{
    const float2 p = saturate(Encode_Oct(n) * 0.5 + 0.5);
    return (uint(p.x * 0x7fff + 0.5) & 0x7fff) | ((uint(p.y * 0x7fff + 0.5) & 0x7fff) << 15);
}

float3 OctToNDirUnorm30(uint pUnorm)
{
    float2 p;
    p.x = saturate(float(pUnorm & 0x7fff) / 0x7fff);
    p.y = saturate(float((pUnorm >> 15) & 0x7fff) / 0x7fff);
    return Decode_Oct(p * 2.0 - 1.0);
}

uint2 PackOrthoMatrix(float3x3 xform)
{
    uint2 packed;
    const uint handedness = dot(cross(xform[0], xform[1]), xform[2]) > 0.0;
    packed.x = NDirToOctUnorm30(xform[0]);
    packed.y = NDirToOctUnorm30(xform[1]) | (handedness << 31);
    return packed;
}

float3x3 UnpackOrthoMatrix(uint2 packed)
{
    const uint handedness = packed.y >> 31;
    packed.y &= 0x7fffffff;
    float3x3 xform;
    xform[0] = OctToNDirUnorm30(packed.x);
    xform[1] = OctToNDirUnorm30(packed.y);
    xform[2] = handedness != 0 ? cross(xform[0], xform[1]) : cross(xform[1], xform[0]);
    return xform;
}

float3x3 MatrixRotateFromTo(const float3 from, const float3 to, uniform bool columnMajor = true)
{
    const float e = dot(from, to);
    if (abs(e) > 1.0 - 1e-10)
        return float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);

    const float3 v = cross(from, to);
    const float  h = 1.0 / (1.0 + e);
    float3x3 mtx;
    mtx[0][0] = e + h * v.x * v.x;
    mtx[0][1] = h * v.x * v.y + (columnMajor ? -v.z : v.z);
    mtx[0][2] = h * v.x * v.z + (columnMajor ? v.y : -v.y);
    mtx[1][0] = h * v.x * v.y + (columnMajor ? v.z : -v.z);
    mtx[1][1] = e + h * v.y * v.y;
    mtx[1][2] = h * v.z * v.y + (columnMajor ? -v.x : v.x);
    mtx[2][0] = h * v.x * v.z + (columnMajor ? -v.y : v.y);
    mtx[2][1] = h * v.z * v.y + (columnMajor ? v.x : -v.x);
    mtx[2][2] = e + h * v.z * v.z;
    return mtx;
}

uint2 PathIDToPixel(uint id)
{
    return uint2(id >> 16, id & 0xffff);
}

uint PathIDFromPixel(uint2 pixel)
{
    return (pixel.x << 16) | pixel.y;
}

uint Morton16BitEncode(uint x, uint y)
{
    uint temp = (x & 0xff) | ((y & 0xff) << 16);
    temp      = (temp ^ (temp << 4)) & 0x0f0f0f0f;
    temp      = (temp ^ (temp << 2)) & 0x33333333;
    temp      = (temp ^ (temp << 1)) & 0x55555555;
    return ((temp >> 15) | temp) & 0xffff;
}

uint GenericTSPixelToAddress(const uint2 pixelPos, const uint planeIndex, const uint lineStride, const uint planeStride)
{
    const uint tileSize       = 8u;
    const uint xInTile        = pixelPos.x % tileSize;
    const uint yInTile        = pixelPos.y % tileSize;
    const uint tilePixelIndex = Morton16BitEncode(xInTile, yInTile);
    const uint tileBaseX      = pixelPos.x - xInTile;
    const uint tileBaseY      = pixelPos.y - yInTile;
    return tileBaseX * tileSize + tileBaseY * lineStride + tilePixelIndex + planeIndex * planeStride;
}

float3 ReinhardMax(float3 color)
{
    const float luminance = max(1e-7, max(max(color.x, color.y), color.z));
    return color / (luminance + 1.0);
}

#endif // __PATH_TRACER_HELPERS_HLSLI__
