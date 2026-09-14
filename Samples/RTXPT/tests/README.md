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

These resource tests do not replace the real-scene validation below.

`GeometryVertexLayoutTest.hpp` also runs the production skinning shader on two
distinct CPU vertices with the default 32-byte position/normal/UV layout. GPU
readback checks every component after a known joint translation. The old Vulkan
shader fails this test; the packed storage layout passes on both backends. This
covers vertex array stride as well as member offsets, without testing generated
SPIR-V text.

## Real-scene execution (#20 / #21)

Build `RTXPT` with RR enabled (the Core default). The target deploys the shared
NGX runtime and `nvngx_dlssd.dll` alongside SR's existing runtime. To exercise the
shared RR loader, configure `RTXPT_RAY_RECONSTRUCTION_SHARED=ON`. To check the
unavailable build, configure `DILIGENT_NO_RAY_RECONSTRUCTION=ON` and rebuild.

Run from the sample directory or its deployed runtime directory, for example:

```bat
RTXPT.exe --mode d3d12 --width 640 --height 360 --realtime true --aa rr --rr_quality quality --rr_preset F
RTXPT.exe --mode vk --width 640 --height 360 --realtime true --aa rr --rr_quality native --rr_preset E
```

`--scene convergence-test.scene.json` selects the reflection/transmission test
scene explicitly. `--aa off|taa|sr|rr`, `--realtime true|false` and `--nrd true|false`
provide reproducible baseline runs. Vulkan requests RR extensions before device
creation; `--vk_rr false` launches without that request and must leave RR
unavailable. F is the default on both backends: selecting Vulkan E is explicit.
The known Vulkan F SDK layout failure is not a passing configuration.

`--rr_validation true` opts into `RRSceneValidation.hpp`, which drives the real
sample for eleven 32-frame stages: Quality, smooth camera motion, explicit history
reset, Native, native-window resize to 768x432, reference accumulation, TAA,
ordinary SR, standalone NRD, return to Quality RR, and a lighting/preset change.
D3D12 starts at F and explicitly changes to E in the final stage. Vulkan should
be launched with E; its existing NRD RGB10A2 UAV limitation means the three
NRD/TAA/SR stages are reported as skipped and remain reference-mode stages.
Automatic window resize is implemented for Windows; other platforms require a
manual resize check. Windows resizing goes through the actual window's WM_SIZE
path, preserving the framework's window/ImGui/swap-chain dimension contract.

The sequence reports failed production frames, RR create/execute/reset counts,
and stage wall time per frame (including stage setup/compilation). It keeps the
window running afterwards for inspection; close normally to exercise shutdown.
Inspect graphics validation logs as well as the sequence counters. For captures,
use SampleBase's `--capture_path`, `--capture_name`, `--capture_frames`,
`--capture_fps` and `--capture_format png` options. Capture comparison must still
assess temporal artifacts and exposure; successful API returns alone do not
establish image-quality equivalence.

### Integration validation record (2026-09-13/14, RTX 5060)

- Debug RR enabled, Debug RR disabled, and Release builds succeeded. The disabled
  build rendered the D3D12 reference scene. Static and shared factory evaluation
  with queued barriers is covered by the Core regression.
- D3D12 F ran 14,983 consecutive real-scene RR evaluations with one history reset.
  The final 352-frame mode sequence reported zero failed frames, 224 RR
  evaluations, five objects and six history resets, including TAA/SR/NRD round trips.
- Vulkan E completed the final sequence with zero failed frames, 224 RR evaluations,
  four objects and six resets. NRD/TAA/SR stages were explicitly skipped for the
  existing RGB10A2 UAV limitation. No SDK F layout errors are suppressed; Vulkan F
  remains an unpassed configuration.
- A new queued-barrier test reproduced `0xc0000005` before the Core Vulkan
  provider fix. After the fix, all five Core RR tests passed on Vulkan E.
- Removing the deployed `nvngx_dlssd.dll` made RR unavailable without crashing;
  D3D12 NRD+TAA continued to render. The DLL was restored after the test.
- D3D12 captures show substantial RR noise reduction and some small-highlight
  smoothing. Mean 8-bit RGB intensity stabilized at about 163 for RR F and 161
  for NRD+TAA, versus 139 for reference accumulation. These are rendered-image
  observations, not a linear-radiance equivalence metric or an image-quality pass.
