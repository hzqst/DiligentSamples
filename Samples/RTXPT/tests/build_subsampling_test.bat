@echo off
rem Run in a VS 2022 x64 Native Tools prompt after building RTXPT Release.
setlocal
pushd "%~dp0..\..\..\.."
set RTXPT_TEST_OUTPUT=%TEMP%\SubSamplingTest
if not exist "%RTXPT_TEST_OUTPUT%" mkdir "%RTXPT_TEST_OUTPUT%"
cl /nologo /std:c++17 /EHsc /MD /O2 /DPLATFORM_WIN32=1 /DNOMINMAX ^
 /IDiligentCore\Platforms\Basic\interface /IDiligentCore\Primitives\interface ^
 /IDiligentCore\Common\interface /IDiligentCore\Graphics\GraphicsEngine\interface ^
 DiligentSamples\Samples\RTXPT\tests\SubSamplingTest.cpp ^
 /Fo"%RTXPT_TEST_OUTPUT%\SubSamplingTest.obj" /Fe"%RTXPT_TEST_OUTPUT%\SubSamplingTest.exe" ^
 /link user32.lib build\x64\Release\DiligentCore\Common\Release\Diligent-Common.lib ^
 build\x64\Release\DiligentCore\Platforms\Basic\Release\Diligent-BasicPlatform.lib ^
 build\x64\Release\DiligentCore\Platforms\Win32\Release\Diligent-Win32Platform.lib
set RTXPT_TEST_RESULT=%ERRORLEVEL%
popd
exit /b %RTXPT_TEST_RESULT%
