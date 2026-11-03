@echo off
rem Run in a VS 2022 x64 Native Tools prompt. Arguments: CMake build root, configuration.
setlocal
if "%~2"=="" exit /b 2
set RTXPT_TEST_BUILD=%~1
set RTXPT_TEST_CONFIG=%~2
set RTXPT_TEST_OUTPUT=%TEMP%\NrdHitDistanceTest
if not exist "%RTXPT_TEST_OUTPUT%" mkdir "%RTXPT_TEST_OUTPUT%"
set RTXPT_TEST_RUNTIME=/MD
if /I "%RTXPT_TEST_CONFIG%"=="Debug" set RTXPT_TEST_RUNTIME=/MDd /D_DEBUG
pushd "%~dp0..\..\..\.."
cl /nologo /std:c++17 /EHsc %RTXPT_TEST_RUNTIME% /O2 /DPLATFORM_WIN32=1 /DNOMINMAX ^
 /IDiligentCore\Platforms\Basic\interface /IDiligentCore\Primitives\interface ^
 /IDiligentCore\Common\interface /IDiligentCore\Graphics\GraphicsEngine\interface ^
 DiligentSamples\Samples\RTXPT\tests\NrdHitDistanceTest.cpp ^
 /Fo"%RTXPT_TEST_OUTPUT%\NrdHitDistanceTest.obj" /Fe"%RTXPT_TEST_OUTPUT%\NrdHitDistanceTest.exe" ^
 /link user32.lib "%RTXPT_TEST_BUILD%\DiligentCore\Common\%RTXPT_TEST_CONFIG%\Diligent-Common.lib" ^
 "%RTXPT_TEST_BUILD%\DiligentCore\Platforms\Basic\%RTXPT_TEST_CONFIG%\Diligent-BasicPlatform.lib" ^
 "%RTXPT_TEST_BUILD%\DiligentCore\Platforms\Win32\%RTXPT_TEST_CONFIG%\Diligent-Win32Platform.lib"
set RTXPT_TEST_RESULT=%ERRORLEVEL%
popd
exit /b %RTXPT_TEST_RESULT%