- Final Debug sequence wall times were 14.4 ms/frame during D3D12 camera
  motion and 16.8 ms/frame after returning to Quality RR; Vulkan E measured 17.3
  and 22.8 ms/frame respectively. These include capture, whole-frame work and stage setup,
  and are not isolated RR GPU timings. Resize/first-use shader compilation costs
  are included in their respective stage logs and should not be compared as steady
  rendering performance.

The initial Vulkan black triangles were traced to a separate vertex-buffer ABI
bug before NGX: the original no-denoiser merge and prepared noisy radiance both
already contained the artifacts. CPU offsets 0/12/24 and stride 32 did not match
DXC SPIR-V offsets 0/16/32 and stride 48. `PackedGeometryVertexData` now fixes the
storage layout to two float4 values, shared by hit, emissive and skinning shaders.
The old shader failed GPU readback; the fixed shader passes on both backends, and
Vulkan Native E / Quality E captures no longer show those black triangles.

**Remaining validation blocker:** directly starting Vulkan reference rendering in
the room still triggers `VK_ERROR_DEVICE_LOST` while capturing output, including
a post-layout-fix run with `--realtime false --aa off --vk_rr false`.
An isolated baseline built from
the #19 `c1124d68` Sample and post-process scheduler (with only reference mode
selected at startup) reproduces the same failure. Its first errors occur in
`FenceVkImpl::Wait`, followed by invalid-fence/command-buffer reports after device
loss. The underlying startup/reference failure is not diagnosed by those
follow-on errors. In contrast, the final RR -> reference -> RR sequence succeeds
with the camera kept inside the room. Full Vulkan image-quality comparison from
an independently started reference capture therefore remains open.
Per the execution-scope decision on 2026-09-14, this pre-existing failure is
tracked separately and remains an unpassed #20 acceptance item for this delivery.

The initial resize test changed only the swap chain and triggered an ImGui/window
size assertion. The harness now resizes the actual Win32 window; reruns above use
that corrected path. Final sequences also use the reduced 0.0025-unit camera step
to keep the camera inside the room rather than crossing a wall.

## Local light sampling regression and GPU profiling

`LocalLightSamplingTest.cpp` dispatches the production `FillLocalSamplingCS`,
then the production `SampleLocalLightIndex`, `SampleLocalPDF`, and
`GetLightSelectionPdf`. An independent CPU histogram verifies the 128-entry
multiset, sorted order, packed multiplicity, selected entries and PDFs. Cases
include one light repeated 128 times, 128 unique lights, duplicates, absent
lights, global-only/local-only/mixed PDFs, partial edge tiles, equal-weight
feedback, invalid feedback and zero proxies.

Build from an x64 VS 2022 Developer Command Prompt at the superproject root:

```bat
cl /nologo /std:c++17 /EHsc /MD /O2 /DPLATFORM_WIN32=1 /DNOMINMAX ^
 /IDiligentCore\Platforms\Basic\interface /IDiligentCore\Primitives\interface /IDiligentCore\Common\interface ^
 /IDiligentCore\Graphics\GraphicsEngine\interface /IDiligentCore\Graphics\GraphicsTools\interface /IDiligentCore\Graphics\GraphicsAccessories\interface ^
 DiligentSamples\Samples\RTXPT\tests\LocalLightSamplingTest.cpp ^
 /Fo"%TEMP%\LocalLightSamplingTest.obj" /Fe"%TEMP%\LocalLightSamplingTest.exe" /link user32.lib ^
 build\x64\Release\DiligentCore\Common\Release\Diligent-Common.lib ^
 build\x64\Release\DiligentCore\Platforms\Basic\Release\Diligent-BasicPlatform.lib ^
 build\x64\Release\DiligentCore\Platforms\Win32\Release\Diligent-Win32Platform.lib
set RTXPT_TEST_ROOT=%CD%
pushd build\x64\Release\DiligentSamples\Samples\RTXPT\Release
"%TEMP%\LocalLightSamplingTest.exe" "%CD%\GraphicsEngineD3D12_64r.dll" "%RTXPT_TEST_ROOT%\DiligentSamples\Samples\RTXPT\shaders"
"%TEMP%\LocalLightSamplingTest.exe" "%CD%\GraphicsEngineVk_64r.dll" "%RTXPT_TEST_ROOT%\DiligentSamples\Samples\RTXPT\shaders"
popd
```

