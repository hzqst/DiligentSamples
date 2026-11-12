# Per-layer NRD hit distance regression

## Signal contract

`DenoisingLayer::HitDistance` belongs to that layer's virtual surface. FILL arms
the record only when a sampled BRDF continuation leaves the surface without
transitioning to another retained delta plane. The first accepted hit completes
the record before emission, SHARC lookup or bounce-limit termination. Rejected
nested-dielectric boundaries add segment length but do not complete it. A miss
completes with 65504 scene units, the finite limit of the RGBA16F NRD inputs.
The primary/replayed surface distance never enters the accumulator. Finite
geometry distances retain FP32 range until method-specific packing: REBLUR
normalizes before its FP16 write, whereas RELAX clamps the raw distance there.

Diffuse samples use an arithmetic mean; specular samples use the minimum positive
distance, matching NRD 4.17.4 `SpecHitDistAveraging_*`. Neither is weighted by PDF,
throughput, radiance or the number of skipped lobes. Black radiance can still have
a valid distance. Invalid/zero distances do not create a valid sample. BUILD
clears the record every frame. Serial FILL dispatches retain aggregates while
resetting cancelled pending continuations between samples.

Counts distinguish missing lobes from valid samples. Flags distinguish diffuse
hit/miss, specular hit/miss and rejected nonfinite/negative distances. When there
is no outgoing sample (including a bounce-limited NEE-only surface), the distance
remains missing; a shadow ray, another layer's distance or the primary depth is
never substituted. This is
the first-segment baseline, not a multi-bounce distance estimator or a redesign
of the existing `specAvg` radiance split. Rare lobes and material boundaries can
still lack a valid sample in NRD's finite reconstruction neighborhood.

Prepare uses each layer's record. RELAX receives scene-unit distances; REBLUR
normalizes only valid positive distances. Calling the NRD normalization helper
on missing zero input would produce epsilon, so missing samples bypass it.
Diffuse normalizes with roughness 1; specular uses the roughness supplied in the
NRD normal/roughness guide. `RTXPTNrdParameters.h` supplies the same A/B/C values
to the shader and `ReblurSettings::hitDistanceParameters`.

RELAX now defaults to 5x5 distance reconstruction and 15/40 diffuse/specular
pre-pass radii, matching the probabilistic continuation sampling. These are
explicitly adjustable through the existing UI. For reproducible comparisons,
the Sample also accepts `--relax_hit_distance_reconstruction off|3x3|5x5`,
`--relax_diffuse_prepass <radius>` and `--relax_specular_prepass <radius>`.
Zero distance does not imply zero radiance: direct-light NEE and continuation
samples have separate provenance.

The shared layer record grows from 96 to 128 bytes. Allocation uses the C++
record size; the existing BUILD/FILL/Prepare UAV dependencies remain in use.
At 1920x1080 with three layers, the additional storage is about 190 MiB.
The legacy pixel-level specular distance and its smoothing remain available to
RR/specular motion; they no longer supply NRD radiance input alpha.

## GPU regression

Build RTXPT, then in a VS 2022 x64 Native Tools prompt run:

```bat
tests\build_nrd_hit_distance_test.bat <absolute-CMake-build-root> Debug
%TEMP%\NrdHitDistanceTest\NrdHitDistanceTest.exe D3D12 <GraphicsEngineD3D12_64d.dll> "<RTXPT>/shaders;<NRD>/Shaders;<MathLib>"
%TEMP%\NrdHitDistanceTest\NrdHitDistanceTest.exe Vk <GraphicsEngineVk_64d.dll> "<RTXPT>/shaders;<NRD>/Shaders;<MathLib>"
```

The test runs production HLSL aggregation and compares specular aggregation
against the linked NRD shader helpers. It also invokes the actual NRD Prepare
entry point and reads back both alpha channels for layer 0 and non-dominant
layer 2. Cases cover multiple samples, rejected-boundary segments, black
radiance, finite environment miss, NaN, cancellation, missing samples with stale
numeric contents, geometry distances beyond FP16 range, C++/shader
structured-buffer stride, and both NRD packers.

