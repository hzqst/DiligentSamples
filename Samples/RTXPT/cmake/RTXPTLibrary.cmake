# Reusable scene and embedded Reference/RealtimeRaw/RealtimeNative renderer. This entry point does not
# fetch assets or optional denoising/reconstruction SDKs. Hosts supply Diligent
# dependencies, either source targets or an interface target for an installed SDK,
# and optionally an NRD target (NRD_TARGET, see rtxpt_add_nrd) for the standalone denoiser.
# D3D_SHADER_REFLECTION states that the dependencies provide ShaderD3D.h when they are not
# Diligent source targets; NRD needs it to bind its resources by register on D3D.
include_guard(GLOBAL)
get_filename_component(RTXPT_LIBRARY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set_property(GLOBAL PROPERTY RTXPT_LIBRARY_ROOT "${RTXPT_LIBRARY_ROOT}")

function(add_rtxpt_library target)
    cmake_parse_arguments(RTXPT "NO_UI;PATH_TRACING_ONLY;D3D_SHADER_REFLECTION" "SHARC_SHADER_DIR;NRD_TARGET" "DEPENDENCIES" ${ARGN})
    get_property(root GLOBAL PROPERTY RTXPT_LIBRARY_ROOT)
    set(sources
        RTXPTScene RTXPTSceneGraph RTXPTSceneJson RTXPTMaterials
        RTXPTEmbeddedRenderer RTXPTLights RTXPTLightsBaker RTXPTLightsBakerPass
        RTXPTLightProxyBuildPass RTXPTEnvMapBaker RTXPTEnvMapBakerPass
        RTXPTEmissiveTrianglePass RTXPTRayTracingPass RTXPTSkinnedGeometry
        RTXPTSceneRuntime RTXPTSceneFrame RTXPTSceneImporter RTXPTComputePass
        RTXPTAccumulationPass RTXPTToneMappingPass RTXPTRenderTargets
        RTXPTDenoisingGuidesBaker RTXPTPostProcessPass RTXPTRealtimePipeline
        RTXPTNrdConfig RTXPTNrdIntegration RTXPTNrdDenoiser)
    set(source_files)
    foreach(source IN LISTS sources)
        list(APPEND source_files "${root}/src/${source}.cpp")
    endforeach()
    add_library(${target} STATIC ${source_files})
    target_include_directories(${target} PUBLIC "${root}/src")
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_link_libraries(${target} PUBLIC ${RTXPT_DEPENDENCIES})
    if(TARGET Diligent-GraphicsEngineD3D12Interface)
        target_link_libraries(${target} PUBLIC Diligent-GraphicsEngineD3D12Interface)
    endif()
    target_compile_definitions(${target} PRIVATE RTXPT_SHARC_SHADER_DIR="${RTXPT_SHARC_SHADER_DIR}")
    if(RTXPT_NO_UI)
        target_compile_definitions(${target} PRIVATE RTXPT_NO_UI=1)
    endif()
    # RTXPT_HAS_NRD changes the layout of RTXPTNrdIntegration, so it must reach every consumer.
    # Without NRD the denoiser classes still build and report NRD as unavailable.
    if(RTXPT_NRD_TARGET)
        target_link_libraries(${target} PUBLIC ${RTXPT_NRD_TARGET})
        target_compile_definitions(${target} PUBLIC RTXPT_HAS_NRD=1)
        if(TARGET Diligent-GraphicsEngineD3DBaseInterface)
            target_link_libraries(${target} PRIVATE Diligent-GraphicsEngineD3DBaseInterface)
            target_compile_definitions(${target} PRIVATE RTXPT_HAS_D3D_SHADER_REFLECTION=1)
        elseif(RTXPT_D3D_SHADER_REFLECTION)
            target_compile_definitions(${target} PRIVATE RTXPT_HAS_D3D_SHADER_REFLECTION=1)
        endif()
    else()
        target_compile_definitions(${target} PUBLIC RTXPT_HAS_NRD=0)
    endif()
    if(RTXPT_PATH_TRACING_ONLY)
        # This affects public class layouts and must reach every consumer.
        target_compile_definitions(${target} PUBLIC RTXPT_PATH_TRACING_ONLY=1)
    endif()
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

# Builds NRD as the static target NRD from a DiligentSamples ThirdParty directory (NRD, MathLib
# and ShaderMake checkouts), configured as the sample configures it: RTXPT compiles NRD's HLSL at
# run time through Diligent, so NRD embeds no shader binaries and nothing is downloaded. Sets
# RTXPT_NRD_SHADER_DIRS and RTXPT_NRD_MATHLIB_DIR in the caller's scope for rtxpt_stage_shaders,
# rtxpt_install_shaders and a host shader source.
function(rtxpt_add_nrd thirdparty_dir binary_dir)
    foreach(dependency NRD MathLib ShaderMake)
        if(NOT EXISTS "${thirdparty_dir}/${dependency}/CMakeLists.txt")
            message(FATAL_ERROR "rtxpt_add_nrd needs the ${dependency} checkout in ${thirdparty_dir}")
        endif()
    endforeach()
    set(FETCHCONTENT_SOURCE_DIR_MATHLIB "${thirdparty_dir}/MathLib" CACHE PATH "MathLib checkout for NRD")
    set(FETCHCONTENT_SOURCE_DIR_SHADERMAKE "${thirdparty_dir}/ShaderMake" CACHE PATH "ShaderMake checkout for NRD")
    set(NRD_STATIC_LIBRARY ON CACHE BOOL "Build NRD as a static library" FORCE)
    set(NRD_NRI OFF CACHE BOOL "RTXPT does not use NRI" FORCE)
    set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "RTXPT compiles NRD HLSL through Diligent DXC" FORCE)
    set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "RTXPT compiles NRD HLSL through Diligent DXC" FORCE)
    set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "RTXPT compiles NRD HLSL through Diligent DXC" FORCE)
    set(SHADERMAKE_FIND_DXC OFF CACHE BOOL "Use locally available DXC for ShaderMake" FORCE)
    add_subdirectory("${thirdparty_dir}/NRD" "${binary_dir}" EXCLUDE_FROM_ALL)
    set(RTXPT_NRD_SHADER_DIRS "${thirdparty_dir}/NRD/Shaders" PARENT_SCOPE)
    set(RTXPT_NRD_MATHLIB_DIR "${thirdparty_dir}/MathLib" PARENT_SCOPE)