Opt-in GPU timing (use an absolute CSV path; omit it for console output only):

```bat
RTXPT.exe --mode d3d12 --realtime true --aa rr --rr_quality balanced --rr_preset F --width 1920 --height 1080 --vsync false --validation 0 --gpu_profile true --gpu_profile_csv "%TEMP%\rtxpt-gpu.csv"
```

Separate timestamp pairs measure total sample rendering, BUILD, LightsBaker,
FILL (all sub-samples), shared guides, RR preparation, native RR and
bloom/tone mapping/blit. Total excludes SampleBase ImGui, Present and work
scheduled by Update. Native RR includes its transitions and command submission.
No duration query is held open across NGX. An eight-frame ring reads completed
queries without GPU waits; if full, it drops a profiling sample instead of
stalling rendering. Console means are reported every 120 collected frames; the
CSV contains every collected frame (`-1` means absent/unavailable). Exclude
startup and scene/config changes when measuring steady state.

Shader edits require a fresh cache because the sample hashes file names rather
than contents. The local-table change advances the runtime cache to
`RTXPTCache_002.bin`, preventing reuse of the incompatible old producer/consumer.
The subsequent inline/upstream-alpha change advances it to `RTXPTCache_005.bin`.

Validation on 2026-09-14: the test first failed on the old producer's incorrect
multiplicity. All five producer/consumer cases passed on D3D12 and Vulkan after
the fix, with graphics validation enabled.

### Performance investigation record (RTX 5060, 2026-09-14)

Release/D3D12, convergence-test, 1920x1080 output, RR Balanced/F (1114x626
render), validation and VSync disabled: the original whole-frame measurement was
about 71 ms. After the local-table fix, 612 GPU samples after frame 240 averaged:

| Stage | GPU ms |
|---|---:|
| Total sample rendering | 28.212 |
| BUILD | 0.317 |
| LightsBaker | 0.498 |
| FILL | 23.969 |
| Guides | 0.28 |
| RR preparation | 0.21 |
| RR evaluation | 2.807 |
| Post-process / blit | 0.10 |

Independent temporary shader ablations localized the remaining FILL cost:

| Diagnostic variant | Total GPU ms | FILL GPU ms |
|---|---:|---:|
| All NEE disabled | 5.942 | 1.699 |
| Environment NEE disabled | 17.347 | 12.957 |
| One direct-light candidate instead of five | 21.451 | 16.990 |
| Inline RayQuery shadows with translated alpha handling | 18.145 | 13.768 |
| Early rejection of delta-only NEE | 29.152 | 24.819 |

These ablations are not production changes or additive pass costs: they also
change shader register allocation/scheduling, and disabling NEE changes the
estimator. This temporary inline prototype retained the port's old alpha filter;
the production implementation and its regression coverage are described below. SER and
material-specialized shading differences are not yet quantitatively attributed.
The user-provided upstream frame time (~7.8 ms) was not remeasured in this run.

Final validation-enabled runs also exercised timestamp collection across native
RR on D3D12/F at 1080p and Vulkan/E at 640x360, with no reported graphics errors
or dropped profiling samples. The existing Vulkan NRD `rgb10_a2` shader warnings
and D3D12 missing-PIX-runtime warning remain. D3D12 PNG capture was inspected for
the expected scene and RR output; this is not a full temporal image-quality test.
Validation-enabled D3D12 measured about 31 ms, so compare performance with the
same validation setting. No extra GPU flush/wait is introduced by the profiler.

A subsequent run of the final executable with validation disabled collected 676
steady frames: Total 30.08 ms, BUILD 0.32, LightsBaker 0.50, FILL 25.67,
Guides 0.29, RR preparation 0.22, RR 2.95 and post-process 0.10. With profiling
also disabled, window-title samples were 30.3–31.1 ms. Report the observed
28–31 ms range (~32–35 FPS), rather than attributing the entire between-run
variation to validation or profiling. The remaining dominant cost is still FILL.

### Inline visibility / upstream alpha regression

`VisibilityRayTest.cpp` links production CPU material classification and executes
production `PathTracerVisibility.hlsli` plus both `PathTracerAnyHit.rahit` entry
points against a real BLAS/TLAS. It compares 48 rays against explicit expected
visibility, not just against the other rendering path. Coverage includes:

