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
   Mark every positive-weight tap of NRD's linear-clamp footprint atomically.
3. Resolve to the layer's R8_UNORM confidence: one only for covered texels with
   valid contributors and no invalid contributors. Rejection wins collisions;
   no mapping, absent surfaces and nonfinite/out-of-screen coordinates contribute
   no confidence. Explicit UAV dependencies order all three passes and scratch
   reuse across layers/frames. Resizing recreates the scratch allocation.

This is a conservative reprojection of the current assessment, not a delayed
mask. It retains the tracer's reset/untracked/unsupported-path decisions and
does not disable NRD's own geometric checks. Shared footprints can reject extra
neighboring history, and holes reject history rather than borrowing confidence.
It does not provide a separate previous-surface identity test or solve all of
HLND2T/HLND2T-DiligentGraphics#266's indirect-light convergence artifacts.

The scratch allocation costs four bytes per render pixel and is reused by all
layers. Prepare timing includes clear and resolve; NRD pool statistics do not
include application-side scratch storage.

Build and run `build_nrd_hit_distance_test.bat` as described in
[NRD_HIT_DISTANCE.md](NRD_HIT_DISTANCE.md). In addition to the distance cases, it
tests seconds-to-milliseconds conversion and automatic timing, previous-coordinate
translation in the production RELAX/REBLUR Prepare entry point, bilinear coverage,
opposing writers in both orders, holes, edge clamping, NaN and off-screen input.
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
