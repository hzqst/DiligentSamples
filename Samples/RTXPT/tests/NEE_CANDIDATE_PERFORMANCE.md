# NEE candidate data-flow experiments — 2026-09-15

Follow-up after the PDF merge was committed: [current baseline, candidate-count
scan and constant-PDF diagnostic](NEE_POST_MERGE_PERFORMANCE.md) (2026-09-23).

This follow-up uses the GPU stage profiler described in
[GPU_PERFORMANCE_COMPARISON.md](GPU_PERFORMANCE_COMPARISON.md). It measures shader
variants without shipping experimental renderer changes.

The clearest optimization candidate is merging the port's three candidate BSDF
PDF call sites: repeated Power runs improve by about 0.6 ms and the default
NEE-AT run improves by 0.8 ms in FILL. The exact GPU execution mechanism remains
unresolved. Corrected upstream variants use the active development source; early
variants that changed only an inactive runtime copy are explicitly rejected below.

## Method and limits

Both existing Release/D3D12 executables used the RTX 5060, the convergence-test
scene camera, 1920x1080 output / 1114x626 internal resolution, 18 maximum / 2
diffuse bounces, 1 spp, 3 stable planes, RR Balanced/E, no ReSTIR DI/GI, and no
active standalone NRD. The port's native FP16 path remained enabled. Power
sampling and 5 candidates / 1 full sample were used unless stated otherwise.
Power excludes the local adaptive table from candidate selection.

Each application ran alone within this experiment, with 35 seconds of startup
warmup (45 seconds for corrected active-source upstream variants) and eight
title samples two seconds apart. GPU results are the means of
the last 1000 completed CSV rows, sorted by frame number. GPU clocks were not
locked; unrelated desktop processes were left running. CSV statistics include
median, P95 and standard deviation. Title FPS is not the GPU FILL measurement.

GPU timestamp queries surround the whole FILL dispatch; they cannot surround an
HLSL function within that dispatch. Consequently these are controlled variant
deltas, not additive timings of individual functions. Changes to candidate
weights also change the winning sample, visibility/shading work, and potentially
compiler decisions. They are diagnostic ablations, not equal-quality speedups.
No GPU register, occupancy, cache-miss or instruction-stall counters were captured.

## Experiments

### Candidate BSDF PDF

The `pdf-flat` diagnostic substitutes 1 for the candidate BSDF PDF in the port's
three light-generation branches. The selected sample's real BSDF PDF is computed
before final MIS/shading. Upstream substitutes `max3(lightSample.Li)` for
`max3(lightSample.Li) * bsdf.evalPdf(...)` in `EvalSampleWeight`; final shading/MIS
continues to use the original BSDF. This removes BSDF importance from WRS and
also changes zero-PDF rejection. It deliberately changes candidate selection.

| Power condition | Port FILL ms | Upstream FILL ms | Difference ms |
| --- | ---: | ---: | ---: |
| Original, 1 candidate | 4.068 | 2.097 | 1.971 |
| Original, 5 candidates | 7.265 | 2.744 | 4.521 |
| Constant candidate BSDF PDF, 1 candidate | 3.927 | 2.129 | 1.798 |
| Constant candidate BSDF PDF, 5 candidates | 5.878 | 2.580 | 3.298 |

Increasing candidates from 1 to 5 costs 3.197 ms in the port and 0.647 ms upstream.
With constant candidate BSDF PDF, those increments are 1.951 and 0.452 ms. Thus
about 1.050 ms of the extra 2.550 ms candidate-count-dependent difference goes
away in this diagnostic; about 1.500 ms remains. This is evidence of sensitivity
to the candidate PDF/data-flow path, not proof that `evalPdf` alone costs 1.050 ms.

### Preserve candidate generation and change code structure

`pdf-merge` moves the port's three `EvalBSDFPdf` calls out of the environment,
analytic and emissive light branches to one `if (sample.valid)` call after those
branches. It retains the PDF formula, generated directions, RNG consumption,
weight calculation and reservoir selection code. It aims to preserve the
estimator; pixel equivalence across compiler changes was not tested.

`loop` adds `[loop]` to the candidate loop. `reservoir-flat` adds `[flatten]` only
to the conditional replacement of the current reservoir candidate. These retain
each renderer's existing comparisons and arithmetic; neither substitutes the
other renderer's WRS threshold formula.

| Power / 5-candidate structure | Port FILL ms | Upstream FILL ms |
| --- | ---: | ---: |
| Original | 7.265 | 2.744 |
| Merge candidate PDF call sites | 6.677 | not applicable |
| Explicit candidate `[loop]` | 7.008 | 2.796 |
| Merge PDF + `[loop]` | 6.595 | not applicable |
| Flatten reservoir replacement | 7.249 | 2.762 |
| Store RNG state in reservoir, replay winner | 9.034 | not tested |

Merging PDF call sites improves the initial Power run by 0.588 ms. Loop control
alone improves it by 0.257 ms, but adding loop control to merged PDF improves only
another 0.081 ms. These effects overlap and must not be added independently.
Flattening reservoir replacement does not establish a gain in either renderer.

The independent Power `pdf-merge` repeat measured 6.677 ms again. In the default
NEE-AT mode, the port's original FILL was 7.569 ms and PDF-merge was 6.770 ms:
0.800 ms less FILL time. Total GPU time was 11.791 versus 10.993 ms, with RR
essentially unchanged at 2.843 versus 2.841 ms. The three per-light-type PDF call
sites are therefore a concrete optimization location supported by repeated
measurements. These measurements do not distinguish code-size reduction,
reconvergence, instruction scheduling or register allocation as the hardware
mechanism; those require compiled-code/counter evidence.