- Opaque hits/misses, TMin/TMax clipping, and continued traversal after alpha rejection.
- Texture alpha versus material opacity, the UNORM8 cutoff midpoint, glTF MASK,
  loaded material overrides, missing textures, and BLEND without stochastic filtering.
- `ExcludeFromNEE` ignores shadow hits while retaining primary hits, including
  CPU non-opaque classification when no texture exists.
- Custom instance IDs distinct from instance indices, nonzero geometry/primitive
  indices and buffer offsets, indexed/non-indexed geometry, and skinned UVs.
- Both 16- and 32-bit index views. Vulkan's texel-buffer format must match the
  view; undefined-value validation diagnostics fail the test.

Build RTXPT Release first with the normal Windows D3D12/Vulkan/OpenGL backends.
Then, from a VS 2022 x64 Native Tools prompt at the repository root:

```bat
call DiligentSamples\Samples\RTXPT\tests\build_visibility_test.bat
set RTXPT_TEST_ROOT=%CD%
pushd build\x64\Release\DiligentSamples\Samples\RTXPT\Release
"%TEMP%\RTXPTVisibilityTest\VisibilityRayTest.exe" "%CD%\GraphicsEngineD3D12_64r.dll" "%RTXPT_TEST_ROOT%\DiligentSamples\Samples\RTXPT\shaders"
"%TEMP%\RTXPTVisibilityTest\VisibilityRayTest.exe" "%CD%\GraphicsEngineD3D12_64r.dll" "%RTXPT_TEST_ROOT%\DiligentSamples\Samples\RTXPT\shaders" uint16
"%TEMP%\RTXPTVisibilityTest\VisibilityRayTest.exe" "%CD%\GraphicsEngineVk_64r.dll" "%RTXPT_TEST_ROOT%\DiligentSamples\Samples\RTXPT\shaders"
"%TEMP%\RTXPTVisibilityTest\VisibilityRayTest.exe" "%CD%\GraphicsEngineVk_64r.dll" "%RTXPT_TEST_ROOT%\DiligentSamples\Samples\RTXPT\shaders" uint16
popd
```

On 2026-09-14 all four configurations passed with validation enabled. Replacing
only MaterialBridge with the pre-change version in a temporary shader copy made
ray 2 fail (`expected 0, got 1`): multiplying opacity incorrectly rejected a
texture-alpha hit. This is a defect-reproduction check, not a claim that the full
GPU test preceded implementation. The sample also retains an explicit fallback:
append `--inline_shadows false` to the GPU timing command above. Without that
switch, adapters supporting inline ray tracing use RayQuery by default.

RTX 5060, D3D12 Release, convergence-test, 1920x1080, RR Balanced/F, 1114x626
render, default 1 spp and 5 light candidates, validation/VSync disabled:
after excluding frames 1–240, the first comparison measured:

| Stage | TraceRay fallback (634 frames), ms | Inline (1256 frames), ms |
|---|---:|---:|
| Total sample rendering | 30.784 | 18.272 |
| BUILD | 0.317 | 0.326 |
| LightsBaker | 0.501 | 0.506 |
| FILL | 26.403 | 13.918 |
| Guides | 0.289 | 0.282 |
| RR preparation | 0.221 | 0.212 |
| RR | 2.917 | 2.894 |
| Post-process | 0.097 | 0.097 |

Both paths use the same upstream alpha semantics. Window-title frame time was
approximately 31 ms versus 18–19 ms (about 32 versus 53 FPS). This confirms a
large FILL saving, but does not yet match the user-provided upstream ~7.8 ms.
SER and material specialization remain unmeasured hypotheses for the remaining
gap. The test covers ray filtering; it is not a full temporal RR or transmission
image-quality regression.

The final Release executable was rebuilt after the index-format correction and
all four GPU test configurations passed again using the checked-in build script.
A validation-enabled D3D12/F 1080p run collected 1267 steady frames: Total
18.111 ms, BUILD 0.318, LightsBaker 0.503, FILL 13.764, RR 2.889. The final
Vulkan/E TraceRay fallback also ran with validation at 640x360; its PNG capture
was inspected for scene/RR output. No new graphics errors were reported; the
pre-existing `rgb10_a2` shader and missing-PIX warnings remain.
