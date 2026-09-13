# RR input and resource contract GPU regression

`RRPrepareInputsTest.cpp` exercises the production resource owner, frame adapter,
C++ prepare wrapper and mode 9 shader on D3D12 or Vulkan. It reads all six outputs.
It requires an NGX-capable device for real Native/Quality source-size queries,
but does not evaluate RR or require a swapchain or external scene assets.

Build RTXPT and the NGX-enabled `DiligentCoreAPITest` target first. The latter's
Debug directory contains the engine, RR/NGX, DXC and `nvngx_dlssd` runtime DLLs.
From an x64 VS 2022 Developer Command Prompt, at the superproject root:

```bat
cl /nologo /std:c++17 /EHsc /MDd /D_DEBUG /DDEVELOPMENT /DPLATFORM_WIN32=1 /DNOMINMAX /DRTXPT_HAS_NRD=0 ^
 /IDiligentCore\Platforms\Basic\interface /IDiligentCore\Primitives\interface /IDiligentCore\Common\interface ^
 /IDiligentCore\Graphics\GraphicsEngine\interface /IDiligentCore\Graphics\GraphicsTools\interface /IDiligentCore\Graphics\GraphicsAccessories\interface ^
 DiligentSamples\Samples\RTXPT\tests\RRPrepareInputsTest.cpp DiligentSamples\Samples\RTXPT\src\RTXPTRenderTargets.cpp DiligentSamples\Samples\RTXPT\src\RTXPTPostProcessPass.cpp ^
 /Fo"%TEMP%\\" /Fe"%TEMP%\RRPrepareInputsTest.exe" /link user32.lib ^
 build\x64\Debug\DiligentCore\Common\Debug\Diligent-Common.lib ^
 build\x64\Debug\DiligentCore\Platforms\Basic\Debug\Diligent-BasicPlatform.lib ^
 build\x64\Debug\DiligentCore\Platforms\Win32\Debug\Diligent-Win32Platform.lib
if errorlevel 1 exit /b 1

set RTXPT_TEST_ROOT=%CD%
pushd build\x64\Debug\DiligentCore\Tests\DiligentCoreAPITest\Debug
"%TEMP%\RRPrepareInputsTest.exe" "%CD%\GraphicsEngineD3D12_64d.dll" "%RTXPT_TEST_ROOT%\DiligentSamples\Samples\RTXPT\shaders" "%CD%\RayReconstruction_64d.dll"
"%TEMP%\RRPrepareInputsTest.exe" "%CD%\GraphicsEngineVk_64d.dll" "%RTXPT_TEST_ROOT%\DiligentSamples\Samples\RTXPT\shaders" "%CD%\RayReconstruction_64d.dll"
popd
```

Check each run's exit code. The harness enables graphics validation, explicitly
enables RR extensions before Vulkan device creation, and fails on graphics error
callbacks. It compiles the direct RR shader before changing directories for the
production wrapper, so the backend loads DXC from the runtime directory first.
No persistent shader cache is used; the optional cache parameter uses direct
device PSO/shader creation in this test.

The first ten shader cases cover:

- no valid planes / black background and minimum albedo;
- emissive sky with the user brightness clamp disabled;
- a surface with known diffuse/specular albedo and roughness;
- enabled max-channel brightness clamp;
- NaN, infinity, negative radiance and NaN roughness;
- mixed roughness and summed noisy radiance across two planes;
- opposite normals with equal guide weights;
- missing primary plane and out-of-range dominant index;
- zero active planes;
- a valid branch with the sky scene-length marker.

Four additional analytic planar-scene cases exercise the same camera ray and
motion helpers used by BUILD. They verify world-space plane Z=3 after ray-distance
conversion, object motion, camera motion, rejection of previous points behind the
camera, temporal jitter, micro-jitter, aperture sampling and near-plane offset.
Expected motion is checked in render pixels with positive X right/positive Y down.

Every case checks finite outputs, unit normal length within fp16 tolerance,
bounded albedo and linear roughness. One-pixel inputs use an 8x8 dispatch to
exercise the production bounds guard. The fixtures use the existing 80-byte
StablePlane packing; they do not reimplement the RR preparation algorithm.

`RRResourceContractTest.hpp` additionally checks:

- Native 640x360, queried Quality 427x240 -> 640x360, resized Quality
  512x288 -> 768x432, and return to Native on the tested RTX 5060;
- complete 2D SRV/UAV format/extents, one mip/sample, distinct inputs/output;
- reuse on unchanged requests, recreation on resize, RR/NRD mode switching and reset;
- packed roughness, linear depth, motion/jitter flags, exposure values, untransposed
  row-major matrices, optional-guide clearing and stale-frame rejection;
- two calls through `RunDLSSRRDenoiserPrepareInputs`, GPU readback, output SRV
  transitions, rebinding after cleanup, and rejection of aliased outputs.

Validation on RTX 5060: both backends pass the resource checks, production wrapper
and 14 readback cases. D3D12 also checks RR -> NRD -> RR allocation with NRD's
RGB10A2 encoding preserved. The tested Vulkan backend does not support the existing
RGB10A2 UAV, so it reports that NRD switch as skipped and tests RR -> reference -> RR
instead. This existing NRD limit does not block RR resources. The existing DXC
`rgb10_a2` annotation warning in NRD shader declarations is not a new validation
error; the test does not dispatch the NRD fallback shader variants.

The one-pixel wrapper fixtures upload eight-byte texels before four-byte texels
to keep Vulkan upload-buffer offsets aligned. They do not test arbitrary mixed
format upload ordering in Core.

Actual NGX evaluation, interactive window resize, history behavior, full BUILD/FILL
scene integration and final RR image quality remain #20 validation. These tests
validate the resource/parameter boundary and cannot replace those checks.
