#ifndef __LOBE_TYPE_HLSLI__
#define __LOBE_TYPE_HLSLI__

static const uint kLobeTypeNone                 = 0x00u;
static const uint kLobeTypeDiffuseReflection    = 0x01u;
static const uint kLobeTypeSpecularReflection   = 0x02u;
static const uint kLobeTypeDeltaReflection      = 0x04u;
static const uint kLobeTypeDiffuseTransmission  = 0x10u;
static const uint kLobeTypeSpecularTransmission = 0x20u;
static const uint kLobeTypeDeltaTransmission    = 0x40u;
static const uint kLobeTypeDiffuse              = 0x11u;
static const uint kLobeTypeSpecular             = 0x22u;
static const uint kLobeTypeDelta                = 0x44u;
static const uint kLobeTypeNonDelta             = 0x33u;
static const uint kLobeTypeReflection           = 0x0fu;
static const uint kLobeTypeTransmission         = 0xf0u;
static const uint kLobeTypeNonDeltaReflection   = 0x03u;
static const uint kLobeTypeNonDeltaTransmission = 0x30u;
static const uint kLobeTypeAll                  = 0xffu;

#endif // __LOBE_TYPE_HLSLI__
