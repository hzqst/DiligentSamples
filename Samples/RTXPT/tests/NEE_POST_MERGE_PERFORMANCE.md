# NEE after merged candidate BSDF PDF calls — 2026-09-23

Follow-up to [NEE candidate experiments](NEE_CANDIDATE_PERFORMANCE.md), measured
after candidate PDF call-site merging was committed as Samples `c395da43`.
This report changes no renderer code and does not evaluate image equivalence.

## Findings

- The default NEE-AT / 5-candidate FILL gap remains 4.20–4.31 ms in the initial
  and repeated baseline pairs. RR is essentially matched.
- With NEE disabled, the FILL gap is 0.509 ms. Most of the default gap still
  depends on NEE work.
- Increasing Power candidates from 1 to 5 adds 2.998 ms in the port and 0.681 ms
  upstream: 2.317 ms of extra candidate-count-dependent cost.
- Replacing candidate BSDF PDF with a constant reduces that extra growth to
  1.614 ms. About 0.702 ms is sensitive to this diagnostic, while a larger
  candidate-count-dependent remainder survives.
- In the port, constant-PDF changes 1- and 2-candidate FILL little, but lowers
  5-candidate FILL by 0.857 ms. A fixed cost per PDF invocation does not explain
  this pattern. Compiled loop/data-flow behavior warrants investigation, but
  register pressure, spills and occupancy have not been measured.

## Measurement controls

Root `a2e57d2`, Samples `c395da43`, Core `89d3e7ce9`, upstream `67b35a2`.
Existing Release/D3D12 executables; RTX 5060, driver 610.88. Both worktrees were
clean before the experiments. Executable SHA256 hashes and revisions are saved
in the environment artifact.

Both renderers used `convergence-test.scene.json`, its camera at (0,2,8),
vertical FOV 0.660595 radians, 1920x1080 output / 1114x626 internal resolution,
18 maximum / 2 diffuse bounces, 1 spp, 3 stable planes, 1 full NEE sample,
RR Balanced / preset E, ReSTIR DI/GI off, and no active standalone NRD. The port
kept native FP16 enabled. Upstream animations were stopped; the scene is static.
The port used its committed source shader directory explicitly as working
directory, or a complete temporary shader overlay for the diagnostic.

Applications ran sequentially, alternating which renderer ran first between
conditions. Each launch had 35 seconds of warmup followed by eight title samples
two seconds apart. All tables use GPU timestamps, not title FPS: the mean of the
last 1000 completed CSV rows sorted by frame number. GPU clocks were not locked;
unrelated desktop processes were left running. Median, P95 and standard deviation
are retained in the JSON results. No new C++ build was needed for these temporary
shader variants; runtime shader compilation and execution were exercised.

## Current baseline

Initial default NEE-AT / 5-candidate pair, GPU milliseconds:

| Stage | Port | Upstream | Port minus upstream |
| --- | ---: | ---: | ---: |
| Total | 11.779 | 7.479 | 4.300 |
| BUILD | 0.330 | 0.396 | -0.066 |
| LightsBaker UpdateEnd | 0.445 | 0.367 | 0.078 |
| FILL | 7.288 | 3.093 | 4.195 |
| Guides | 0.297 | 0.148 | 0.149 |
| RRPrepare | 0.220 | 0.208 | 0.012 |
| RR | 3.048 | 3.073 | -0.025 |
| PostProcess | 0.104 | 0.136 | -0.032 |
| VBufferExport | no separate pass | 0.014 | -0.014 |

FILL median / P95 / standard deviation were 7.118 / 8.357 / 0.620 ms in the
port and 2.889 / 3.834 / 0.354 ms upstream. Single-frame variability is material;
small deltas are not treated as established improvements.

| Restored baseline | Port initial / repeat FILL | Upstream initial / repeat FILL |
| --- | ---: | ---: |
| NEE-AT, 5 candidates | 7.288 / 7.395 | 3.093 / 3.087 |
| Power, 5 candidates | 7.116 / 7.039 | 2.888 / 2.882 |

Default repeated Total was 11.847 / 7.466 ms and RR was 3.017 / 3.064 ms.
The baseline drift is smaller than the 5-candidate port constant-PDF delta.
With NEE disabled, FILL was 2.229 / 1.720 ms, leaving a 0.509 ms gap.

## Candidate-count scan

