cmake_minimum_required(VERSION 3.10)

project(RTXPT CXX)

set(SOURCE
    src/RTXPTSample.cpp
    src/RTXPTPostProcessPipeline.cpp
    src/RTXPTNrdConfig.cpp
    src/RTXPTNrdIntegration.cpp
    src/SharcIntegration.cpp
    src/AdaptiveSampling.cpp
    src/RTXPTBloomPass.cpp
    src/RTXPTSuperResolutionPass.cpp
    src/RTXPTRayReconstructionPass.cpp
    src/RTXPTTemporalAAPass.cpp
    src/RTXPTBlitPass.cpp
)

set(INCLUDE
    src/RTXPTSample.hpp
    src/RTXPTEmbeddedRenderer.hpp
    src/RTXPTShaderSource.hpp
    src/RTXPTCameraBasis.hpp
    src/RTXPTScene.hpp
    src/RTXPTSceneJson.hpp
    src/RTXPTSceneGraph.hpp
    src/RTXPTMaterials.hpp
    src/RTXPTMaterialData.hpp
    src/RTXPTLights.hpp
    src/RTXPTLightsBaker.hpp
    src/RTXPTEnvironmentLights.hpp
    src/RTXPTLightFeedback.hpp
    src/RTXPTLightHistory.hpp
    src/RTXPTLightSourceHistory.hpp
    src/RTXPTLightsBakerPass.hpp
    src/RTXPTLightProxyBuildPass.hpp
    src/RTXPTFrameConstants.hpp
    src/RTXPTRealtimeSettings.hpp
    src/RTXPTEnvMapBaker.hpp
    src/RTXPTEnvMapBakerPass.hpp
    src/RTXPTSubInstanceData.hpp
    src/RTXPTSkinnedGeometry.hpp
    src/RTXPTSceneRuntime.hpp
    src/RTXPTSceneFrame.hpp
    src/RTXPTSceneImporter.hpp
    src/RTXPTRenderTargets.hpp
    src/RTXPTPostProcessPipeline.hpp
    src/RTXPTPostProcessPass.hpp
    src/RTXPTNrdConfig.hpp
    src/RTXPTNrdIntegration.hpp
    src/RTXPTRayTracingPass.hpp
    src/SharcIntegration.hpp
    src/AdaptiveSampling.hpp
    src/AsyncPrefix.hpp
    tests/AsyncPrefixSceneValidation.hpp
    src/RTXPTSharedTexture.hpp
    src/RTXPTDenoisingGuidesBaker.hpp
    src/RTXPTEmissiveTrianglePass.hpp
    src/RTXPTComputePass.hpp
    src/RTXPTAccumulationPass.hpp
    src/RTXPTBloomPass.hpp
    src/RTXPTSuperResolutionPass.hpp
    src/RTXPTRayReconstructionPass.hpp
    src/RTXPTRayReconstructionInputs.hpp
    src/RTXPTTemporalAAPass.hpp
    src/RTXPTToneMappingPass.hpp
    src/RTXPTBlitPass.hpp
    src/RTXPTPipelineStateCache.hpp
    RTXPT_FORK_MAPPING.md
    tests/RRSceneValidation.hpp
    tests/HDRValidationCapture.hpp
    tests/SubSamplingSceneValidation.hpp
)

