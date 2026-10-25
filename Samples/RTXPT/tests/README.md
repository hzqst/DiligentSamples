# RR input and resource contract GPU regression

## SHaRC validation (issue #23)

All path-tracing and SHaRC shaders explicitly request DXC O3, including Debug.
Do not use O0/Od for path tracing: that DXC path has a previously diagnosed
correctness bug. Engine DEFAULT maps to Od in Debug, so it is not suitable here.

Build `RTXPT` Debug/Release, then run `tests/build_sharc_test.bat` from a VS 2022
x64 Native Tools prompt. Run `%TEMP%/RTXPTSharcTest/SharcCacheTest.exe` with the
D3D12 engine DLL and the deployed `shaders` directory as arguments.
The test uses the production SHaRC bindings, clear and resolve shaders. Its
independent expectation is `(2,3,4) + (.5,.25,.125)*(4,8,16) = (4,5,6)`.
It also checks production RT training/query resource reflection, empty-cache
misses, stale eviction, explicit invalidation and the last entry of a second
dispatch row. `sharc-cache-test-final.log` and `sharc-cache-test-debug.log`
record passes with no graphics validation errors.

Scene-linear captures: `--hdr_capture_path <absolute.pfm>` averages 64 raw
RGBA16F output frames starting at frame 256, before display tone mapping.
`--hdr_capture_start` and `--hdr_capture_count` override that window. The PFM
contains bottom-up, little-endian RGB float32. Capture intentionally waits for
GPU readback; do not use these runs as timing evidence. For timings use separate
`--gpu_profile true --gpu_profile_csv <absolute.csv>` runs; SHaRC update and
resolve have separate asynchronous timestamp columns.

Fixed comparison: `--scene convergence-test.scene.json --width 640 --height 360
--realtime true --aa off --nrd false`, once with `--sharc false`, once with
`--sharc true`. Keep camera, resolution, sample window and all other settings
fixed. Compare linear HDR energy and spatial error, not UI-containing PNGs.
`--rr_validation true --sharc true` exercises the existing camera, resize,
mode/history/reload sequence. Dynamic skinning uses
`--scene ../tests/sharc-dynamic.scene.json --scene_animations true --sharc true`.

Vulkan SHaRC runtime is explicitly unsupported: the engine does not enable
64-bit shader integers/atomics. `--sharc true` must log that limitation and render
without cache; use the existing supported RR route to avoid the separately
documented RGB10A2 non-RR limitation. Offline SHaRC resolve SPIR-V compilation
passes, but is not evidence that Vulkan can execute it.

Workstation results (RTX 5060, 2026-10-26):

| Check | Result / local evidence |
| --- | --- |
| Debug / Release builds | Exit 0, `sharc-build-{debug,release}-final.log`. Both include the Vulkan backend; Vulkan SHaRC execution remains unsupported as described above. |
| Shader compilation | 24 train/query, FP16/FP32, raygen/hit/miss DXIL/SPIR-V variants pass (`sharc-shader-matrix.log`); Clear/Resolve pass both targets (`sharc-resolve-matrix.log`). All use O3. |
| GPU contracts | Both Debug and Release pass the independent radiance and lifetime expectations above. Production training must expose actual cache writes; compiling an unused SHaRC include alone is insufficient. |
| Capacity and switches | Capacity log2 16/downscale 2 and log2 24/downscale 5 exit 0 without graphics errors; disabled update/resolve also exits 0 (`sharc-capacity-{min,max}.log`, `sharc-frozen.log`). |
| Scene transitions | 19-stage D3D12 sequence reports failures=0, RR executions=448, creates=6, history resets=13 (`sharc-transitions.log`). Covers camera, resize, Reference/realtime, NRD/TAA/SR/RR, reload and lighting changes. |
| Animated geometry | BrainStem skinning/BLAS path executes and produces finite HDR (`sharc-dynamic.log`, `.pfm`). Cache invalidation is conservative every animated frame. |
| Specializations / debug | HitObject/SER, main FP32 with TraceRay shadows, and cache-radiance debug runs exit 0 without graphics errors (`sharc-hitobject.log`, `sharc-fp32.log`, `sharc-cache-debug.log`). |
| Vulkan fallback | Requested SHaRC reports unsupported and the uncached Vulkan RR route exits 0 (`sharc-vulkan.log`). This is not a SHaRC GPU pass. |
| Linear HDR | Matched TraceRay runs average frames 256..319 at 640x360. All values finite; mean luminance off/on = 0.447110 / 0.447287 (+0.0395%); RGB MAE 0.044012, RMSE 0.079965 (`sharc-hdr-metrics.json`, paired PFM/PNG). These stochastic-image differences are not a ground-truth bias bound. |
| Frame averaging | Against the uncached 64-frame estimate, RGB RMSE for 1 vs 8 frames falls from 0.6710 to 0.2389 without cache and from 0.6610 to 0.2467 with cache. All four runs exit 0 and produce finite HDR (`sharc-convergence-metrics.json`). This checks noise reduction from averaging, not asymptotic convergence against ground truth. |
| GPU time | Separate 2048-frame runs, excluding frames <=240: 1805 valid samples each. Off/on total = 4.134 / 4.330 ms (+4.75%); update 0.126 ms, resolve 0.030 ms. This scene is slower with SHaRC; no speedup is claimed (`sharc-profile-metrics.json`, paired CSV). |
| Formatting | clang-format 10 reports no replacements for the changed integration C++ files; `git diff --check` passes. |

