# RTXPT upstream GPU comparison — 2026-09-15

Follow-up: [NEE candidate/PDF/reservoir measurements](NEE_CANDIDATE_PERFORMANCE.md)
test the candidate data flow with matched variants and repeated baselines.

## Result

The measured remaining difference is concentrated in FILL's NEE-dependent work.
RR does not account for the multi-millisecond gap. This change adds measurement
and benchmark controls; it does not ship any of the experimental shader changes.

Both renderers used Release/D3D12 on the RTX 5060, `convergence-test.scene.json`,
1920x1080 output / 1114x626 internal resolution, the scene camera at (0,2,8),
forward (0,0,-1), vertical FOV 0.660595 radians, 18 maximum bounces, 2 diffuse
bounces, 1 spp, 3 stable planes, 5 NEE candidates / 1 full sample, NEE-AT,
ReSTIR DI/GI disabled and standalone NRD inactive. The final pair explicitly
matched RR Balanced / preset E (both runtime logs report preset 5). FP16 remained
enabled in the port. Animations were stopped in upstream; the scene is static.

Applications ran sequentially. Startup warmup was 30–45 seconds and each result
below averages the last 1000 completed GPU samples, sorted by frame number.
Clocks were not locked and other desktop processes were left running. Differences
of a few hundredths of a millisecond are not treated as established improvements.

## Final stage timings

Captures: `upstream-final-steady` and `port-final-e`; 6216 and 3121 collected rows.
Values are GPU milliseconds, excluding scene/AS preparation, UI and Present.

| Stage | Diligent port | Upstream | Port − upstream |
| --- | ---: | ---: | ---: |
| Total | 11.842 | 7.275 | +4.566 |
| BUILD | 0.323 | 0.401 | -0.078 |
| LightsBaker UpdateEnd | 0.437 | 0.372 | +0.065 |
| FILL | 7.603 | 3.030 | +4.573 |
| Guides | 0.280 | 0.138 | +0.142 |
| RRPrepare | 0.215 | 0.205 | +0.010 |
| RR | 2.853 | 2.944 | -0.091 |
| PostProcess | 0.100 | 0.134 | -0.035 |
| VBufferExport | no separate pass | 0.014 | -0.014 |

Total includes small gaps between stages, so it need not equal their sum.
Earlier same-session baseline pairs measured upstream FILL 2.944–2.964 ms and
port FILL 7.607–7.651 ms. The original port 20-bounce run and the matched
18-bounce run did not show a multi-millisecond change. The final E/E pair also
confirms that the previous E-upstream/F-port preset difference was not the cause.
These measurements replace comparisons against FPS from a different session.

## Controlled FILL experiments

The exploratory pairs used upstream's default RR E and the port's default F;
the final pair above aligns E/E. NEE and candidate-count experiments change the
estimator's workload/noise. They are diagnostic comparisons, not equal-quality
performance gains. Full sample count remained 1 in every row.

| Condition | Port FILL ms | Upstream FILL ms | Difference |
| --- | ---: | ---: | ---: |
| NEE-AT, 5 candidates | 7.607 | 2.944 | 4.663 |
| NEE disabled | 2.157 | 1.622 | 0.536 |
| Uniform, 5 candidates | 5.376 | 2.567 | 2.809 |
| Power, 5 candidates | 7.373 | 2.756 | 4.617 |
| Power, 1 candidate | 4.287 | 2.075 | 2.212 |

Turning off NEE reduces the cross-renderer FILL gap by about 4.13 ms. Power
sampling does not use the local adaptive table, yet its gap is almost as large
as NEE-AT's. Therefore the local table cannot explain the main gap.

In Power mode, increasing candidates from 1 to 5 increases the port's FILL time
by 3.087 ms versus 0.681 ms upstream: an extra 2.405 ms of candidate-count-related
cost. This localizes a substantial part of the difference to the work introduced
by candidate generation/PDF/reservoir processing and its compiled execution.
Changing candidates can also change which sample is shaded or visible, so these
deltas are not an additive, instruction-level breakdown.

Uniform and Power also expose a separate scheduling difference: upstream
LightsBaker UpdateEnd takes about 0.0002 ms, whereas the port still spends
0.394–0.406 ms. This is outside FILL and is a concrete optimization target for
those modes. In the default NEE-AT mode that stage differs by only about 0.07 ms.

## Additional experiments

- **Hoist BSDF preparation once per NEE vertex:** a temporary port shader prepared
  `FalcorBSDF`, the tangent frame and view direction once and reused them for all
  candidate PDF evaluations and final BSDF evaluation. FILL was 7.578 ms versus
  7.607 ms. Different compiled closest-hit hashes confirmed that the variant was
  loaded. This experiment did not establish a useful gain.