`NrdRadianceScaleTest.hpp` runs the same production entry points on layers in
working units (`ptConsts.radianceScale`): Prepare of both packers clamps the
demodulated radiance at `min(255, preExposedGrayLuminance * denoiserRadianceClampK * 16)`
whatever the scale and starts the HDR with the stable radiance divided by the
scale; the RELAX final merge and the merge without a denoiser add their radiance
divided by the scale; each stays within FP16, and a scale of 1 changes nothing.

## Inspecting real traced inputs

Select `Denoising guide debug` in the Sample, or pass
`--nrd_guide_debug diffuse|specular|validity`. The quadrants show layers 0, 1, 2
(top-left, top-right, bottom-left); the fourth is unused. Distance views use a
logarithmic heat map, with black for missing data. Validity uses red for a valid
diffuse sample, green for specular and blue for environment miss; magenta marks
an invalid input. Capture the un-tonemapped debug view with
`--hdr_capture_processed true` and the usual HDR capture options.

For image comparison, use fixed scene/camera, resolution, SPP, AA, NEE and seed
sequence. Separate the distance-input change from RELAX pre-pass changes. Compare
against the undenoised estimator mean, not an unrelated Reference configuration.
Retain RMSE, frame-to-frame luminance changes and GPU timestamps; input-contract
correctness alone is not proof of a quality or performance improvement.

## Recorded verification (2026-11-03)

Windows / RTX 5060, Debug, NRD 4.17.4:

- RTXPT builds; the GPU test passes on D3D12 and Vulkan. Running the test against
  the parent NRD frontend reproduces the missing-zero normalization failure.
- Both APIs render RELAX and REBLUR captures with finite HDR values. Additional
  D3D12 captures cover 4 SPP + SHARC, quarter-rate tracing and a one-bounce,
  NEE-only configuration. A single-frame validity readback contains diffuse and
  specular samples in all three layers, including both non-dominant layers, and
  no invalid-distance markers. This is not an exhaustive motion/ghosting test.
- The existing D3D12 20-stage SubSampling validation reports `failures=0` through
  640 frames: continuous camera motion, disocclusion, sampling-rate/pattern
  changes, odd viewport sizes and the NRD-to-RR transition.
- The fixed convergence-test scene used 1 SPP, Power+ NEE, AA off and a 512-frame
  undenoised mean. Actual captured extent was 298x184 (the window's minimum size
  exceeded the requested 160x120). Each denoised sequence used the same sample
  indices; the following metrics average frames 33-64.

| Configuration | RMSE against mean | Adjacent-frame luminance MAE |
| --- | ---: | ---: |
| RELAX old input + old OFF/0/0 settings | 0.128689 | 0.003762 |
| RELAX new input + old OFF/0/0 settings | 0.130084 | 0.003734 |
| RELAX new input + new 5x5/15/40 settings | 0.117063 | 0.003196 |
| REBLUR old input | 0.106558 | 0.000548 |
| REBLUR new input | 0.125325 | 0.000407 |

The old-input control uses the parent tracing/Prepare shader behavior, the new
executable and the same 128-byte record layout; it is not a separately built
parent executable. This isolates the input change but cannot measure the memory
cost of the larger record. This is one low-resolution scene/run, not a general
quality claim. The 512-frame mean also retains sampling error.

REBLUR's lower temporal variation comes with a measurable detail/error tradeoff
here. Separate controls supplying only the new specular guide or only the new
diffuse guide yield RMSE 0.117994 and 0.115923 respectively (both keep the new
missing-zero guard/roughness normalization). Both channels contribute to the
change. NRD's `REBLUR_Common_SpatialFilter.hlsli` derives blur area and sample
weights from hit distance; zero/near-zero guides previously restricted those
footprints. The comparison shows smoother floor texture with the new guides.
This supports a filtering explanation, not proof that every remaining error is
caused by over-blur. Do not restore fake zero guides merely to tune this scene;
broader REBLUR tuning and direct/indirect signal separation remain separate work.

Median post-capture D3D12 FILL timestamps (frames 96+) were 0.733 -> 0.750 ms for
RELAX and 0.736 -> 0.758 ms for REBLUR. Total GPU intervals were 1.363 -> 1.435 ms
and 1.363 -> 1.389 ms respectively. These low-resolution Debug measurements are
indicative only; the Sample's separate NRD timestamp columns were unavailable,
so no isolated NRD cost or performance improvement is claimed.