The room, mirror/transmission paths and populated cache debug output were also
visually inspected. These tests do not establish absence of light leaks or
temporal artifacts across arbitrary scenes; long camera trajectories and a
converged ground-truth image-quality study remain unvalidated. Evidence is under
the ignored superproject `build/rtxpt-v190/sharc-*` files, not checked-in fixtures.

## v1.9.0 synchronization validation (2026-10-26)

Source target `RTXPT-fork/9503910`, local starting point `DiligentSamples/50774cdc`.
Current mapping and intentional feature gaps are in [RTXPT_FORK_MAPPING.md](../RTXPT_FORK_MAPPING.md).
Evidence from this workstation is under the superproject's ignored
`build/rtxpt-v190/` directory; these paths are local artifacts, not checked-in fixtures.

| Check | Result / evidence |
| --- | --- |
| RTXPT Debug and Release | Both builds exit 0; `build-final-debug.log`, `build-final-release.log`. |
| Formatting | Repository clang-format 10.0.0 reports no replacements for changed C++ files; `git diff --check` passes. |
| BSDF/layer/lighting contracts | D3D12 and Vulkan exit 0: `local-final-*.log`. New independent expectations cover A8 roughness, the delta threshold, sRGB8, signed world motion, position-error offset and index 2,097,151. The pre-port shader failed the new A8 expectation (`lighting-red.log`). |
| Cluster proposals | Both backends pass active/inactive LOD local+global leaf PDF tests, including a local table that contains both cluster and leaf entries. |
| Cluster production / feedback | Both backends pass far/near LOD, stale flag clearing, trimmed dark ends, all-black and small-cluster cases through production `ProcessClustersCS`; proxy counts and histograms are read back. Existing feedback/history tests also pass. |
| 2M capacity | `feedback-final-*.log`: both backends allocate 2,097,152 light descriptors and produce/read back the uniform proxy table; every light count agrees with its histogram, including the last index. |
| Environment hierarchy | Both backends pass the independent CPU oracle with 2,812 nodes and its GPU sampling/normalization checks. |
| RR / vertex ABI | `rr-final-D3D12.log`, `rr-final-Vk.log`: resource lifetime/resize/alias checks, 32-byte vertex skinning, and 15 numerical input cases pass. The production guide-baker entry reads back seven outputs, including distinct specular motion and invalid-roughness fallback. |
| Visibility | `visibility-{D3D12,Vk}-{uint32,uint16}.log`: all four runs exit 0, 48 expected rays per configuration. |
| Real-scene sequence | `integration-d3d12.log`, `integration-vk.log`: both report 19 stages, failures=0, RR executions=448, creates=6, history resets=13. Stages exercise camera motion, resize, Reference, mode/history changes and NEE/visibility specializations. |
| Vulkan skipped stages | NRD/TAA/SR are still skipped by the harness due to the existing RGB10A2 UAV limitation. A zero sequence-failure count does not make these stages passing. |
| Vulkan Reference startup | Retested with `--realtime false --aa off --vk_rr false`; `reference-vk.log` contains no graphics error and `reference-vk001.png` shows accumulated scene output. The previously recorded device loss did not reproduce in this run; this does not establish a root-cause fix. |
| FP32 and TraceRay visibility | D3D12 Debug without denoising produces scene captures (`fp32-d3d12*`). Vulkan Release with RR Native/E produces scene captures (`fp32-rr-vk*`). Both use `--fp16 false --inline_shadows false`; Vulkan also uses TraceRay scatter. |
| Vulkan realtime without denoising | `fp32-release-vk.log` reports unsupported RGB10A2 UAV and incomplete targets; capture shows the failure background. The non-RR allocation branch requires NRD resources even with `--nrd false`; this condition also exists in baseline `50774cdc`. This configuration is a known failure, not a passing FP32 render test. |
| Vulkan Debug FP32 | The standalone launch did not reach a captured frame within 90 seconds; initialization was stopped by the test driver. Release subsequently compiled/rendered FP32 with RR. Debug runtime for this combination remains unverified. |

