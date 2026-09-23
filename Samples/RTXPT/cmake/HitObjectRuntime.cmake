# Pin the runtime and compiler together: native HitObject requires Shader Model 6.9.
# Keep this local to RTXPT; other samples may still use the DirectSR preview SDK.
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(rtxpt_agility
    URL https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.619.0
    URL_HASH SHA256=d5edab9a0c4d1b78ba6fe55b425eeef9fefba7d2a101e889d70fd21d481e6cb1
)
FetchContent_Declare(rtxpt_dxc
    URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602.24/dxc_2026_05_27.zip
    URL_HASH SHA256=cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4
)
FetchContent_MakeAvailable(rtxpt_agility rtxpt_dxc)
target_sources(RTXPT PRIVATE src/RTXPTAgilitySDK.cpp)
set(RTXPT_AGILITY_DLLS
    "${rtxpt_agility_SOURCE_DIR}/build/native/bin/x64/D3D12Core.dll"
    "${rtxpt_agility_SOURCE_DIR}/build/native/bin/x64/D3D12StateObjectCompiler.dll"
    "${rtxpt_agility_SOURCE_DIR}/build/native/bin/x64/d3d12SDKLayers.dll"
)
if(DILIGENT_DSR_SUPPORTED)
    list(APPEND RTXPT_AGILITY_DLLS "${DIRECTSR_BIN_DIR}/DirectSR.dll")
endif()
set(RTXPT_DXC_DLLS "${rtxpt_dxc_SOURCE_DIR}/bin/x64/dxcompiler.dll" "${rtxpt_dxc_SOURCE_DIR}/bin/x64/dxil.dll")
add_custom_command(TARGET RTXPT POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:RTXPT>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RTXPT_AGILITY_DLLS} "$<TARGET_FILE_DIR:RTXPT>/D3D12"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RTXPT_DXC_DLLS} "$<TARGET_FILE_DIR:RTXPT>"
    VERBATIM
)
if(DILIGENT_INSTALL_SAMPLES)
    install(FILES ${RTXPT_AGILITY_DLLS} DESTINATION "${CMAKE_INSTALL_BINDIR}/${RTXPT_REL_PATH}/$<CONFIG>/D3D12")
    install(FILES ${RTXPT_DXC_DLLS} DESTINATION "${CMAKE_INSTALL_BINDIR}/${RTXPT_REL_PATH}/$<CONFIG>")
endif()
