# RR input preparation GPU regression

`RRPrepareInputsTest.cpp` compiles and dispatches the production
`PostProcessing/RTXPTPostProcess.csh` RR variant on D3D12, then reads the four
RGBA16F outputs. It does not require NGX, an RR model, a swapchain, or scene assets.

Build RTXPT first so its Diligent engine DLL is available. From an x64 VS 2022
Developer Command Prompt, at the superproject root:

```bat
cl /nologo /std:c++17 /EHsc /MDd /DPLATFORM_WIN32=1 /DNOMINMAX /IDiligentCore\Primitives\interface /IDiligentCore\Common\interface /IDiligentCore\Graphics\GraphicsEngine\interface DiligentSamples\Samples\RTXPT\tests\RRPrepareInputsTest.cpp /Fo"%TEMP%\RRPrepareInputsTest.obj" /Fe"%TEMP%\RRPrepareInputsTest.exe"
"%TEMP%\RRPrepareInputsTest.exe" "%CD%\build\x64\Debug\DiligentCore\Graphics\GraphicsEngineD3D12\Debug\GraphicsEngineD3D12_64d.dll" "%CD%\DiligentSamples\Samples\RTXPT\shaders"
```

The engine must be able to locate `dxcompiler.dll` (the standard RTXPT build
environment provides it). Exit code 0 means all ten cases passed:

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

Every case checks finite outputs, unit normal length within fp16 tolerance,
bounded albedo and linear roughness. One-pixel inputs use an 8x8 dispatch to
exercise the production bounds guard. The fixtures use the existing 80-byte
StablePlane packing; they do not reimplement the RR preparation algorithm.

This test covers shader input preparation, not the C++ dispatch wrapper,
path-traced scene production, camera-motion behavior, Vulkan execution or final
RR image quality. Those require the frame/resource integration in #19/#20.