RR tests now use `DenoisingGuidesBaker.hlsl::PrepareDLSSRRGuides`, rather than
the legacy mode-9 entry. The new smooth-reflector case has world displacement
1 on X, producing specular motion -0.5 pixels while ordinary motion is -0.75.
The NaN-roughness case first exposed an incorrect specular branch after albedo
sanitization; the branch now checks the original layer data before use.

Performance comparison: RTX 5060, Release/D3D12, convergence-test, 640x360,
RR Quality/F, same default camera, 1 spp, same validation/profiling settings.
The CSV means exclude frames 1-240; no scene-validation mode switches occur in
these two runs. This small-scene test is not a 2M-light scalability benchmark.

| GPU milliseconds | Before (16,800 frames) | After (15,720 frames) |
| --- | ---: | ---: |
| Total | 1.9153 | 1.9554 |
| BUILD | 0.0817 | 0.0878 |
| LightsBaker | 0.1275 | 0.1098 |
| FILL | 0.9823 | 1.0339 |
| Guides | 0.0421 | 0.0406 |
| RR prepare | 0.0188 | 0.0213 |
| RR | 0.6083 | 0.6125 |
| Post-process | 0.0272 | 0.0248 |

Inputs: `baseline-d3d12.csv` and `final-d3d12.csv`. Total increased about 2.1%;
there is no measured speedup claim. Baseline/final first-frame PNGs have mean
absolute RGB differences 4.55/4.77/5.15 on an 8-bit scale, including the UI.
Later D3D12 and Vulkan captures show the room, transmission and reflections;
this is a visual smoke check, not an HDR energy or temporal-artifact metric.
The initial baseline Vulkan capture was nearly black and is unsuitable for an
image-equivalence comparison.

Remaining acceptance limits: no matched before/after scene-linear HDR capture,
no automated temporal ghosting/leak/firefly metric, no full 2M-light scene
render/performance run, and no dedicated animated emissive scene capture.
Skinning ABI and cluster rebuild paths have GPU coverage, but that does not
replace the animated-scene test. These items remain unverified, not passed.
No source/dependency changes were made outside the RTXPT sample.

For native scatter HitObject / reordering, the pinned SM 6.9 runtime, numerical
ray tests and three-mode timings, see [HitObject performance](HITOBJECT_PERFORMANCE.md).

For same-boundary upstream GPU timings and the measured NEE/FILL investigation,
see [GPU performance comparison](GPU_PERFORMANCE_COMPARISON.md).

`RRPrepareInputsTest.cpp` exercises the production resource owner, frame adapter,
C++ prepare wrapper and guide-baker RR shader on D3D12 or Vulkan. It reads all seven outputs.
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
DenoisingLayer packing; they do not reimplement the RR preparation algorithm.

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
and 15 readback cases. D3D12 also checks RR -> NRD -> RR allocation with NRD's
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
sample for nineteen 32-frame stages: Quality, smooth camera motion, explicit history
reset, Native, native-window resize to 768x432, reference accumulation, TAA,
ordinary SR, standalone NRD, return to Quality RR, lighting/preset/environment
changes, and NEE/MIS/visibility permutations before restoring defaults.
D3D12 starts at F and explicitly changes to E during the sequence. Vulkan should
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

### Further FILL upstream-parity audit (2026-09-14)