- **Omit NEE visibility queries:** the port overlay measured FILL 6.619 ms.
  The intended upstream variant did not run: only its inactive runtime copy was
  changed (see correction below). The recorded upstream 2.982 ms is not a
  no-visibility result, and the claimed paired 3.637 ms gap is withdrawn. The
  port change also allows more samples to reach final shading/feedback, so its
  delta is not the pure cost of shadow rays.
- **Use upstream's ordinary TraceRay fallback:** this experiment also changed
  only the inactive runtime copy. The recorded 2.976 ms cannot support a
  conclusion about TraceRay versus HitObject/reordering. That conclusion is
  withdrawn; a test of the active source is required.

All experimental port shaders live in temporary overlay directories. Upstream's
two runtime shader files were restored byte-for-byte to source and their write
times advanced to invalidate the temporary compiled variants. The final baseline
ran after restoration. No renderer shader source change is part of this delivery.
Restoring a runtime copy did not prove that the experiment had used that copy.

**Source-selection correction from the follow-up:**
`SampleCommon/ShaderCompilerUtils.cpp`, `ShaderCompilerConfig::Initialize`,
prefers `../Rtxpt/Shaders` relative to the executable whenever that development
directory exists. `bin/ShaderDynamic/Source/Rtxpt` is only a fallback. Baseline
stage captures and CLI-controlled NEE/type/count comparisons above remain valid.
The two upstream shader ablations above are invalid and must not be reused.

The remaining instruction/memory/register-pressure cause within the candidate
path is not established by these experiments. The linked follow-up measures
`GenerateDirectLightCandidate` / `EvalBSDFPdf` / reservoir data flow against
upstream's `GenerateLightSample`, including PDF call-site merging that retains
candidate generation and weighting. Material specialization or memory-layout
explanations still require evidence; they are not conclusions of this report.

## Reproduce

From upstream `D:/RTXPT-fork/bin`, with an existing output directory:

```powershell
.\Rtxpt.exe --d3d12 --nonInteractive --scene convergence-test.scene.json --width 1920 --height 1080 --overrideToRealtimeMode --realtimeAA 3 --stopAnimations --useReSTIRDI 0 --useReSTIRGI 0 --gpuProfileCSV C:/Temp/upstream.csv
```

From the port's Release executable directory:

```powershell
.\RTXPT.exe --mode d3d12 --realtime true --scene convergence-test.scene.json --width 1920 --height 1080 --vsync false --validation 0 --aa rr --rr_quality balanced --rr_preset E --bounces 18 --gpu_profile true --gpu_profile_csv C:/Temp/port.csv
```

Use upstream `--NEEType 1 --neeCandidateSamples 1` and port
`--nee_type 1 --nee_candidates 1` for the one-candidate Power pair. Use upstream
`--useNEE 0` and port `--nee false` for no NEE. Verify the actual settings at
frame 120 (`<upstream CSV>.settings.txt` and the port log). Scene settings can
override the port's initial bounce value; this scene has no max-bounces override.

## Validation and artifacts

- Both final Release application builds completed successfully.
- Eighteen CSV captures passed numerical checks: at least 1000 samples,
  unique frame IDs, finite/nonnegative executed-stage times, and Total at least
  FILL + RR. This is timing/resource execution validation, not image equivalence
  or proof that a shader variant was active. The two upstream shader ablations
  were subsequently rejected as described above.
- `upstream-power-one` was rejected because its settings file showed 5 candidates:
  the UI preset had overwritten the initial CLI value. The override now applies
  after presets; `upstream-power-one-valid` records 1 candidate and is used above.
- `upstream-final` was rejected because rendering paused on focus loss (1933 rows,
  frozen title, GPU at 2%). Profiling now keeps upstream rendering while unfocused;
  `upstream-final-steady` records 6216 rows and 98% GPU utilization.
- Vulkan profiling was not validated in this task. No new claim about Vulkan
  performance or FP16/FP32 image equivalence is made.

Local artifacts are under
`C:/Users/HZDEV/AppData/Local/Temp/rtxpt-perf-diagnosis/`: each capture has `.csv`,
`.timing.log`, `.out.log` and `.err.log` files, plus upstream settings sidecars.
`summarize-stages.py` produces `stage-comparison-summary.json` with means, medians,
P95, standard deviations and frame-ID checks. Rejected captures remain available
for audit but are excluded from that summary.
