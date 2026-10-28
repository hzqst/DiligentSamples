#ifndef __POLYMORPHIC_LIGHT_H__
#define __POLYMORPHIC_LIGHT_H__

#ifdef __cplusplus
namespace Diligent
{
#    define LIGHT_UINT Uint32
#else
#    define LIGHT_UINT uint
#endif

static const LIGHT_UINT kPolymorphicLightTypeShift            = 24;
static const LIGHT_UINT kPolymorphicLightTypeMask             = 0xf;
static const LIGHT_UINT kPolymorphicLightTypeTriangle         = 1;
static const LIGHT_UINT kPolymorphicLightTypeEnvironment      = 5;
static const LIGHT_UINT kPolymorphicLightTypeCluster          = 8;
static const LIGHT_UINT kPolymorphicLightIsInCluster          = 1u << 31;
static const LIGHT_UINT kPolymorphicLightShapingEnableBit     = 1u << 28;
static const LIGHT_UINT kPolymorphicLightIesProfileEnableBit  = 1u << 29;
static const LIGHT_UINT kPolymorphicLightShapingUseMinFalloff = 1u << 30;
static const float      kPolymorphicLightMinLog2Radiance      = -8.0f;
static const float      kPolymorphicLightMaxLog2Radiance      = 40.0f;

// RTXPT-fork's GPU ABI: two uint4 blocks, plus a separate optional uint4.
struct PolymorphicLightInfo
{
    float3     Center;
    LIGHT_UINT ColorTypeAndFlags;
    LIGHT_UINT Direction1;
    LIGHT_UINT Direction2;
    LIGHT_UINT Scalars;
    LIGHT_UINT LogRadiance;

    bool HasLightShaping() { return (ColorTypeAndFlags & kPolymorphicLightShapingEnableBit) != 0; }
    bool IsPartOfLightCluster() { return (ColorTypeAndFlags & kPolymorphicLightIsInCluster) != 0; }
};

struct PolymorphicLightInfoEx
{
    LIGHT_UINT IesProfileIndex;
    LIGHT_UINT PrimaryAxis;
    LIGHT_UINT CosConeAngleAndSoftness;
    LIGHT_UINT ClusterIndexAndCount;

    static LIGHT_UINT PackClusterIndexAndCount(LIGHT_UINT index, LIGHT_UINT count) { return (index << 9) | (count - 1); }
    static LIGHT_UINT UnpackClusterIndex(LIGHT_UINT packed) { return packed >> 9; }
    static LIGHT_UINT UnpackClusterLightCount(LIGHT_UINT packed) { return (packed & 511u) + 1; }
};

struct PolymorphicLightInfoFull
{
    PolymorphicLightInfo   Base;
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