The production baseline is Samples commit `bb9f98c8` (inline shadows and
upstream alpha filtering). This audit changes only isolated shader copies under
`%TEMP%/rtxpt-perf-diagnosis/fill-audit-*`, with separate working directories and
fresh caches. It does not change the production renderer. Settings are D3D12
Release, convergence-test, RR Balanced/F, 1920x1080 output / 1114x626 render,
1 spp, 5 candidates, VSync/validation off. GPU runs are sequential; CSV means
exclude frames 1–240. Measurements are GPU sample-rendering times, not Present
or upstream per-stage measurements.

| Isolated experiment | Frames | Total GPU ms | FILL GPU ms | RR GPU ms |
|---|---:|---:|---:|---:|
| Production baseline | 1240 | 18.236 | 13.876 | 2.902 |
| Full BSDF evaluation only for the selected visible light | 1310 | 16.130 | 11.752 | 2.913 |
| Defer mixed-PDF lookup/correction until selected light is visible | 1202 | 17.381 | 13.025 | 2.901 |
| Both preceding changes | 1354 | 15.572 | 11.204 | 2.906 |
| Disable independent environment NEE (diagnostic, biased) | 1593 | 13.945 | 9.568 | 2.915 |
| Surface bounce limit 18 instead of 20 | 1124 | 18.124 | 13.768 | 2.897 |
| Unmodified baseline repeated after experiments | 1238 | 18.175 | 13.833 | 2.884 |

Confirmed source differences and interpretation:

1. **Full BSDF work inside the candidate loop.** Upstream
   `D:/RTXPT-fork/Rtxpt/Shaders/PathTracer/PathTracerNEE.hlsli::EvalSampleWeight`
   uses radiance times BSDF PDF; `ProcessLightSample` evaluates full BSDF only
   after visibility. The port's `Lighting/LightSampler.hlsli::GenerateDirectLightCandidate`
   calls `EvalBSDF` and saves `bsdfF` for every candidate. Its reservoir weight
   uses only `bsdfPdf`. The prototype stops storing candidate `bsdfF`, allowing
   DXC to eliminate the unused full evaluation, and evaluates it once after the
   chosen shadow ray succeeds. This preserves candidate PDFs and RNG order and
   saves approximately 2.1 ms here. Production implementation should expose an
   explicit PDF-only helper and verify numerical output equivalence.

2. **Other-proposal PDF queried too early.** Upstream `GenerateLightSample`
   uses local/global candidate counts, conditional proposal PDFs and
   `LATE_WRS_MIS`; cross-proposal density queries happen in `ProcessLightSample`
   after visibility. The port randomly selects a proposal component, immediately
   calculates the mixed density for each candidate, then runs the reservoir.
   The diagnostic retains that random component selection, stores its selection
   PDF, weights candidates with the conditional density, and corrects the chosen
   visible sample by `conditionalPdf / mixedPdf` before outer MIS. It saves
   approximately 0.85 ms alone. This is not a full upstream stratified-sampling
   port and changes reservoir selection/variance; boundary, estimator and image
   regressions are required before merging. Combined with item 1 it saves
   2.66 ms, not the sum of the individual savings.

3. **Environment lighting follows a separate NEE path.** Upstream's
   `Lighting/PolymorphicLight.hlsli::EnvironmentQuadLight` participates in the
   unified light sampler and its environment-direction reverse lookup/MIS.
   The port calls `SampleDirectLightNEE` plus `SampleEnvironmentNEE` from
   `PathTracer.hlsli::HandleNEE`: potentially two visibility rays per eligible
   vertex instead of one selected unified-light sample when FullSamples=1.
   Disabling the independent environment path removes about 4.3 ms, but leaves
   its BSDF-miss MIS in place and is deliberately biased. This measures a cost
   opportunity, not a valid optimization or an additive predicted saving.
   Alignment requires environment proxy generation, unified candidate sampling,
   direction-to-light PDF lookup and both MIS directions together.

4. **Local sampling coherence is not aligned.** Upstream
   `LightSampler::IsScreenSpaceCoherentHeuristic` uses ray-cone width / total
   travel to select screen-local versus global-only candidates, and passes the
   same classification into temporal feedback. The port's candidate routine
   enables local sampling whenever the local table exists and its feedback
   path passes `true`; `HandleNEE` nevertheless records LightSamplingIsSSC=false.
   This is a further sampler/feedback contract difference, not yet isolated
   quantitatively in this audit. Address it with the complete sampler port.