All values are FILL milliseconds. Five-candidate values below use the initial
captures; repeats are reported separately above.

| Selection | Candidates | Port | Upstream | Difference |
| --- | ---: | ---: | ---: | ---: |
| Uniform | 1 | 3.607 | 2.169 | 1.438 |
| Uniform | 2 | 4.426 | 2.365 | 2.061 |
| Uniform | 5 | 5.692 | 2.710 | 2.983 |
| Power | 1 | 4.118 | 2.206 | 1.911 |
| Power | 2 | 5.161 | 2.447 | 2.714 |
| Power | 5 | 7.116 | 2.888 | 4.228 |
| NEE-AT | 1 | 4.257 | 2.306 | 1.951 |
| NEE-AT | 2 | 5.322 | 2.614 | 2.707 |
| NEE-AT | 5 | 7.288 | 3.093 | 4.195 |

| Selection | Port 1-to-5 increase | Upstream increase | Extra increase |
| --- | ---: | ---: | ---: |
| Uniform | 2.085 | 0.540 | 1.544 |
| Power | 2.998 | 0.681 | 2.317 |
| NEE-AT | 3.031 | 0.787 | 2.244 |

Power's marginal cost per additional candidate is not constant: 1-to-2 adds
1.044 / 0.241 ms, while 2-to-5 adds 0.651 / 0.147 ms per candidate in port /
upstream. These are measured finite differences between specialized variants,
not a fitted instruction cost. Candidate count and selection distribution also
change which samples reach visibility and shading.

Uniform bypasses the Power proxy selection and local adaptive table, yet still
has 1.544 ms of extra 1-to-5 growth. Power and NEE-AT growth are close. The local
adaptive table cannot account for the main candidate-count-dependent gap.

## Constant candidate BSDF PDF after call-site merging

The temporary port overlay changes the single merged assignment in
`GenerateDirectLightCandidate` from `EvalBSDFPdf(bsdfData, wo, sample.dir)` to
`1.0`. After reservoir selection and successful visibility, it recomputes
`picked.bsdfPdf = EvalBSDFPdf(bsdfData, wo, picked.dir)` before final MIS.

The temporary upstream development shader changes `EvalSampleWeight` from
`max3(lightSample.Li) * bsdf.evalPdf(...)` to `max3(lightSample.Li)`. Its existing
real PDF evaluation before final MIS remains intact. Only Power mode is used
for this diagnostic, excluding the adaptive local table from selection.

This removes BSDF importance from candidate weights and changes zero-PDF
rejection. It changes the estimator's sampling distribution, selected candidates
and subsequent work. The deltas are neither equivalent-quality optimizations
nor isolated execution times of `evalPdf`.

| Candidates | Port normal | Port constant | Reduction | Upstream normal | Upstream constant | Reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 4.118 | 4.161 | -0.043 | 2.206 | 2.213 | -0.007 |
| 2 | 5.161 | 5.159 | 0.002 | 2.447 | 2.351 | 0.096 |
| 5 | 7.116 | 6.258 | 0.857 | 2.888 | 2.696 | 0.191 |

Normal 1-to-5 growth is 2.998 / 0.681 ms; constant-PDF growth is 2.097 / 0.483 ms.
The extra growth therefore falls from 2.317 to 1.614 ms, a 0.702 ms reduction.
At five candidates alone, the total cross-renderer FILL gap falls from 4.228
to 3.562 ms, a 0.666 ms reduction. These are different comparisons and should
not be conflated. Relative to the restored Power baseline, the port's
five-candidate reduction is 0.781 ms instead of 0.857 ms.

The candidate PDF path remains relevant after merging, but it does not explain
the larger surviving remainder. No register, spill, occupancy or memory-stall
counters were collected, so no hardware-level cause is established here.

## Validation, exclusions and restoration

- Thirty accepted captures contain 3332–7111 rows each. All passed checks for
  unique frame IDs, finite/nonnegative stage times, Total >= FILL + RR, expected
  scene/camera/render parameters, and no error/fatal/device-removed stderr.
- Upstream uses the active development source
  `D:/RTXPT-fork/Rtxpt/Shaders/PathTracer/PathTracerNEE.hlsli`, not the inactive
  runtime fallback copy. All 15 accepted upstream captures generated 16 fresh
  DXIL binaries each, with file names, sizes and SHA256 hashes recorded.
