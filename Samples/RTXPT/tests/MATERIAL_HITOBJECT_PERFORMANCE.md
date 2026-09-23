# Material specialization and HitObject/SER ablations

## Question and scope

Follow-up to the packed-light-storage migration: determine whether upstream
material specialization or the scatter-ray HitObject/SER path accounts for the
remaining FILL difference. Test upstream's existing fallback implementations,
keeping its BSDF, NEE, scene, camera and lighting code fixed. This is an ablation
on upstream, not an implementation or a speedup claim for the Diligent port.

Source baselines: upstream `67b35a2`; port Samples `6ade2c64`, root `e67bea1`.
No image-difference comparison is performed.

## What upstream actually enables

`Materials/MaterialsBaker.cpp::PTMaterial::ComputeShaderPermutation` currently
enables only limited specialization: ordinary non-emissive, non-analytic-proxy
materials define `RTXPT_MATERIAL_IS_EMISSIVE=0` and
`RTXPT_MATERIAL_IS_ANALYTIC_LIGHT_PROXY=0`. Thin-surface, transmission, texture
and per-material-unique variants are disabled/commented out. In convergence-test
the default FILL PSO binds two unique closest-hit permutations, not one unique
shader for each of the scene's 24 materials.

The captured default macros select **DX HitObject**, not NVAPI HitObject:
`USE_DX_HIT_OBJECT_EXTENSION=1`, `USE_DX_MAYBE_REORDER_THREADS=1`, and both NVAPI
macros are zero. `SampleUI.cpp` selects the DX path for the configured Agility
SDK. Its existing `--noSER` switch controls the NVAPI support check and is not
sufficient evidence that this DX path is disabled.

## Experimental controls

Temporary instrumentation in two upstream C++ files provides process-local
environment controls; normal operation with no controls retains default behavior:

- `RTXPT_PROFILE_UBERSHADER=1` selects the existing
  `PIPELINE_BAKER_REVERT_TO_UBERSHADER` alternative at both hit-group assignment
  and shader-library selection. This binds a single FILL closest-hit permutation
  with `RTXPT_MATERIAL_PERMUTATIONS_ENABLED=0`. It changes both compiled material
  specialization and hit-group grouping, as the native upstream fallback does.
- `RTXPT_PROFILE_TRACE_RAY=1` forces both HitObject-extension macros and both
  reorder macros to zero in `FillPTPipelineGlobalMacros`. The same macro vector
  also controls the C++ pipeline's miss/any-hit exports, so this is a valid
  ordinary-TraceRay pipeline, not merely a shader-side override.
- `RTXPT_PROFILE_NO_REORDER=1` keeps DX HitObject enabled but sets its reorder
  macro to zero, separating reordering from the broader HitObject path.

NEE visibility continues to use inline RayQuery in every case. The TraceRay
fallback changes the **scatter-ray** traversal/dispatch path, including its
miss/any-hit dispatch organization. Its timing delta is not pure SER cost.

`RTXPT_PROFILE_VARIANTS_FILE` records the exact raygen/closest-hit compile hashes,
entry-point names and combined macros selected when constructing each PSO.
Logging is protected by a mutex because pipeline creation is parallel. The
capture audit requires REF, BUILD and FILL records, two unique hit hashes per
default variant or one for ubershader, plus the requested HitObject/reorder and
material-specialization macro values.

## Protocol

RTX 5060, D3D12 Release, convergence-test, fixed camera `(0,2,8)` facing `-Z`,
FOV `0.660595`, 1920x1080 output / 1114x626 input, RR Balanced preset E, 18/2
bounces, 1 SPP, three stable planes, NEE-AT 5 candidates / 1 full sample,
ReSTIR DI/GI disabled. One renderer runs at a time.

Five cases run in forward order, then reverse order:

1. Default specialization + HitObject + reorder.
2. Ubershader + HitObject + reorder.
3. Default specialization + ordinary TraceRay.
4. Ubershader + ordinary TraceRay.
5. Default specialization + HitObject, reorder disabled.

Two supplementary captures then test ubershader + HitObject without reordering,
completing the material/dispatch matrix. Finally, the unchanged current port is
captured once with the same camera/render settings.

Each process warms for 35 seconds, followed by eight window-title observations
two seconds apart. Statistics use GPU timestamps from its final 1,000 frames,
requiring at least 2,000 valid frames overall. Camera/render settings come from
the CSV settings sidecar. Audits reject duplicate frame IDs, nonfinite/negative
timestamps, total time below FILL+RR, incomplete PSO evidence and error logs.
Raw artifacts live under `%TEMP%/rtxpt-perf-diagnosis/nee6*`.

Initial `nee6-*` captures are excluded because concurrent diagnostic writes
could lose PSO evidence. The corrected series is `nee6v2-*`. A corrected-series
baseline process and one TraceRay process exited during startup without usable
timings and were retried under new artifact names; their logs are retained
rather than counted as samples.

## Results