set(SHADERS
    shaders/RTXPTCommon.fxh
    shaders/PathTracer/PathTracer.hlsli
    shaders/PathTracer/PathTracerHandleHit.hlsli
    shaders/PathTracer/PathTracerSHaRC.hlsli
    shaders/PathTracer/SharcBindings.hlsli
    shaders/PathTracer/SharcConstants.h
    shaders/PathTracer/SharcResolve.hlsl
    shaders/PathTracer/SubSampling.h
    shaders/PathTracer/SubSamplingCompaction.hlsl
    shaders/PathTracer/Utils/MicroRng.hlsli
    shaders/PathTracer/Config.h
    shaders/PathTracer/PathTracerHelpers.hlsli
    shaders/PathTracer/PathTracerShared.h
    shaders/PathTracer/PathTracerBridge.hlsli
    shaders/PathTracer/PathTracerCamera.hlsli
    shaders/PathTracer/PathPayload.hlsli
    shaders/PathTracer/PathState.hlsli
    shaders/PathTracer/PathTracerTypes.hlsli
    shaders/PathTracer/Surface/ShadingData.hlsli
    shaders/PathTracer/DenoisingLayers.hlsli
    shaders/PathTracer/PathTracerDenoisingLayers.hlsli
    shaders/PathTracer/DenoisingGuidesBaker.hlsl
    shaders/PathTracer/DenoisingGuidesRR.hlsli
    shaders/PathTracer/PathTracerNestedDielectrics.hlsli
    shaders/PathTracer/BSDFs/MaterialBridge.hlsli
    shaders/PathTracer/BSDFs/LobeType.hlsli
    shaders/PathTracer/BSDFs/InteriorList.hlsli
    shaders/PathTracer/Scene/Material/MaterialData.hlsli
    shaders/PathTracer/Scene/Material/HomogeneousVolumeData.hlsli
    shaders/PathTracer/Rendering/Volumes/HomogeneousVolumeSampler.hlsli
    shaders/PathTracer/Lighting/LightingConfig.h
    shaders/PathTracer/Lighting/LightingTypes.hlsli
    shaders/PathTracer/Lighting/EnvMap.hlsli
    shaders/PathTracer/Lighting/PolymorphicLight.hlsli
    shaders/PathTracer/Lighting/LightSampler.hlsli
    shaders/PathTracer/Lighting/LightsBaker.hlsl
    shaders/PathTracer/Lighting/LightProxyBuild.hlsl
    shaders/PathTracer/Lighting/EnvMapImportanceBaker.hlsl
    shaders/PathTracer/Utils/SampleGenerators.hlsli
    shaders/PathTracer/Utils/Packing.hlsli
    shaders/PathTracer/Utils/StatelessSampleGenerators.hlsli
    shaders/PathTracer/BSDFs/BxDF.hlsli
    shaders/PathTracer/BSDFs/StandardBSDFData.hlsli
    shaders/PathTracer/PathTracerSample.rgen
    shaders/PathTracer/PathTracerMiss.rmiss
    shaders/PathTracer/PathTracerVisibilityMiss.rmiss
    shaders/PathTracer/PathTracerClosestHit.rchit
    shaders/PathTracer/EmissiveTriangleBuild.hlsl
    shaders/PathTracer/PathTracerAnyHit.rahit
    shaders/PathTracer/PathTracerVisibility.hlsli
    shaders/PathTracer/Lighting/LightSamplingMIS.hlsli
    shaders/PathTracer/Lighting/EnvironmentLightConfig.h
    shaders/PathTracer/Lighting/EnvironmentLight.hlsli
    shaders/PathTracer/Lighting/EnvironmentLightsBaker.hlsl
    shaders/PathTracer/Lighting/LightFeedback.hlsl
    shaders/PathTracer/Lighting/LightHistory.hlsl
    shaders/PathTracer/SkinnedVertexBuild.csh
    shaders/PostProcessing/RTXPTFullscreen.vsh
    shaders/PostProcessing/RTXPTAccumulation.csh
    shaders/PostProcessing/RTXPTPostProcess.csh
    shaders/PostProcessing/RTXPTTemporalAAInputs.csh
    shaders/PostProcessing/RTXPTSuperResolutionMotion.csh
    shaders/PostProcessing/RTXPTDenoiserNRD.hlsli
    shaders/PostProcessing/RTXPTBloomShared.h
    shaders/PostProcessing/RTXPTBloomCopy.psh
    shaders/PostProcessing/RTXPTBloomBlur.psh
    shaders/PostProcessing/ToneMapper/ToneMappingShared.h
    shaders/PostProcessing/ToneMapper/ToneMapping.hlsl
    shaders/PostProcessing/ToneMapper/ToneMapping.ps.hlsli
    shaders/PostProcessing/ToneMapper/Luminance.psh
    shaders/RTXPTDebugCompute.csh
    shaders/RTXPTBlit.vsh
    shaders/RTXPTBlit.psh
)

include(FetchContent)

