#ifndef __POLYMORPHIC_LIGHT_H__
#define __POLYMORPHIC_LIGHT_H__

#ifdef __cplusplus
namespace Diligent
{
#define LIGHT_UINT Uint32
#else
#define LIGHT_UINT uint
#endif

static const LIGHT_UINT kPolymorphicLightTypeShift = 24;
static const LIGHT_UINT kPolymorphicLightTypeMask = 0xf;
static const LIGHT_UINT kPolymorphicLightTypeTriangle = 1;
static const LIGHT_UINT kPolymorphicLightTypeEnvironment = 5;
static const LIGHT_UINT kPolymorphicLightShapingEnableBit = 1u << 28;
static const LIGHT_UINT kPolymorphicLightIesProfileEnableBit = 1u << 29;
static const LIGHT_UINT kPolymorphicLightShapingUseMinFalloff = 1u << 30;
static const float kPolymorphicLightMinLog2Radiance = -8.0f;
static const float kPolymorphicLightMaxLog2Radiance = 40.0f;

// RTXPT-fork's GPU ABI: two uint4 blocks, plus a separate optional uint4.
struct PolymorphicLightInfo
{
    float3 Center;
    LIGHT_UINT ColorTypeAndFlags;
    LIGHT_UINT Direction1;
    LIGHT_UINT Direction2;
    LIGHT_UINT Scalars;
    LIGHT_UINT LogRadiance;

    bool HasLightShaping() { return (ColorTypeAndFlags & kPolymorphicLightShapingEnableBit) != 0; }
};

struct PolymorphicLightInfoEx
{
    LIGHT_UINT IesProfileIndex;
    LIGHT_UINT PrimaryAxis;
    LIGHT_UINT CosConeAngleAndSoftness;
    LIGHT_UINT UniqueID;
};

struct PolymorphicLightInfoFull
{
    PolymorphicLightInfo Base;
    PolymorphicLightInfoEx Extended;
};

#undef LIGHT_UINT
#ifdef __cplusplus
static_assert(sizeof(PolymorphicLightInfo) == 32, "Packed light base must occupy two uint4 blocks");
static_assert(sizeof(PolymorphicLightInfoEx) == 16, "Packed light extension must occupy one uint4 block");
static_assert(sizeof(PolymorphicLightInfoFull) == 48, "Decoded base and extension layout");
} // namespace Diligent
#endif
#endif
