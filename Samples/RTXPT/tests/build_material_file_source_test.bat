@echo off
rem Run in a VS 2022 x64 Native Tools prompt. %1 is a DiligentEngine install directory (the target of its install
rem build), %2 its configuration, Debug or Release.
setlocal
if "%~2"=="" (
 echo usage: build_material_file_source_test.bat ^<DiligentEngine install directory^> ^<Debug^|Release^>
 exit /b 1
)
set RTXPT_TEST_INSTALL=%~f1
set RTXPT_TEST_CONFIG=%~2
if /i "%RTXPT_TEST_CONFIG%"=="Debug" (
 set RTXPT_TEST_FLAGS=/MDd /Od /Zi /D_DEBUG /DDILIGENT_DEBUG
 set RTXPT_TEST_SUFFIX=d
) else (
 set RTXPT_TEST_FLAGS=/MD /O2 /DNDEBUG
 set RTXPT_TEST_SUFFIX=
)
pushd "%~dp0..\..\..\.."
set RTXPT_TEST_OUTPUT=%TEMP%\MaterialFileSourceTest
if not exist "%RTXPT_TEST_OUTPUT%" mkdir "%RTXPT_TEST_OUTPUT%"
set RTXPT_TEST_LIB=%RTXPT_TEST_INSTALL%\lib
cl /nologo /std:c++17 /EHsc /bigobj %RTXPT_TEST_FLAGS% /DPLATFORM_WIN32=1 /DNOMINMAX ^
 /IDiligentCore\Primitives\interface /IDiligentCore\Common\interface /IDiligentCore\Platforms\interface ^
 /IDiligentCore\Platforms\Basic\interface /IDiligentCore\Graphics\GraphicsEngine\interface ^
 /IDiligentCore\Graphics\GraphicsAccessories\interface /IDiligentCore\Graphics\GraphicsTools\interface ^
 /IDiligentTools\AssetLoader\interface /IDiligentTools\TextureLoader\interface ^
 /IDiligentTools\ThirdParty\json\single_include\nlohmann ^
 DiligentSamples\Samples\RTXPT\tests\MaterialFileSourceTest.cpp DiligentSamples\Samples\RTXPT\src\RTXPTScene.cpp ^
 DiligentSamples\Samples\RTXPT\src\RTXPTSceneGraph.cpp DiligentSamples\Samples\RTXPT\src\RTXPTSceneJson.cpp ^
 DiligentSamples\Samples\RTXPT\src\RTXPTMaterials.cpp ^
 /Fo"%RTXPT_TEST_OUTPUT%\\" /Fd"%RTXPT_TEST_OUTPUT%\\" /Fe"%RTXPT_TEST_OUTPUT%\MaterialFileSourceTest.exe" ^
 /link user32.lib comdlg32.lib "/LIBPATH:%RTXPT_TEST_LIB%\DiligentCore\%RTXPT_TEST_CONFIG%" "/LIBPATH:%RTXPT_TEST_LIB%\DiligentTools\%RTXPT_TEST_CONFIG%" ^
 DiligentCore.lib DiligentTools.lib xxhash.lib LibJpeg.lib LibTiff.lib ZLib.lib libpng16_static%RTXPT_TEST_SUFFIX%.lib
set RTXPT_TEST_RESULT=%ERRORLEVEL%
if %RTXPT_TEST_RESULT%==0 echo Built %RTXPT_TEST_OUTPUT%\MaterialFileSourceTest.exe; run it with ^<D3D12^|Vk^> ^<engine DLL^>
popd
exit /b %RTXPT_TEST_RESULT%