5. **Defaults and shared path mechanics.** Upstream `SampleUI.cpp` applies the
   Balanced performance preset at startup: 18 surface bounces, 2 diffuse
   bounces, 5 candidates, 1 full sample and 3 stable planes. The port has 20
   surface bounces; the measured difference is only ~0.11 ms here. Both main
   payloads are 80 bytes. Both FILL raygens resume plane 0 via FirstHitFromVBuffer
   and narrow the first ray's interval to 0.99–1.01 of its stored distance;
   there is no extra full traversal of all three planes in the port's FILL loop.

6. **SER and material specialization remain unmeasured.** The original Release
   executable was run directly with `--d3d12 --nonInteractive --scene
   convergence-test.scene.json --width 1920 --height 1080
   --overrideToRealtimeMode --realtimeAA 3 --stopAnimations`. Six window-title
   measurements were 7.910–7.971 ms (125.4–126.4 FPS), confirming the user's
   result and the same 1114x626 internal resolution. Upstream
   `PathTracerSample.hlsl::nextHit` uses inline scatter traversal, HitObject,
   reordering and hit invocation; `SampleCommon/PTPipelineBaker.cpp` builds
   material-specialized hit shaders. The port uses TraceRay for scatter and a
   general hit shader. **Do not treat `--noSER` as an off measurement in this
   build:** `build/CMakeCache.txt` selects Agility SDK 619, and
   `SampleUI.cpp:145–151` enables DXHitObjectExtension independently of the
   NVAPI capability flag masked by `disableSER`. A genuine off test must disable
   the DX path/reorder setting and confirm the resulting macros. No valid SER
   off experiment or separate material-specialization timing was obtained.

Recommended implementation order: PDF-only candidate evaluation, then a tested
upstream sampler/MIS/coherence port, then unified environment lighting. Obtain a
valid DX SER/material-specialization comparison before attributing the rest of
the gap to them. These experiments establish FILL costs; they do not establish
full image-quality equivalence or promise upstream frame rates after combining
all changes. Raw CSVs/logs and experimental shader copies remain in the temporary
diagnostic directory; aggregate evidence and source entry points are retained here.

### Production sampler / MIS / environment alignment (2026-09-14)

The preceding audit's first two optimizations are now implemented with explicit
PDF-only and value-only BSDF evaluation. Global/local candidates are stratified;
cross-proposal PDF queries and late balance MIS run only for a visible selected
sample. Full and approximate MIS are shader specializations. Reverse emissive and
environment MIS now applies to FILL as well as Reference. The previous FILL route
omitted reverse emission MIS, so image brightness is not expected to be identical
to that incorrect baseline.

Environment quads now share the same candidate reservoir, replacing the independent
environment NEE ray. The production baker also processes temporal/global feedback,
coherence, reprojection, blended reservoirs, local windows/top-up, R2 jitter and
history decay. Proxy jobs scatter in parallel through an indirect dispatch.
See `RTXPT_FORK_MAPPING.md` for the explicit remaining broader baker differences.

Build all lighting regressions from an x64 VS 2022 Native Tools prompt after the
RTXPT Release build:

```bat
DiligentSamples\Samples\RTXPT\tests\build_lighting_tests.bat
set RTXPT_TEST_ROOT=%CD%
set RTXPT_ENGINE_DIR=%CD%\build\x64\Release\DiligentSamples\Samples\RTXPT\Release
cd DiligentSamples\Samples\RTXPT
"%TEMP%\RTXPTLightingTests\LocalLightSamplingTest.exe" "%RTXPT_ENGINE_DIR%\GraphicsEngineD3D12_64r.dll" "%CD%\shaders"
"%TEMP%\RTXPTLightingTests\EnvironmentLightSamplingTest.exe" "%RTXPT_ENGINE_DIR%\GraphicsEngineD3D12_64r.dll"
"%TEMP%\RTXPTLightingTests\LightFeedbackTest.exe" "%RTXPT_ENGINE_DIR%\GraphicsEngineD3D12_64r.dll"
rem Repeat the three commands with GraphicsEngineVk_64r.dll.
```

