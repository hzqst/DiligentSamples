@echo off
rem Run in a VS 2022 x64 Native Tools prompt. Builds and runs the device-free RTXPTSceneRuntime test.
setlocal
pushd "%~dp0..\..\..\.."
set RTXPT_TEST_OUTPUT=%TEMP%\RTXPTSceneRuntimeTest
if not exist "%RTXPT_TEST_OUTPUT%" mkdir "%RTXPT_TEST_OUTPUT%"
cl /nologo /std:c++17 /EHsc /MDd /Od /Zi /W4 /D_DEBUG /DPLATFORM_WIN32=1 /DNOMINMAX ^
 /IDiligentCore\Primitives\interface /IDiligentCore\Common\interface /IDiligentCore\Platforms\interface ^
 /IDiligentCore\Platforms\Basic\interface /IDiligentCore\Graphics\GraphicsEngine\interface ^
 DiligentSamples\Samples\RTXPT\tests\SceneRuntimeTest.cpp DiligentSamples\Samples\RTXPT\src\RTXPTSceneRuntime.cpp ^
 /Fo"%RTXPT_TEST_OUTPUT%\\" /Fd"%RTXPT_TEST_OUTPUT%\\" /Fe"%RTXPT_TEST_OUTPUT%\SceneRuntimeTest.exe"
set RTXPT_TEST_RESULT=%ERRORLEVEL%
popd
if not %RTXPT_TEST_RESULT%==0 exit /b %RTXPT_TEST_RESULT%
"%RTXPT_TEST_OUTPUT%\SceneRuntimeTest.exe"
exit /b %ERRORLEVEL%
