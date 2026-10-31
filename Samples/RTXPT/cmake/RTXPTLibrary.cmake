# Reusable scene and embedded reference renderer. This entry point does not
# fetch assets or optional denoising/reconstruction SDKs. Hosts supply Diligent
# dependencies, either source targets or an interface target for an installed SDK.
include_guard(GLOBAL)
get_filename_component(RTXPT_LIBRARY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set_property(GLOBAL PROPERTY RTXPT_LIBRARY_ROOT "${RTXPT_LIBRARY_ROOT}")

function(add_rtxpt_library target)
    cmake_parse_arguments(RTXPT "NO_UI;PATH_TRACING_ONLY" "SHARC_SHADER_DIR" "DEPENDENCIES" ${ARGN})
    get_property(root GLOBAL PROPERTY RTXPT_LIBRARY_ROOT)
    set(sources
        RTXPTScene RTXPTSceneGraph RTXPTSceneJson RTXPTMaterials
        RTXPTEmbeddedRenderer RTXPTLights RTXPTLightsBaker RTXPTLightsBakerPass
        RTXPTLightProxyBuildPass RTXPTEnvMapBaker RTXPTEnvMapBakerPass
        RTXPTEmissiveTrianglePass RTXPTRayTracingPass RTXPTSkinnedGeometry
        RTXPTSceneRuntime RTXPTSceneFrame RTXPTSceneImporter RTXPTComputePass
        RTXPTAccumulationPass RTXPTToneMappingPass)
    set(source_files)
    foreach(source IN LISTS sources)
        list(APPEND source_files "${root}/src/${source}.cpp")
    endforeach()
    add_library(${target} STATIC ${source_files})
    target_include_directories(${target} PUBLIC "${root}/src")
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_link_libraries(${target} PUBLIC ${RTXPT_DEPENDENCIES})
    target_compile_definitions(${target} PRIVATE RTXPT_SHARC_SHADER_DIR="${RTXPT_SHARC_SHADER_DIR}")
    if(RTXPT_NO_UI)
        target_compile_definitions(${target} PRIVATE RTXPT_NO_UI=1)
    endif()
    if(RTXPT_PATH_TRACING_ONLY)
        # This affects public class layouts and must reach every consumer.
        target_compile_definitions(${target} PUBLIC RTXPT_PATH_TRACING_ONLY=1)
    endif()
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

# Staging runs on every build so shader-only edits cannot leave stale files.
function(rtxpt_stage_shaders target destination)
    get_property(root GLOBAL PROPERTY RTXPT_LIBRARY_ROOT)
    add_custom_target(${target}-RTXPTShaders
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${root}/shaders" "${destination}"
        VERBATIM)
    add_dependencies(${target} ${target}-RTXPTShaders)
endfunction()

function(rtxpt_install_shaders destination)
    get_property(root GLOBAL PROPERTY RTXPT_LIBRARY_ROOT)
    install(DIRECTORY "${root}/shaders/" DESTINATION "${destination}" ${ARGN})
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