All three executables passed on both D3D12 and Vulkan with graphics validation:

- `LocalLightSamplingTest`: deterministic sorted-table fixtures validate conditional
  local/global PDFs; 128 BSDF cases compare PDF-only/value-only evaluation to the
  combined evaluator. Candidate counts cover 0–63. Exhaustive enumeration of all
  32 candidate tuples and reservoir outcomes checks a known two-light integral,
  including zero local support and complementary forward/reverse Full MIS.
  Approximate MIS complement, zero-density boundaries and spotlight smoothstep
  are checked separately. The legacy maximum-feedback shader is a table fixture,
  not the production temporal producer.
- `EnvironmentLightSamplingTest`: production base/boost/lookup passes compared with
  an independent CPU subdivision oracle for uniform, concentrated-HDR and black
  maps. Checks all 5368 leaves, radiance/importance, exact coverage, area/PDF
  normalization and disabled-environment resource behavior.
- `LightFeedbackTest`: production preprocessing/reprojection/table/history passes
  cover SSC, WSC, depth disocclusion, offscreen motion and zero proxies on partial
  tiles. Global counts include WSC; local samples reject it. Production proxy jobs
  cover black maps, uniform/skewed power, feedback mixing, sphere/point/spot power,
  capacity limits and 70,000 jobs exercising the second indirect-dispatch dimension.
  The proxy histogram must exactly match the counters used by the shader PDFs.

Cache version **007** is required. New reproducibility options are `--nee_mis 0|1|2`
(Full / ApproxInRealtime / Approximate), `--nee_type 0|1|2`, `--nee true|false`,
`--nee_env true|false` and `--nee_emissive true|false`. Keep all other defaults for
the comparison. Full MIS can also be selected through the now-enabled UI control.

During validation, explicit null binding of the unused visibility SBT slot fixed
the inline-RayQuery path's validation error with the shared two-ray TLAS stride.
The feedback wrapper explicitly barriers retained reservoirs before FILL and
scratch reads before the next frame overwrites them.

Final cache-007 Release validation also completed:

- D3D12 Full MIS scene validation: zero failures, 224 RR executions, six feature
  creates and six history resets.
- Vulkan ApproxInRealtime scene validation: zero failures, 224 RR executions,
  five feature creates and six history resets. NRD/TAA/SR mode checks were skipped
  because of the existing RGB10A2 UAV support limitation; these modes are not
  covered by this Vulkan result.
- D3D12 Reference / Full MIS / AA disabled rendered successfully without logged
  errors. This is a functional smoke test, not a converged image-quality comparison.

Final performance used the RTX 5060, D3D12 Release, convergence-test, 1920x1080
output / 1114x626 input, RR Balanced/F, default realtime settings, VSync and
graphics validation disabled. The mean below covers 1832 GPU timestamp records
after frame 240 (`verified-1080.csv` in the temporary diagnostic directory).

| GPU stage | Final mean (ms) |
| --- | ---: |
| Total | 12.439642 |
| BUILD | 0.320709 |
| LightsBaker | 0.424334 |
| FILL | 8.198489 |
| Guides | 0.283214 |
| RRPrepare | 0.215677 |
| RR | 2.866800 |
| PostProcess | 0.096876 |

Against the preceding audit baseline (Total 18.236 ms, FILL 13.876 ms), GPU total
time fell about 32% and FILL about 41%. Observed window frame rate was roughly
76-80 FPS. RR remains close to its previous 2.902 ms, so the measured improvement
comes primarily from path tracing. The upstream 7.910-7.971 ms result remains
faster. General scatter tracing/material dispatch and the documented broader
LightsBaker differences remain candidates for investigation; the existing SER-off
experiment does not isolate HitObject/SER and cannot establish its contribution.

### LightsBaker importance and source history (2026-09-14)

Cache **011** adds frustum and intensity-delta importance boosts and bidirectional
source history maps. The new regression coverage includes:

- Known per-light weights across three frames, including the frustum boundary,
  distant half boost, intensity changes, reordered IDs and a newly appearing light.
- CPU source-range reorder, removal/reappearance and changed triangle count;
  GPU environment lookup remapping and return to identity on unchanged frames.
- Production 5368-node environment trees changing from uniform to a concentrated
  HDR source and then black, exercising actual split/merge maps in both directions;
  disabling the environment invalidates its old IDs.