All **12 upstream captures** passed the settings, active-PSO and timestamp
audits, with 4,273–6,340 valid frames each. Each table cell averages two runs,
each using its final 1,000 frames. Times are milliseconds.

| Material path | HitObject + reorder | HitObject, no reorder | Ordinary TraceRay |
|---|---:|---:|---:|
| Default specialized FILL | **3.044** | **5.938** | **6.061** |
| Ubershader FILL | **2.901** | **4.467** | **4.701** |

Full stage context:

| Case | BUILD | LightsBaker | FILL | RR | Total |
|---|---:|---:|---:|---:|---:|
| Default | 0.392 | 0.367 | 3.044 | 3.009 | 7.352 |
| Ubershader | 0.385 | 0.365 | 2.901 | 2.951 | 7.125 |
| TraceRay | 0.390 | 0.367 | 6.061 | 2.981 | 10.322 |
| Ubershader + TraceRay | 0.384 | 0.365 | 4.701 | 2.907 | 8.872 |
| HitObject, no reorder | 0.391 | 0.361 | 5.938 | 2.945 | 10.155 |
| Ubershader + HitObject, no reorder | 0.381 | 0.363 | 4.467 | 2.867 | 8.588 |

Per-run FILL means, retained to expose run-to-run variability:

| Case | First capture | Repeat |
|---|---:|---:|
| Default | 3.098 | 2.990 |
| Ubershader | 2.959 | 2.844 |
| TraceRay | 6.095 | 6.027 |
| Ubershader + TraceRay | 4.891 | 4.510 |
| HitObject, no reorder | 6.049 | 5.827 |
| Ubershader + HitObject, no reorder | 4.492 | 4.442 |

Recorded end-of-capture graphics clocks were 2805–2835 MHz. RR means varied
from 2.861 to 3.071 ms, and the two ubershader/TraceRay runs differ by 0.381 ms.
Therefore small differences should not be interpreted as precise intrinsic
costs or strong statistical evidence. The multi-millisecond reorder effect
has the same direction in both runs and both material configurations.

## Interpretation

1. **Reordering is a major upstream FILL optimization.** Disabling only
   reordering while retaining HitObject costs 2.894 ms with default material
   specialization and 1.566 ms with ubershader. Disabling the complete
   HitObject/reorder path costs 3.017 and 1.800 ms respectively. Keeping
   HitObject without reordering does not recover most of the benefit.
2. **Material specialization is not the missing speedup in this scene.**
   Ubershader is slightly faster with reordering (0.143 ms in the two-run means)
   and substantially faster without it. In ordinary TraceRay it saves 1.360 ms.
   This tests upstream's limited native specialization and SBT grouping, not
   every possible material specialization strategy or every scene.
3. **The effects interact.** The material-specialization delta changes by
   approximately 1.217 ms between the reordered and ordinary-TraceRay paths.
   Do not add an independent "SER cost" and "material cost", or promise the
   default-specialized 3 ms benefit to the port's current generic shader.
4. **Prioritize scatter-ray HitObject plus reordering in the port**, initially
   preserving its generic material path. Its actual benefit must be measured
   after integration. The current visibility-ray inline RayQuery optimization
   does not implement this scatter-ray scheduling path.

The fresh, unchanged port capture contains 3,597 valid frames and measures
**FILL 7.425 ms, RR 2.885 ms, Total 11.701 ms**. Its executable and all three
closest-hit hashes match the packed-layout delivery. Against default upstream
the observed FILL gap is 4.381 ms; against upstream ubershader + ordinary
TraceRay it is still **2.724 ms**. This is an observational comparison of two
renderers, not an additive attribution: substantial BSDF/material/compiled
execution differences remain even with these two features disabled upstream.

## Evidence and restoration

- Every repeated condition has identical selected FILL raygen/closest-hit
  compile hashes. Ubershader changes the bound hit libraries while preserving
  the corresponding raygen hash; the three dispatch conditions have distinct
  raygen hashes. The selected DX/NVAPI and material macros were checked for
  every REF/BUILD/FILL binding, not inferred from UI labels or file edits.
- All 12 runs use the same instrumented executable, SHA-256
  `96317d288e5905c89fe287a9aa9d0dd53ccc413b18994501836ad5af9c77a4bf`.
- `nee6-final-summary.json` retains all settings, selected macros/hashes,
  per-capture CSV hashes, means, medians, P95 and standard deviations.
  `nee6-run.py`, `nee6-audit.py`, `nee6v2-instrumentation.patch` and the saved
  instrumented executable retain the reproducible local experiment.
- Both modified upstream C++ files, the original EXE and PDB were restored
  byte-for-byte against their pre-experiment SHA-256 values. Restored source
  timestamps were advanced so a future incremental build cannot retain the
  temporary instrumentation objects. Upstream Git status is clean.
- No renderer changes are delivered in either repository by this experiment.
  This report supersedes the withdrawn inactive-source TraceRay/SER ablation
  in `GPU_PERFORMANCE_COMPARISON.md`; it does not rehabilitate those old data.