endfunction()

# NRD compiles its shaders at run time, so a host that links NRD deploys NRD's shader sources and
# headers with the RTXPT shaders: NRD_SHADER_DIRS are the directories with NRD's .hlsl/.hlsli files
# and MATHLIB_DIR is the MathLib root (for ml.hlsli). They are flattened into <destination>/NRD, the
# directory that RTXPTAppendNrdShaderSearchDirs() and a host shader source covering subdirectories
# search. Third-party license notices remain the host's responsibility.
function(rtxpt_check_nrd_shader_args mathlib_dir)
    if(ARGN AND NOT EXISTS "${mathlib_dir}/ml.hlsli")
        message(FATAL_ERROR "NRD_SHADER_DIRS require MATHLIB_DIR with ml.hlsli")
    endif()
    foreach(dir IN LISTS ARGN)
        file(GLOB shaders "${dir}/*.hlsl" "${dir}/*.hlsli")
        if(NOT shaders)
            message(FATAL_ERROR "No NRD shaders found in ${dir}")
        endif()
    endforeach()
endfunction()

# Staging runs on every build so shader-only edits cannot leave stale files.
function(rtxpt_stage_shaders target destination)
    cmake_parse_arguments(RTXPT "" "MATHLIB_DIR" "NRD_SHADER_DIRS" ${ARGN})
    get_property(root GLOBAL PROPERTY RTXPT_LIBRARY_ROOT)
    rtxpt_check_nrd_shader_args("${RTXPT_MATHLIB_DIR}" ${RTXPT_NRD_SHADER_DIRS})
    set(nrd_commands)
    foreach(dir IN LISTS RTXPT_NRD_SHADER_DIRS)
        list(APPEND nrd_commands COMMAND ${CMAKE_COMMAND} -E copy_directory "${dir}" "${destination}/NRD")
    endforeach()
    if(RTXPT_NRD_SHADER_DIRS)
        list(APPEND nrd_commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RTXPT_MATHLIB_DIR}/ml.hlsli" "${destination}/NRD")
    endif()
    add_custom_target(${target}-RTXPTShaders
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${root}/shaders" "${destination}"
        ${nrd_commands}
        VERBATIM)
    add_dependencies(${target} ${target}-RTXPTShaders)
endfunction()

# Remaining arguments are passed to install(), for example COMPONENT.
function(rtxpt_install_shaders destination)
    cmake_parse_arguments(RTXPT "" "MATHLIB_DIR" "NRD_SHADER_DIRS" ${ARGN})
    get_property(root GLOBAL PROPERTY RTXPT_LIBRARY_ROOT)
    rtxpt_check_nrd_shader_args("${RTXPT_MATHLIB_DIR}" ${RTXPT_NRD_SHADER_DIRS})
    install(DIRECTORY "${root}/shaders/" DESTINATION "${destination}" ${RTXPT_UNPARSED_ARGUMENTS})
    foreach(dir IN LISTS RTXPT_NRD_SHADER_DIRS)
        install(DIRECTORY "${dir}/" DESTINATION "${destination}/NRD" ${RTXPT_UNPARSED_ARGUMENTS}
                FILES_MATCHING PATTERN "*.hlsl" PATTERN "*.hlsli")
    endforeach()
    if(RTXPT_NRD_SHADER_DIRS)
        install(FILES "${RTXPT_MATHLIB_DIR}/ml.hlsli" DESTINATION "${destination}/NRD" ${RTXPT_UNPARSED_ARGUMENTS})
    endif()
endfunction()

# Call only for a host that links the reconstruction providers. Core supplies
# these helpers and the pinned SDK/runtime paths; no SDK is downloaded here.
function(rtxpt_deploy_reconstruction_runtimes target)
    cmake_parse_arguments(RTXPT "RAY_RECONSTRUCTION_SHARED" "" "" ${ARGN})
    if(DILIGENT_DLSS_SUPPORTED)
        copy_dlss_dlls(${target})
    endif()
    if(RAY_RECONSTRUCTION_SUPPORTED)
        if(RTXPT_RAY_RECONSTRUCTION_SHARED)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:Diligent-RayReconstruction-shared>" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
        copy_diligent_ngx_runtime(${target})
        copy_dlssd_dlls(${target})
    endif()
endfunction()
