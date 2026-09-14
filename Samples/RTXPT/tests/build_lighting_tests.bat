@echo off
rem Run in a VS 2022 x64 Native Tools prompt after building RTXPT Release.
setlocal
pushd "%~dp0..\..\..\.."
set RTXPT_TEST_OUTPUT=%TEMP%\RTXPTLightingTests
if not exist "%RTXPT_TEST_OUTPUT%" mkdir "%RTXPT_TEST_OUTPUT%"
set RTXPT_TEST_INCLUDES=/IDiligentCore\Platforms\Basic\interface /IDiligentCore\Primitives\interface /IDiligentCore\Common\interface /IDiligentCore\Graphics\GraphicsEngine\interface /IDiligentCore\Graphics\GraphicsTools\interface /IDiligentCore\Graphics\GraphicsAccessories\interface
cl /nologo /c /std:c++17 /EHsc /MD /O2 /Gy /DPLATFORM_WIN32=1 /DNOMINMAX %RTXPT_TEST_INCLUDES% ^
 DiligentSamples\Samples\RTXPT\src\RTXPTLightProxyBuildPass.cpp /Fo"%RTXPT_TEST_OUTPUT%\RTXPTLightProxyBuildPass.obj"
if errorlevel 1 goto failed
for %%T in (LocalLightSamplingTest EnvironmentLightSamplingTest LightFeedbackTest) do (
 cl /nologo /std:c++17 /EHsc /MD /O2 /Gy /DPLATFORM_WIN32=1 /DNOMINMAX %RTXPT_TEST_INCLUDES% ^
  DiligentSamples\Samples\RTXPT\tests\%%T.cpp /Fo"%RTXPT_TEST_OUTPUT%\%%T.obj" /Fe"%RTXPT_TEST_OUTPUT%\%%T.exe" ^
  /link /OPT:REF "%RTXPT_TEST_OUTPUT%\RTXPTLightProxyBuildPass.obj" user32.lib ^
  build\x64\Release\DiligentCore\Common\Release\Diligent-Common.lib ^
  build\x64\Release\DiligentCore\Platforms\Basic\Release\Diligent-BasicPlatform.lib ^
  build\x64\Release\DiligentCore\Platforms\Win32\Release\Diligent-Win32Platform.lib
 if errorlevel 1 goto failed
)
echo Built lighting tests in %RTXPT_TEST_OUTPUT%
popd
exit /b 0
:failed
popd
exit /b 1
