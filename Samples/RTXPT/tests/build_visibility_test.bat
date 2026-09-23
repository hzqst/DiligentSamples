@echo off
rem Run in a VS 2022 x64 Native Tools prompt after building RTXPT Release.
setlocal
pushd "%~dp0..\..\..\.."
set RTXPT_TEST_OUTPUT=%TEMP%\RTXPTVisibilityTest
if not exist "%RTXPT_TEST_OUTPUT%" mkdir "%RTXPT_TEST_OUTPUT%"
set RTXPT_TEST_INCLUDES=/IDiligentCore\Platforms\interface /IDiligentCore\Platforms\Basic\interface /IDiligentCore\Primitives\interface /IDiligentCore\Common\interface /IDiligentCore\Graphics\GraphicsEngine\interface /IDiligentCore\Graphics\GraphicsTools\interface /IDiligentCore\Graphics\GraphicsAccessories\interface /IDiligentTools\AssetLoader\interface /IDiligentTools\TextureLoader\interface /IDiligentTools\ThirdParty\json\single_include\nlohmann
cl /nologo /c /std:c++17 /EHsc /MD /O2 /Gy /GL /DPLATFORM_WIN32=1 /DNOMINMAX %RTXPT_TEST_INCLUDES% ^
 DiligentSamples\Samples\RTXPT\src\RTXPTMaterials.cpp /Fo"%RTXPT_TEST_OUTPUT%\RTXPTMaterials.obj"
if errorlevel 1 goto failed
cl /nologo /c /std:c++17 /EHsc /MD /O2 DiligentSamples\Samples\RTXPT\src\RTXPTAgilitySDK.cpp /Fo"%RTXPT_TEST_OUTPUT%\RTXPTAgilitySDK.obj"
if errorlevel 1 goto failed
cl /nologo /std:c++17 /EHsc /MD /O2 /Gy /GL /DPLATFORM_WIN32=1 /DNOMINMAX %RTXPT_TEST_INCLUDES% ^
 DiligentSamples\Samples\RTXPT\tests\VisibilityRayTest.cpp ^
 /Fo"%RTXPT_TEST_OUTPUT%\VisibilityRayTest.obj" /Fe"%RTXPT_TEST_OUTPUT%\VisibilityRayTest.exe" ^
 /link /OPT:REF /LTCG "%RTXPT_TEST_OUTPUT%\RTXPTMaterials.obj" "%RTXPT_TEST_OUTPUT%\RTXPTAgilitySDK.obj" user32.lib comdlg32.lib ^
 build\x64\Release\DiligentTools\TextureLoader\Release\Diligent-TextureLoader.lib ^
 build\x64\Release\DiligentTools\ThirdParty\libpng\Release\libpng16_static.lib ^
 build\x64\Release\DiligentTools\ThirdParty\libtiff\Release\LibTiff.lib ^
 build\x64\Release\DiligentTools\ThirdParty\Release\ZLib.lib ^
 build\x64\Release\DiligentTools\ThirdParty\libjpeg-9e\Release\LibJpeg.lib ^
 build\x64\Release\DiligentCore\Graphics\GraphicsAccessories\Release\Diligent-GraphicsAccessories.lib ^
 build\x64\Release\DiligentCore\Graphics\GraphicsTools\Release\Diligent-GraphicsTools.lib ^
 build\x64\Release\DiligentCore\Graphics\GraphicsEngineVulkan\Release\Diligent-GraphicsEngineVk-static.lib ^
 build\x64\Release\DiligentCore\Graphics\GraphicsEngineOpenGL\Release\Diligent-GraphicsEngineOpenGL-static.lib ^
 build\x64\Release\DiligentCore\Graphics\GraphicsEngineD3DBase\Release\Diligent-GraphicsEngineD3DBase.lib ^
 build\x64\Release\DiligentCore\Common\Release\Diligent-Common.lib ^
 build\x64\Release\DiligentCore\Platforms\Basic\Release\Diligent-BasicPlatform.lib ^
 build\x64\Release\DiligentCore\Platforms\Win32\Release\Diligent-Win32Platform.lib
if errorlevel 1 goto failed
set RTXPT_RUNTIME=%CD%\build\x64\Release\DiligentSamples\Samples\RTXPT\Release
if not exist "%RTXPT_TEST_OUTPUT%\D3D12" mkdir "%RTXPT_TEST_OUTPUT%\D3D12"
copy /y "%RTXPT_RUNTIME%\D3D12\*.dll" "%RTXPT_TEST_OUTPUT%\D3D12\" >nul
if errorlevel 1 goto failed
copy /y "%RTXPT_RUNTIME%\dxcompiler.dll" "%RTXPT_TEST_OUTPUT%\" >nul
if errorlevel 1 goto failed
copy /y "%RTXPT_RUNTIME%\dxil.dll" "%RTXPT_TEST_OUTPUT%\" >nul
if errorlevel 1 goto failed
echo Built %RTXPT_TEST_OUTPUT%\VisibilityRayTest.exe
popd
exit /b 0
:failed
popd
exit /b 1
