# NRD frame time and history confidence

The renderer-facing frame time remains in seconds. At the NRD adapter boundary,
`RTXPTNrdFrameTimeMilliseconds` converts finite positive values to milliseconds.
Nonpositive, nonfinite or overflowing values select NRD's internal timing (zero).
Upscaler inputs retain their seconds contract.

NRD 4.17.4 samples both confidence inputs at the previous-frame reprojection UV.
The current layer's `HasValidHistory()` is a current-frame path correspondence
decision, so it cannot be written directly at the current pixel coordinate.

`RunDenoiserPrepare` now executes three ordered dispatches per layer:

1. Clear a render-size R32_UINT coverage scratch texture to zero.
2. Prepare the NRD inputs and scatter current path validity into the previous
   coordinates obtained from the same quantized RGBA16F motion NRD consumes.
   Mark every positive-weight in-bounds tap of the bilinear history footprint
   atomically. Texel-center coordinates have support `-1 < p < size`; the
   reprojection center need not be inside the image. This also covers the
   edge texels read by the linear-clamp confidence sampler.
3. Resolve to the layer's R8_UNORM confidence: one only for covered texels with
   valid contributors and no invalid contributors. Rejection wins collisions;
   no mapping, absent surfaces, nonfinite coordinates and footprints without
   positive in-bounds weight contribute no confidence. Explicit UAV dependencies
   order all three passes and scratch reuse across layers/frames. Resizing
   recreates the scratch allocation.

This is a conservative reprojection of the current assessment, not a delayed
mask. It retains the tracer's reset/untracked/unsupported-path decisions and
does not disable NRD's own geometric checks. Shared footprints can reject extra
neighboring history, and holes reject history rather than borrowing confidence.
It does not provide a separate previous-surface identity test or solve all of
HLND2T/HLND2T-DiligentGraphics#266's indirect-light convergence artifacts.

The scratch allocation costs four bytes per render pixel and is reused by all
layers. Prepare timing includes clear and resolve; NRD pool statistics do not
include application-side scratch storage.

## Sun shadow inputs (#325)

In frames whose constants set `sun.ShadowDenoising`, the FILL surface of layer 0 (no bounce from the layer) writes
`SunDirectLighting` (RGBA16F, working units, the path throughput times the BSDF at the disk centre times the sun's
irradiance, without shadow) and `SunHitDistance` (R16F: the distance to the closest occluder of one shadow ray to a
uniform point of the disk, 0 where the surface gets no sunlight, 65504 where nothing occludes it). BUILD clears both
for every pixel. The prepare of layer 0 writes `IN_PENUMBRA` (`SunPenumbra`, R16F) with
`SIGMA_FrontEnd_PackPenumbra(distance, sun.TanAngularRadius)` for every pixel; SIGMA reads the view Z, motion and
normal/roughness guides of layer 0 and writes `OUT_SHADOW_TRANSLUCENCY` (`SunShadow`, R8_UNORM), and the final merge
of layer 0 adds `SunDirectLighting * SIGMA_BackEnd_UnpackShadow(SunShadow)` to its surface pixels. Prepare and merge
bind the four textures in every NRD frame; `params.z` of their mini constants enables them.
`NrdSunShadowTest.hpp` checks the penumbra of an occluded and an unoccluded ray, the merge of layer 0 and the absence
of sunlight on sky pixels through the production entry points.

## Consumer coordinates and the border regression

The pinned NRD 4.17.4 sources use different confidence coordinates:

- `RELAX_TemporalAccumulation.cs.hlsl` samples diffuse and specular confidence at
  `prevUVSMB`. The resulting specular frame limits apply to both surface and
  virtual accumulation. Virtual history uses `prevUVVMB`, but external confidence
  is still sampled at the surface UV.
- `REBLUR_TemporalAccumulation.cs.hlsl` samples diffuse and surface specular
  confidence at `smbPixelUv`. Virtual specular separately samples at `vmbPixelUv`.
  Both history paths validate individual bilinear taps with `IsInScreenBilinear`.

The border fix covers the application's surface-motion scatter footprint, used
by both Prepare variants. It does not reconstruct NRD's internal virtual UV or
prove per-current-path rejection at REBLUR's independent virtual coordinate.
The shared previous-coordinate mask remains conservative where mappings overlap;
the synthetic consumer test verifies its linear-clamp lookup, not a complete
execution of NRD's specular virtual reprojection.

Build and run `build_nrd_hit_distance_test.bat` as described in
[NRD_HIT_DISTANCE.md](NRD_HIT_DISTANCE.md). In addition to the distance cases, it
tests seconds-to-milliseconds conversion and automatic timing, previous-coordinate
translation in the production RELAX/REBLUR Prepare entry point, bilinear coverage,
opposing writers in both orders, holes, edge clamping, NaN and off-screen input.
The border cases include four edges and four corners, half-pixel centers,
three-quarter-pixel centers, the last sampler subtexel inside bilinear support,
and exact/outside support endpoints. Each case checks the resolved texture and
an actual GPU linear-clamp lookup at every contributor's previous coordinate.
The integrating backend's world-package GPU regression computes an independent
CPU footprint from the exported layer records and exercises instance replacement,
rigid/skinned/camera motion and supported/unsupported reflection paths.

Recorded verification (2026-11-03, Windows, RTX 5060, Debug, NRD 4.17.4):

- Input GPU regression passes on D3D12 and Vulkan with device validation enabled.
- The integrating backend's complete world-package PT GPU suite passes on both
  APIs, including Native N01-N08, stale-color controls, reflection paths and resize.
  Its CPU oracle allows only a 1/256-pixel fringe for CPU/GPU projection rounding;
  the synthetic GPU footprint expectations remain exact.
- RTXPT and the full x64 Debug host build/install succeed; all 17 host render CPU
  tests and the module-loader test pass. Formatting, dependency and pointer checks pass.

The original cs_assault disocclusion sequence has not been remeasured here. These
results verify the two input contracts and their regressions, not the disappearance
of the remaining low-frequency blotches in issue #266.

Border follow-up verification (2026-11-03, same environment):

- With the previous shader, the new GPU consumer regression reproduces
  `PreviousPixel=(-0.75, 1), expected consumer=0, actual=1` on D3D12.
- With the fix, all 82 synthetic confidence dispatch sequences and the existing
  frame-time/Prepare/hit-distance regressions pass on D3D12 and Vulkan with device
  validation enabled. This includes positive in-bounds support, exact zero-weight
  endpoints, both contributor orders and an unaffected history control.
- RTXPT Debug and the integrating backend GPU test target build successfully.
  Both complete world-package PT GPU suites pass, using the updated CPU oracle.
- The fork's clang-format 10 validation passes for all 261 checked files.

The border follow-up does not establish a Native/SR image-quality baseline or
validate REBLUR's independent virtual-coordinate mapping. The original cs_assault
sequence remains pending.
