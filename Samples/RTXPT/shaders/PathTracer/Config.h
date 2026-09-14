#ifndef __CONFIG_H__
#define __CONFIG_H__

// Path-tracer compile-time configuration. Values mirror
// D:/RTXPT-fork/Rtxpt/Shaders/PathTracer/Config.h.
#define PATH_TRACER_MODE_REFERENCE           0
#define PATH_TRACER_MODE_BUILD_STABLE_PLANES 1
#define PATH_TRACER_MODE_FILL_STABLE_PLANES  2

#ifndef PATH_TRACER_MODE
#    define PATH_TRACER_MODE PATH_TRACER_MODE_REFERENCE
#endif

#define RTXPT_PRIMARY_RAY_INDEX    0
#define RTXPT_VISIBILITY_RAY_INDEX 1
#define RTXPT_HIT_GROUP_STRIDE     2

#ifndef RTXPT_USE_INLINE_VISIBILITY
#    define RTXPT_USE_INLINE_VISIBILITY 0
#endif

#ifndef ENABLE_DEBUG_VIZUALISATIONS
#    define ENABLE_DEBUG_VIZUALISATIONS 0
#endif

#ifndef ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
#    define ENABLE_DEBUG_DELTA_TREE_VIZUALISATION 0
#endif

// ENABLE_MATERIAL_TEXTURES and MATERIAL_TEXTURE_COUNT are supplied by C++ when the
// bindless material-texture table exists.

#if NON_PATH_TRACING_PASS || defined(__cplusplus) || (__SHADER_TARGET_MAJOR < 6 || __SHADER_TARGET_MINOR < 6)
#    define PAYLOAD_QUALIFIER
#    define PAYLOAD_FIELD_RW_ALL
#    define PAYLOAD_FIELD_READCALLER
#else
#    define PAYLOAD_QUALIFIER [raypayload]
#    define PAYLOAD_FIELD_RW_ALL     : read(caller, closesthit, anyhit, miss) : write(caller, closesthit, anyhit, miss)
#    define PAYLOAD_FIELD_READCALLER : read(caller) : write(closesthit, anyhit, miss)
#endif

#endif // __CONFIG_H__