- At each of 1, 2 and 5 candidates, constant-PDF changes eight upstream binary
  hashes relative to normal Power, including material-specialization binaries.
  The port's corresponding closest-hit shader hash lists also differ.
- Upstream NEE-AT 1- and 2-candidate first attempts exited early. They are excluded
  completely. Separate `-retry1` captures completed with 6388 and 6032 rows and
  verified parameters. Windows recorded a `LiveKernelEvent 141` near the first
  failed attempt; stderr was empty and the cause of the exits is unresolved.
  The restored default/Power baseline pairs completed after these failures.
- The upstream source was restored byte-for-byte in `finally` after each launch.
  Its restored SHA256 is
  `214D7F3903BA4A6C6B7CB48CADDBB19B3D0601685728F3A2B05D3456708241F3`.
  Port experimental changes only exist in the temporary overlay. All task-owned
  renderer processes exited. No experimental renderer changes are retained.
- Image comparison was intentionally omitted as requested. Vulkan was not
  measured. No cross-session speedup is inferred from the older report's numbers.

## Next measurements

1. Inspect compiled candidate-loop and BSDF PDF behavior for 2 versus 5 candidates,
   preferably with register/spill/occupancy evidence. The observed transition is
   a reason to investigate code generation, not proof of register pressure.
2. Separate the surviving candidate-generation cost from reservoir/data storage
   using controlled variants. Start with light-data loads, candidate fields and
   their live ranges; do not assume the previously unhelpful selection-PDF lookup
   or reservoir-flatten variants have become optimizations.
3. Keep the single-candidate gap separate from growth: Power already has a
   1.911 ms gap with one candidate, versus 0.509 ms with NEE off. A candidate-loop
   optimization alone will not necessarily remove this difference.

## Artifacts and reproduction

Artifacts are in `C:/Users/HZDEV/AppData/Local/Temp/rtxpt-perf-diagnosis/`:

- `nee3-20260923-{port|up}-{base|off|flat}-t{0|1|2}-c{1|2|5}` names each capture;
  optional `-repeat` and `-retry1` suffixes identify repeats and accepted retries.
- `.csv`, `.args.txt`, `.out.log`, `.err.log`, `.timing.log`, `.gpu.txt`, and
  `.result.json` retain timing data, commands, GPU state, statistics and hashes.
  Upstream additionally writes `.csv.settings.txt` with the runtime parameters.
- `nee3-20260923-summary.json` combines the 30 audited captures;
  `nee3-20260923-environment.json` records revisions, driver and executable hashes.
- `nee3-20260923-flat/shaders` is the port diagnostic overlay, with an assets
  junction. `nee3-20260923-up-original.hlsli` is the byte-exact upstream backup.
- `nee3-run.py` creates the overlay, runs the matrix with source restoration and
  per-capture validation; `nee3-measure.ps1` launches and closes each owned process.
  `nee3-summary.py` audits settings/logs/hashes; `nee3-tables.py` verifies variant
  binary differences and prints the comparison tables. Completed captures are
  skipped on resume and incomplete captures are never overwritten.

Normal port command, from the sample source directory (vary type/count as needed):

```powershell
D:/DiligentEngine-hzqst/build/x64/Release/DiligentSamples/Samples/RTXPT/Release/RTXPT.exe --mode d3d12 --realtime true --vsync false --validation 0 --scene convergence-test.scene.json --width 1920 --height 1080 --aa rr --rr_quality balanced --rr_preset E --bounces 18 --nee true --nee_type 1 --nee_candidates 5 --gpu_profile true --gpu_profile_csv C:/Temp/port-power5.csv
```

Normal upstream command, from `D:/RTXPT-fork/bin`:

```powershell
./Rtxpt.exe --d3d12 --nonInteractive --scene convergence-test.scene.json --width 1920 --height 1080 --overrideToRealtimeMode --realtimeAA 3 --stopAnimations --useReSTIRDI 0 --useReSTIRGI 0 --useNEE 1 --NEEType 1 --neeCandidateSamples 5 --gpuProfileCSV C:/Temp/up-power5.csv
```

Use an existing CSV output directory. Disable NEE using port `--nee false` or
upstream `--useNEE 0`. For the diagnostic, apply only the two port substitutions
or the one upstream substitution described above, verify active compilation,
then restore the original source and run the normal baseline again.