# Shader-only SDK, pinned to the version used by RTXPT-fork 9503910.
set(RTXPT_SHARC_ROOT "" CACHE PATH "Optional local SHaRC SDK checkout")
if(NOT RTXPT_SHARC_ROOT)
    FetchContent_Declare(RTXPTSharc
        GIT_REPOSITORY https://github.com/NVIDIA-RTX/SHARC.git
        GIT_TAG 4e21b585c33c83d723ca9a1e11bbb1090d145793)
    FetchContent_MakeAvailable(RTXPTSharc)
    set(RTXPT_SHARC_ROOT "${rtxptsharc_SOURCE_DIR}")
endif()
if(NOT EXISTS "${RTXPT_SHARC_ROOT}/include/SharcCommon.h")
    message(FATAL_ERROR "RTXPT_SHARC_ROOT must contain include/SharcCommon.h")
endif()

# Fetch assets
set(RTXPT_ASSETS_FETCH_URL "https://github.com/NVIDIA-RTX/RTXPT-Assets")
set(RTXPT_ASSETS_FETCH_TAG "main")
set(RTXPT_ASSETS_FETCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE PATH "Directory to fetch RTXPT assets to")
message(STATUS "Updating RTXPT assets from ${RTXPT_ASSETS_FETCH_URL}, tag ${RTXPT_ASSETS_FETCH_TAG}, into folder ${RTXPT_ASSETS_FETCH_DIR}")
FetchContent_Declare(
    RTXPTAssets
    GIT_REPOSITORY ${RTXPT_ASSETS_FETCH_URL}
    GIT_TAG        ${RTXPT_ASSETS_FETCH_TAG}
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(RTXPTAssets)
file(MAKE_DIRECTORY "${RTXPT_ASSETS_FETCH_DIR}")
file(COPY "${rtxptassets_SOURCE_DIR}/"
    DESTINATION "${RTXPT_ASSETS_FETCH_DIR}"
    PATTERN ".git" EXCLUDE
    PATTERN ".gitattributes" EXCLUDE
    PATTERN "README.md" EXCLUDE
)

if(NOT DEFINED RTXPT_NRD_ROOT)
    if(DILIGENT_NRD_DIR)
        set(RTXPT_NRD_ROOT "${DILIGENT_NRD_DIR}" CACHE PATH "NVIDIA NRD SDK root for the RTXPT standalone denoiser")
    else()
        set(RTXPT_NRD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/NRD" CACHE PATH "NVIDIA NRD SDK root for the RTXPT standalone denoiser")
    endif()
endif()
if(NOT DEFINED RTXPT_NRI_ROOT)
    if(DILIGENT_NRI_DIR)
        set(RTXPT_NRI_ROOT "${DILIGENT_NRI_DIR}" CACHE PATH "NVIDIA NRI SDK root used by the RTXPT NRD submodule when NRD_NRI is enabled")
    else()
        set(RTXPT_NRI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/NRI" CACHE PATH "NVIDIA NRI SDK root used by the RTXPT NRD submodule when NRD_NRI is enabled")
    endif()
endif()
if(NOT DEFINED RTXPT_MATHLIB_ROOT)
    if(DILIGENT_MATHLIB_DIR)
        set(RTXPT_MATHLIB_ROOT "${DILIGENT_MATHLIB_DIR}" CACHE PATH "MathLib root used by the RTXPT NRD submodule")
    else()
        set(RTXPT_MATHLIB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/MathLib" CACHE PATH "MathLib root used by the RTXPT NRD submodule")
    endif()
endif()
if(NOT DEFINED RTXPT_SHADERMAKE_ROOT)
    if(DILIGENT_SHADERMAKE_DIR)
        set(RTXPT_SHADERMAKE_ROOT "${DILIGENT_SHADERMAKE_DIR}" CACHE PATH "ShaderMake root used by the RTXPT NRD submodule")
    else()
        set(RTXPT_SHADERMAKE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ShaderMake" CACHE PATH "ShaderMake root used by the RTXPT NRD submodule")
    endif()
endif()

set(RTXPT_EFFECTIVE_NRD_ROOT "${RTXPT_NRD_ROOT}")
if(RTXPT_EFFECTIVE_NRD_ROOT AND
   NOT EXISTS "${RTXPT_EFFECTIVE_NRD_ROOT}/CMakeLists.txt" AND
   DILIGENT_NRD_DIR AND EXISTS "${DILIGENT_NRD_DIR}/CMakeLists.txt")
    message(STATUS "RTXPT_NRD_ROOT does not contain NRD; falling back to DILIGENT_NRD_DIR.")
    set(RTXPT_EFFECTIVE_NRD_ROOT "${DILIGENT_NRD_DIR}")
endif()

set(RTXPT_EFFECTIVE_NRI_ROOT "${RTXPT_NRI_ROOT}")
if(RTXPT_EFFECTIVE_NRI_ROOT AND
   NOT EXISTS "${RTXPT_EFFECTIVE_NRI_ROOT}/CMakeLists.txt" AND
   DILIGENT_NRI_DIR AND EXISTS "${DILIGENT_NRI_DIR}/CMakeLists.txt")
    message(STATUS "RTXPT_NRI_ROOT does not contain NRI; falling back to DILIGENT_NRI_DIR.")
    set(RTXPT_EFFECTIVE_NRI_ROOT "${DILIGENT_NRI_DIR}")
endif()

set(RTXPT_EFFECTIVE_MATHLIB_ROOT "${RTXPT_MATHLIB_ROOT}")
if(RTXPT_EFFECTIVE_MATHLIB_ROOT AND
   NOT EXISTS "${RTXPT_EFFECTIVE_MATHLIB_ROOT}/CMakeLists.txt" AND
   DILIGENT_MATHLIB_DIR AND EXISTS "${DILIGENT_MATHLIB_DIR}/CMakeLists.txt")
    message(STATUS "RTXPT_MATHLIB_ROOT does not contain MathLib; falling back to DILIGENT_MATHLIB_DIR.")
    set(RTXPT_EFFECTIVE_MATHLIB_ROOT "${DILIGENT_MATHLIB_DIR}")
endif()

set(RTXPT_EFFECTIVE_SHADERMAKE_ROOT "${RTXPT_SHADERMAKE_ROOT}")
if(RTXPT_EFFECTIVE_SHADERMAKE_ROOT AND
   NOT EXISTS "${RTXPT_EFFECTIVE_SHADERMAKE_ROOT}/CMakeLists.txt" AND
   DILIGENT_SHADERMAKE_DIR AND EXISTS "${DILIGENT_SHADERMAKE_DIR}/CMakeLists.txt")
    message(STATUS "RTXPT_SHADERMAKE_ROOT does not contain ShaderMake; falling back to DILIGENT_SHADERMAKE_DIR.")
    set(RTXPT_EFFECTIVE_SHADERMAKE_ROOT "${DILIGENT_SHADERMAKE_DIR}")
endif()

set(RTXPT_HAS_NRD OFF)
set(RTXPT_NRD_SHADER_INCLUDE_DIR "")
set(RTXPT_NRD_SHADER_SOURCE_DIR "")
set(RTXPT_NRD_SHADER_CONFIG_DIR "")
set(RTXPT_NRD_MATHLIB_INCLUDE_DIR "")
set(RTXPT_NRD_ROOT_NORMALIZED "")
set(DILIGENT_NRI_DIR_NORMALIZED "")
set(DILIGENT_SHADERMAKE_DIR_NORMALIZED "")
set(DILIGENT_MATHLIB_DIR_NORMALIZED "")
set(RTXPT_NRI_AVAILABLE OFF)
set(RTXPT_NRD_MATHLIB_AVAILABLE OFF)
set(RTXPT_NRD_SHADERMAKE_AVAILABLE OFF)
set(RTXPT_NRD_CMAKE_DEPS_AVAILABLE OFF)
set(RTXPT_HAS_D3D_SHADER_REFLECTION OFF)
set(NRD_NRI OFF CACHE BOOL "Build NRD NRI integration")

if(RTXPT_EFFECTIVE_NRD_ROOT)
    file(TO_CMAKE_PATH "${RTXPT_EFFECTIVE_NRD_ROOT}" RTXPT_NRD_ROOT_NORMALIZED)
endif()

if(RTXPT_NRD_ROOT_NORMALIZED)
    if(NRD_NRI)
        if(FETCHCONTENT_SOURCE_DIR_NRI)
            file(TO_CMAKE_PATH "${FETCHCONTENT_SOURCE_DIR_NRI}" DILIGENT_NRI_DIR_NORMALIZED)
            if(EXISTS "${DILIGENT_NRI_DIR_NORMALIZED}/CMakeLists.txt" AND EXISTS "${DILIGENT_NRI_DIR_NORMALIZED}/Include/NRI.h")
                set(RTXPT_NRI_AVAILABLE ON)
            else()
                message(WARNING "NRD_NRI is ON, but FETCHCONTENT_SOURCE_DIR_NRI does not contain a valid NRI checkout.")
            endif()
        elseif(RTXPT_EFFECTIVE_NRI_ROOT)
            file(TO_CMAKE_PATH "${RTXPT_EFFECTIVE_NRI_ROOT}" DILIGENT_NRI_DIR_NORMALIZED)
            if(EXISTS "${DILIGENT_NRI_DIR_NORMALIZED}/CMakeLists.txt" AND EXISTS "${DILIGENT_NRI_DIR_NORMALIZED}/Include/NRI.h")
                set(FETCHCONTENT_SOURCE_DIR_NRI "${DILIGENT_NRI_DIR_NORMALIZED}" CACHE PATH "Use RTXPT's local NRI submodule when NRD_NRI is enabled")
                set(RTXPT_NRI_AVAILABLE ON)
            else()
                message(WARNING "NRD_NRI is ON, but RTXPT_NRI_ROOT does not contain a valid NRI checkout.")
            endif()
        elseif(DILIGENT_NRI_DIR)
            file(TO_CMAKE_PATH "${DILIGENT_NRI_DIR}" DILIGENT_NRI_DIR_NORMALIZED)
            if(EXISTS "${DILIGENT_NRI_DIR_NORMALIZED}/CMakeLists.txt" AND EXISTS "${DILIGENT_NRI_DIR_NORMALIZED}/Include/NRI.h")
                set(FETCHCONTENT_SOURCE_DIR_NRI "${DILIGENT_NRI_DIR_NORMALIZED}" CACHE PATH "Use DiligentSamples' local NRI submodule when NRD_NRI is enabled")
                set(RTXPT_NRI_AVAILABLE ON)
            else()
                message(WARNING "NRD_NRI is ON, but DILIGENT_NRI_DIR does not contain a valid NRI checkout.")
            endif()
        endif()
    endif()

    set(RTXPT_NRD_INCLUDE_DIR "${RTXPT_NRD_ROOT_NORMALIZED}/Include")
    if(EXISTS "${RTXPT_NRD_ROOT_NORMALIZED}/Shaders/Include/NRD.hlsli" AND
       EXISTS "${RTXPT_NRD_ROOT_NORMALIZED}/Shaders/Source")
        set(RTXPT_NRD_SHADER_INCLUDE_DIR "${RTXPT_NRD_ROOT_NORMALIZED}/Shaders/Include")
        set(RTXPT_NRD_SHADER_SOURCE_DIR "${RTXPT_NRD_ROOT_NORMALIZED}/Shaders/Source")
    else()
        set(RTXPT_NRD_SHADER_INCLUDE_DIR "${RTXPT_NRD_ROOT_NORMALIZED}/Shaders")
        set(RTXPT_NRD_SHADER_SOURCE_DIR "${RTXPT_NRD_ROOT_NORMALIZED}/Shaders")
    endif()
    set(RTXPT_NRD_SHADER_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}/NRD/Shaders")

    if(TARGET MathLib)
        get_target_property(RTXPT_NRD_MATHLIB_INCLUDE_DIR MathLib SOURCE_DIR)
        if(RTXPT_NRD_MATHLIB_INCLUDE_DIR)
            set(RTXPT_NRD_MATHLIB_AVAILABLE ON)
        endif()
    elseif(FETCHCONTENT_SOURCE_DIR_MATHLIB)
        file(TO_CMAKE_PATH "${FETCHCONTENT_SOURCE_DIR_MATHLIB}" RTXPT_NRD_MATHLIB_INCLUDE_DIR)
        if(EXISTS "${RTXPT_NRD_MATHLIB_INCLUDE_DIR}/CMakeLists.txt")
            set(RTXPT_NRD_MATHLIB_AVAILABLE ON)
        endif()
    elseif(RTXPT_EFFECTIVE_MATHLIB_ROOT)
        file(TO_CMAKE_PATH "${RTXPT_EFFECTIVE_MATHLIB_ROOT}" DILIGENT_MATHLIB_DIR_NORMALIZED)
        if(EXISTS "${DILIGENT_MATHLIB_DIR_NORMALIZED}/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_MATHLIB "${DILIGENT_MATHLIB_DIR_NORMALIZED}" CACHE PATH "Use RTXPT's local MathLib submodule for NRD")
            set(RTXPT_NRD_MATHLIB_INCLUDE_DIR "${DILIGENT_MATHLIB_DIR_NORMALIZED}")
            set(RTXPT_NRD_MATHLIB_AVAILABLE ON)
        endif()
    elseif(DILIGENT_MATHLIB_DIR)
        file(TO_CMAKE_PATH "${DILIGENT_MATHLIB_DIR}" DILIGENT_MATHLIB_DIR_NORMALIZED)
        if(EXISTS "${DILIGENT_MATHLIB_DIR_NORMALIZED}/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_MATHLIB "${DILIGENT_MATHLIB_DIR_NORMALIZED}" CACHE PATH "Use DiligentSamples' local MathLib submodule for NRD")
            set(RTXPT_NRD_MATHLIB_INCLUDE_DIR "${DILIGENT_MATHLIB_DIR_NORMALIZED}")
            set(RTXPT_NRD_MATHLIB_AVAILABLE ON)
        endif()
    endif()

    if(TARGET ShaderMakeBlob AND TARGET ShaderMake)
        set(RTXPT_NRD_SHADERMAKE_AVAILABLE ON)
    elseif(FETCHCONTENT_SOURCE_DIR_SHADERMAKE)
        file(TO_CMAKE_PATH "${FETCHCONTENT_SOURCE_DIR_SHADERMAKE}" RTXPT_NRD_SHADERMAKE_ROOT_NORMALIZED)
        if(EXISTS "${RTXPT_NRD_SHADERMAKE_ROOT_NORMALIZED}/CMakeLists.txt")
            set(RTXPT_NRD_SHADERMAKE_AVAILABLE ON)
        endif()
    elseif(RTXPT_EFFECTIVE_SHADERMAKE_ROOT)
        file(TO_CMAKE_PATH "${RTXPT_EFFECTIVE_SHADERMAKE_ROOT}" DILIGENT_SHADERMAKE_DIR_NORMALIZED)
        if(EXISTS "${DILIGENT_SHADERMAKE_DIR_NORMALIZED}/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_SHADERMAKE "${DILIGENT_SHADERMAKE_DIR_NORMALIZED}" CACHE PATH "Use RTXPT's local ShaderMake submodule for NRD")
            set(RTXPT_NRD_SHADERMAKE_AVAILABLE ON)
        endif()
    elseif(DILIGENT_SHADERMAKE_DIR)
        file(TO_CMAKE_PATH "${DILIGENT_SHADERMAKE_DIR}" DILIGENT_SHADERMAKE_DIR_NORMALIZED)
        if(EXISTS "${DILIGENT_SHADERMAKE_DIR_NORMALIZED}/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_SHADERMAKE "${DILIGENT_SHADERMAKE_DIR_NORMALIZED}" CACHE PATH "Use DiligentSamples' local ShaderMake submodule for NRD")
            set(RTXPT_NRD_SHADERMAKE_AVAILABLE ON)
        endif()
    endif()

    if(RTXPT_NRD_MATHLIB_AVAILABLE AND RTXPT_NRD_SHADERMAKE_AVAILABLE)
        set(RTXPT_NRD_CMAKE_DEPS_AVAILABLE ON)
    endif()

    if(EXISTS "${RTXPT_NRD_ROOT_NORMALIZED}/CMakeLists.txt" AND
       EXISTS "${RTXPT_NRD_INCLUDE_DIR}/NRD.h" AND
       EXISTS "${RTXPT_NRD_INCLUDE_DIR}/NRDSettings.h" AND
       EXISTS "${RTXPT_NRD_INCLUDE_DIR}/NRDDescs.h" AND
       EXISTS "${RTXPT_NRD_SHADER_INCLUDE_DIR}/NRD.hlsli" AND
       (EXISTS "${RTXPT_NRD_SHADER_SOURCE_DIR}/REBLUR_DiffuseSpecular_TemporalAccumulation.cs.hlsl" OR
        EXISTS "${RTXPT_NRD_SHADER_SOURCE_DIR}/REBLUR_TemporalAccumulation.cs.hlsl") AND
       RTXPT_NRD_CMAKE_DEPS_AVAILABLE AND
       (NOT NRD_NRI OR RTXPT_NRI_AVAILABLE))
        set(NRD_STATIC_LIBRARY ON CACHE BOOL "Build NRD as a static library" FORCE)
        set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "RTXPT compiles NRD HLSL through Diligent DXC" FORCE)
        set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "RTXPT compiles NRD HLSL through Diligent DXC" FORCE)
        set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "RTXPT compiles NRD HLSL through Diligent DXC" FORCE)
        set(SHADERMAKE_FIND_DXC OFF CACHE BOOL "Use locally available DXC for ShaderMake" FORCE)
        add_subdirectory("${RTXPT_NRD_ROOT_NORMALIZED}" "${CMAKE_CURRENT_BINARY_DIR}/NRD")
        set(RTXPT_HAS_NRD ON)
    else()
        message(STATUS "RTXPT standalone NRD is disabled: required NRD files, local ShaderMake/MathLib CMake targets, or optional NRI checkout were not found.")
    endif()
endif()

include(cmake/RTXPTLibrary.cmake)
add_rtxpt_library(RTXPT-Runtime
    SHARC_SHADER_DIR "${RTXPT_SHARC_ROOT}/include"
    DEPENDENCIES Diligent-AssetLoader Diligent-JSON Diligent-GraphicsTools
                 Diligent-Archiver-static DiligentFX Diligent-Imgui)

add_sample_app(RTXPT
    IDE_FOLDER
        DiligentSamples/Samples
    SOURCES
        ${SOURCE}
    INCLUDES
        ${INCLUDE}
    SHADERS
        ${SHADERS}
    DXC_REQUIRED
        YES
    INSTALL_ASSETS_DIR
        assets
)

if(PLATFORM_WIN32)
    set_target_properties(RTXPT PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(MSVC)
        # Embed the DPI-awareness manifest so that the window size is not
        # stretched by the desktop scaling factor (PerMonitorV2, matching the
        # original RTXPT which runs with per-monitor DPI awareness enabled).
        target_sources(RTXPT PRIVATE RTXPT.manifest)
    endif()
endif()

if(PLATFORM_WIN32 OR PLATFORM_LINUX)
    add_custom_target(RTXPT-SharcShaders
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${RTXPT_SHARC_ROOT}/include" "$<TARGET_FILE_DIR:RTXPT>/shaders/SHARC"
        VERBATIM)
    add_dependencies(RTXPT RTXPT-SharcShaders)
    rtxpt_stage_shaders(RTXPT "$<TARGET_FILE_DIR:RTXPT>/shaders")
endif()

if(DILIGENT_INSTALL_SAMPLES AND (PLATFORM_LINUX OR PLATFORM_WIN32))
    file(RELATIVE_PATH RTXPT_REL_PATH "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
    rtxpt_install_shaders("${CMAKE_INSTALL_BINDIR}/${RTXPT_REL_PATH}/$<CONFIG>/shaders")
    install(DIRECTORY "${RTXPT_SHARC_ROOT}/include/"
            DESTINATION "${CMAKE_INSTALL_BINDIR}/${RTXPT_REL_PATH}/$<CONFIG>/shaders/SHARC")
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/shaders PREFIX Shaders FILES ${SHADERS})

if (NOT SUPER_RESOLUTION_SUPPORTED)
    message(FATAL_ERROR "RTXPT requires Diligent SuperResolution; configure without DILIGENT_NO_SUPER_RESOLUTION or disable the RTXPT sample")
endif()

target_link_libraries(RTXPT
PRIVATE
    RTXPT-Runtime
    Diligent-AssetLoader
    Diligent-JSON
    Diligent-SuperResolution-static
    Diligent-GraphicsTools      # CreateRenderStateCache / RenderStateCache.h
    Diligent-Archiver-static    # LoadAndGetArchiverFactory()
    DiligentFX
)

if(RTXPT_HAS_NRD AND TARGET Diligent-GraphicsEngineD3DBaseInterface)
    set(RTXPT_HAS_D3D_SHADER_REFLECTION ON)
endif()

target_compile_definitions(RTXPT
PRIVATE
    RTXPT_SHARC_SHADER_DIR="${RTXPT_SHARC_ROOT}/include"
    RTXPT_HAS_NRD=$<BOOL:${RTXPT_HAS_NRD}>
    RTXPT_HAS_D3D_SHADER_REFLECTION=$<BOOL:${RTXPT_HAS_D3D_SHADER_REFLECTION}>
    RTXPT_NRD_SHADER_INCLUDE_DIR="${RTXPT_NRD_SHADER_INCLUDE_DIR}"
    RTXPT_NRD_SHADER_SOURCE_DIR="${RTXPT_NRD_SHADER_SOURCE_DIR}"
    RTXPT_NRD_SHADER_CONFIG_DIR="${RTXPT_NRD_SHADER_CONFIG_DIR}"
    RTXPT_NRD_MATHLIB_INCLUDE_DIR="${RTXPT_NRD_MATHLIB_INCLUDE_DIR}"
)

# The shared post-process implementation also serves the Sample's existing NRD branch.
target_compile_definitions(RTXPT-Runtime PRIVATE
    RTXPT_HAS_NRD=$<BOOL:${RTXPT_HAS_NRD}>
    RTXPT_NRD_SHADER_INCLUDE_DIR="${RTXPT_NRD_SHADER_INCLUDE_DIR}"
    RTXPT_NRD_SHADER_CONFIG_DIR="${RTXPT_NRD_SHADER_CONFIG_DIR}"
    RTXPT_NRD_MATHLIB_INCLUDE_DIR="${RTXPT_NRD_MATHLIB_INCLUDE_DIR}")

if(RTXPT_HAS_NRD)
    target_link_libraries(RTXPT PRIVATE NRD)
    if(TARGET Diligent-GraphicsEngineD3DBaseInterface)
        target_link_libraries(RTXPT PRIVATE Diligent-GraphicsEngineD3DBaseInterface)
    endif()
    target_include_directories(RTXPT PRIVATE
        "${RTXPT_NRD_INCLUDE_DIR}"
        "${RTXPT_NRD_SHADER_INCLUDE_DIR}"
    )
endif()

if (PLATFORM_WIN32 AND D3D12_SUPPORTED AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    include(cmake/HitObjectRuntime.cmake)
endif()

# Share Core's NGX runtime and pinned DLSS SDK with the SR module.
option(RTXPT_RAY_RECONSTRUCTION_SHARED "Use the shared ray reconstruction module" OFF)
if(RAY_RECONSTRUCTION_SUPPORTED)
    target_compile_definitions(RTXPT PRIVATE RTXPT_HAS_RAY_RECONSTRUCTION=1)
    if(RTXPT_RAY_RECONSTRUCTION_SHARED)
        target_link_libraries(RTXPT PRIVATE Diligent-RayReconstruction-shared)
    else()
        target_link_libraries(RTXPT PRIVATE Diligent-RayReconstruction-static)
    endif()
else()
    target_compile_definitions(RTXPT PRIVATE RTXPT_HAS_RAY_RECONSTRUCTION=0)
endif()

if(RTXPT_RAY_RECONSTRUCTION_SHARED)
    rtxpt_deploy_reconstruction_runtimes(RTXPT RAY_RECONSTRUCTION_SHARED)
else()
    rtxpt_deploy_reconstruction_runtimes(RTXPT)
endif()