- Feedback selected-ID remapping before global counts, invalid/removed IDs and
  historical local-table fallback. Mixed-light tables verify sorted IDs and
  per-entry multiplicities against their histograms.

GPU-assisted Vulkan validation exposed a shared-memory sorting race. The bitonic
step now has each lane read both inputs, synchronize, write its own output and
synchronize again. The GPU-assisted regression completes without validation errors.

Release build, `LightFeedbackTest` and `EnvironmentLightSamplingTest` passed on
D3D12 and Vulkan; Vulkan GPU-assisted `LightFeedbackTest` also exited zero.
The production RR sequence now has 14 stages, adding environment intensity,
disable and re-enable changes and assertions that camera/environment changes retain
light-feedback history. D3D12 Full MIS and Vulkan ApproxInRealtime both completed
with zero failures and 320 RR executions (six/five creates respectively, nine image
history resets). Vulkan still skips the three NRD/TAA/SR mode checks due to the
existing RGB10A2 UAV limitation.

The final Vulkan 1920x1080 Balanced/E startup smoke also rendered 1256 RR frames
with no logged errors (one feature create and one history reset).

During integration, allocating the previous-environment buffer in initialization
reproducibly caused Vulkan device loss, including with all history dispatches
disabled. The preceding commit passed the same scene; isolated SRB and constants
allocations also passed. The final implementation allocates the snapshot on first
use from the live buffer description, and the full history-enabled regression
passes without extra waits. GPU-assisted instrumentation changed the reproduction
and reported no corresponding buffer error. The underlying allocation-order
interaction is still unresolved; the experiment does not establish a driver or
backend root cause. No speculative AS/emissive implementation changes are retained.

Final D3D12 performance uses the same RTX 5060, convergence-test, 1920x1080 /
1114x626, RR Balanced/F, defaults and disabled VSync/validation as the preceding
measurement. Mean of 1931 timestamp records after frame 240 (`final-history-1080.csv`):

| GPU stage | Mean (ms) |
| --- | ---: |
| Total | 12.521099 |
| BUILD | 0.325100 |
| LightsBaker | 0.451288 |
| FILL | 8.248081 |
| Guides | 0.283243 |
| RRPrepare | 0.215227 |
| RR | 2.863749 |
| PostProcess | 0.096304 |

Observed window rates were 77.9-79.2 FPS. The preceding total/FILL means were
12.439642/8.198489 ms, so this run establishes no performance improvement; remaining
FILL work is still the dominant gap. This change corrects temporal light selection
behavior rather than closing the original upstream performance gap.

### NEE sample-count specialization (2026-09-15)

Cache **012** specializes the full and total candidate sample counts in all RT
variants. Changing either count rebuilds before dispatch; the outgoing MIS state
uses the same counts. The scene regression now has 19 stages, including full=0,
candidates=1/full=2, candidates=8/full=2, Reference 1/1, and realtime default 5/1
restoration. Each added stage checks the compiled counts and successful rendering.

Release build passed. D3D12 Full MIS and Vulkan ApproxInRealtime, both with API
validation enabled, completed all 19 stages with **failures=0**, 448 RR executions
and 13 history resets (7 D3D12 / 6 Vulkan creates). Vulkan retains the documented
NRD/TAA/SR skips. The first D3D12 run ended before all cold permutations compiled;
the final run allowed 95 seconds and completed. No errors were logged in the final
regressions or performance runs.

Sequential performance comparison: RTX 5060, D3D12 Release, convergence-test,
1920x1080 output / 1114x626 input, RR Balanced/F, default 5/1 NEE, VSync and
validation disabled. The same executable used an isolated copy of the preceding
shader tree for the before run and the production shader tree for the after run.
Distinct shader hashes confirm the changed code was compiled. Means exclude the
first 240 frames: 2778 before / 2954 after records.

| GPU stage | Before (ms) | After (ms) |
| --- | ---: | ---: |
| Total | 12.478870 | 11.762980 |
| BUILD | 0.319502 | 0.319070 |
| LightsBaker | 0.439976 | 0.444980 |
| FILL | 8.261499 | 7.555130 |
| Guides | 0.280712 | 0.277228 |
| RRPrepare | 0.213611 | 0.212247 |
| RR | 2.833419 | 2.825355 |
| PostProcess | 0.098721 | 0.098032 |