The replay diagnostic stores only the winning candidate's pre-generation RNG
state, local/global flag, weight and accumulated weight, then regenerates the
selected candidate using a separate RNG instance after the loop. It retains the
original candidate RNG sequence and weight comparison for finite valid inputs,
but performs one extra light/PDF generation. It is substantially slower. This
rejects this particular recomputation-for-storage strategy; it does not prove
that register pressure is absent.

### Candidate selection PDF lookup

`selection-flat` keeps the Power proxy-index lookup and the selected light index,
but uses selection PDF 1 inside the candidate loop instead of reading
`ProxyCounters[lightIndex]`. After WRS, it queries the selected light's real PDF
once and restores the selected sample's radiance/PDF fields before final shading.
This changes WRS weights, so it is a diagnostic rather than an equivalent
optimization. BSDF PDF computation remains enabled.

The port measured 7.220 ms, essentially the same as its nearby restored Power
baseline of 7.217 ms (initial baseline 7.265 ms). The corrected upstream result
was 2.822 ms versus the final original-source baseline of 2.759 ms. This diagnostic provides no evidence
that eliminating per-candidate proxy-count reads is a useful port optimization;
it is not an isolated measurement of memory latency.

## Rejected upstream variants and source-selection correction

The initial upstream shader variants changed
`bin/ShaderDynamic/Source/Rtxpt/PathTracer/PathTracerNEE.hlsli`. Compilation-output
verification found no newly generated DXIL files. Inspection of
`SampleCommon/ShaderCompilerUtils.cpp:64-94` showed that the executable prefers
`../Rtxpt/Shaders` when this development directory exists. The runtime copy is
only a fallback. All upstream shader variants without `-active` in their capture
name are excluded, regardless of their measured times. Their settings and CSV
contents were valid, but the intended shader changes were inactive.

Corrected runs temporarily replace the actual development source, save its hash,
require newly compiled DXIL files, and save their names, sizes, timestamps and
hashes. A `finally` block restores the original source byte-for-byte and advances
its timestamp. A final baseline rebuild/run follows. Baselines and CLI-only
candidate-count/type controls were not affected by this source-selection issue.
The preceding comparison report also withdraws its two upstream shader-ablation
conclusions, which used the same inactive runtime-copy approach.

## Validation and delivery

- 31 new CSV captures were collected. Seven inactive-source upstream shader
  variants are rejected; 24 accepted captures passed checks for at least 1000
  rows, unique frame IDs, finite/nonnegative stage times, Total >= FILL + RR,
  expected scene/camera/render settings, and no error/fatal/device-removed stderr.
- All six corrected upstream runs, including restoration to baseline, generated
  16 fresh DXIL files each. Their `.compiled.json` manifests and source hashes
  record which files were rebuilt. Port logs record distinct closest-hit shader
  hashes for the overlays; the PDF-merge repeat uses the same hash as its first run.
- Original-source Power FILL was 7.265 ms initially and 7.217 ms finally in the
  port, versus 2.744 ms initially and 2.759 ms in the final rebuilt upstream run.
  This drift is small relative to the repeated PDF-merge improvement.
- The active upstream source, runtime fallback copy and saved original all have
  SHA256 `214D7F3903BA4A6C6B7CB48CADDBB19B3D0601685728F3A2B05D3456708241F3`
  after restoration. All task-owned renderer runs exited.
- Only reports change in the repository. Existing Release executables were used;
  no C++ rebuild was required. Runtime shader compilation and D3D12 execution
  were exercised. Vulkan and pixel-equivalence validation were not performed.

The next implementation candidate is PDF call-site merging, followed by image
and backend regression checks. Remaining multi-millisecond differences are not
fully attributed. Candidate generation/PDF/control-flow execution still warrants
compiled-code or hardware-counter investigation; this report does not assign
the unexplained remainder to registers, memory layout or BSDF math alone.

## Artifact names and reproduction

Artifacts live under
`C:/Users/HZDEV/AppData/Local/Temp/rtxpt-perf-diagnosis/`. Capture names begin with
`nee2-port-` or `nee2-up-`; variant suffixes match the names above. `-one` means
one candidate, `-at` means NEE-AT, and `-repeat` / `-final` identify repeats.
Accepted upstream shader variants have an `-active` suffix and a nonempty
`.compiled.json` manifest.

- `nee2-variants.py` creates the PDF-merge, constant-PDF, loop and flattened-WRS
  overlays and upstream runtime variants from the original sources.
- `nee2-replay.py` and `nee2-combined.py` create the replay and combined overlays.
- `nee2-selection.py` creates the deferred selection-PDF diagnostic.
- `nee2-run.ps1` invokes the port measurement script. Its old upstream mode
  changes an inactive runtime copy in this development checkout: do not use it
  for upstream shader experiments.
- `nee2-run-active.ps1` invokes the upstream measurement script, substitutes the
  actual development source inside `try/finally`, requires freshly compiled
  DXIL, and restores the source with an advanced timestamp.
- `nee2-summary.py` writes `nee2-summary.json` with GPU statistics and frame checks.
- `nee2-audit.py` writes `nee2-evidence.json` with verified settings, port shader
  hashes and CSV hashes.

For example, run sequentially from the artifact directory:

```powershell
.\nee2-run.ps1 -Side port -Variant pdf-merge -Suffix '-new'
.\nee2-run-active.ps1 -Variant pdf-flat -Candidates 1 -CaptureSuffix '-new'
.\nee2-run.ps1 -Side port -Variant pdf-merge -NEEType 2 -Suffix '-new-at'
```

The runner refuses to overwrite an existing capture. Port variants use temporary
shader overlays plus an assets junction. Accepted upstream variants temporarily
replace the actual development `PathTracerNEE.hlsli`. The baseline source hash
is checked before substitution and after restoration. No experimental shader
change is retained in either renderer's tracked source.