FILL decreased by 0.706 ms (8.6%); total GPU time decreased by 0.716 ms (5.7%).
Window rates were 77.1-79.7 FPS before and 82.5-84.5 FPS after. This closes only
part of the upstream performance gap. First use of a new sample-count permutation
can pause rendering for shader compilation; restoring it reuses the state cache.

Local artifacts are in `%TEMP%/rtxpt-perf-diagnosis/`: `nee-specialization-build.log`,
`nee-specialization-d3d12-regression.*`, `nee-specialization-vk-regression.*`,
and `nee-specialization-before/after.csv` plus their runner logs.

### Native FP16 BSDF validation (2026-09-15)

Cache **013** enables upstream's native `lpfloat`/`lpuint` types for the RT
permutations by default, using DXC `-enable-16bit-types`. Use `--fp16 false` for
the FP32 control. Startup logs report both requested and effective precision.
Material storage, getters, stable-plane roughness and albedo estimation follow
upstream's precision boundaries; lobe evaluation and PDFs remain FP32. Device
feature checks fall back to FP32 when native 16-bit support is unavailable.

Verification:

- Release `RTXPT` and `DiligentCoreAPITest` builds passed. One intermediate relink
  encountered LNK1104 while the task's test application held RTXPT.exe open;
  the final build succeeded after that application exited.
- `DXCompilerTest.Native16BitTypes` compiles native float16/uint16 arithmetic via
  the public compile flag for both DXIL and SPIR-V. It and `IncludeResolution`
  passed (2 tests), covering the opt-in path and existing default compilation.
- Final D3D12 Full MIS / RR F and Vulkan ApproxInRealtime / RR E scene regressions
  completed all 19 stages with **failures=0**, 448 RR executions and 13 resets
  each (7 D3D12 / 6 Vulkan creates). Vulkan retains the previously documented
  three NRD/TAA/SR skips. Both logs confirm native FP16 enabled. New narrowing
  conversions are explicit; no BSDF compilation or API validation errors appeared.
- clang-format 10 changed-line checks and `git diff --check` passed. These scene
  regressions check execution/resource/history behavior, not pixel equivalence
  between FP16 and FP32.

Sequential RTX 5060 / D3D12 measurements used convergence-test, 1920x1080 output,
1114x626 internal resolution, Balanced/F, default NEE 5/1, VSync off and validation
off. Each run warmed up for 30 seconds and sampled the title eight times; the
table averages the final 1000 GPU timestamp rows. All runs used the same final
Release executable. The pre-change control restored the four modified shader
files from Samples commit `90f69ead` into an isolated temporary shader tree.

| Stage (ms) | Pre-change shaders | New shaders, FP32 | Default FP16 |
|---|---:|---:|---:|
| BUILD | 0.3183 | 0.3180 | 0.3260 |
| LightsBaker | 0.4390 | 0.4349 | 0.4438 |
| FILL | 8.0521 | 8.1129 | 8.1044 |
| Guides | 0.2963 | 0.2929 | 0.2957 |
| RR prepare | 0.2290 | 0.2260 | 0.2277 |
| RR | 3.0487 | 3.0539 | 3.0425 |
| Post-process | 0.1032 | 0.1039 | 0.1052 |
| Total | 12.5204 | 12.5768 | 12.5799 |

The title stayed around 77-79 FPS in all three runs. There is **no discernible
performance gain** from native FP16 in this scene: versus the new FP32 control,
FILL changes by -0.0085 ms and total by +0.0030 ms. Versus pre-change shaders,
total changes by +0.0595 ms (under 0.5%). This session's pre-change control is also
slower than the earlier NEE benchmark, so the earlier 83-84 FPS is not a valid
same-session baseline for this change. The remaining upstream gap is unresolved.

Artifacts: `%TEMP%/rtxpt-perf-diagnosis/fp16-final-build.log`,
`fp16-compiler-final.log`, `fp16-d3d12-final.*`, `fp16-vk-final.*`, and
`fp16-original/before/after.csv` with corresponding runner logs. Initial
`fp16-*-regression.*` runs preceded the final explicit casts / roughness-view
alignment; the `*-final.*` logs validate the delivered shaders.
