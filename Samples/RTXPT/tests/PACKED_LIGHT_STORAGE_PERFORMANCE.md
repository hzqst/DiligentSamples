# Upstream packed light storage

## Scope

The baseline is Samples `1a6574a4` / root `3fc5c82`, including the merged
candidate BSDF PDF and `DirectLightSample` changes. This migration changes
storage and its producers/consumers; it is not another PDF ablation.

- `Lighting/PolymorphicLight.h` shares the upstream 32-byte base and separate
  16-byte extension ABI with C++. `PolymorphicLightInfoFull` combines these
  locally. CPU authoring and decoded shader values are no longer GPU records.
- Analytic lights pack RGB8/log16 radiance, FP16 radius/solid angle, octahedral
  direction and optional shaping fields. Emissive triangles store a FP32
  centroid and three FP16 edge pairs. Environment nodes pack X/Y/dimension,
  retain FP32 power weight and use the same packed radiance.
- `RTXPTLightsBaker` exposes unified `[environment | analytic | triangle]`
  base/extension buffers. BUILD, FILL, BSDF-hit MIS and proxy weighting read
  that table; extensions are fetched only when the shaping flag is set.
- Packed producer buffers remain staging sources. Dirty data is assembled
  before BUILD with GPU copies; static frames do not recopy the table.
  Preparation belongs to the total GPU timestamp, outside the BUILD and
  LightsBaker subranges. Steady-state measurements exclude initialization.
- Environment history snapshots also store packed nodes. Source-key history
  matching remains separate from debug-only `UniqueID` hashes, so moving or
  reordered lights do not use a table offset as source identity.

This matches upstream's quantization rather than preserving the previous
FP32 storage bit-for-bit. In particular, the upstream ABI has no finite
analytic `range` cutoff; lights now use its unbounded range semantics.
The port's scene authoring and sampling algorithms otherwise remain in place
(including explicit directional lights). IES profiles remain unsupported.
No image-difference test was requested or performed.

## Validation

Release RTXPT and the lighting test executables were built with MSVC; all
commands exited 0.
`LocalLightSamplingTest`, `EnvironmentLightSamplingTest` and `LightFeedbackTest`
run with graphics validation on D3D12 and Vulkan. Coverage includes:

- CPU-to-GPU 32/16-byte strides, signed octahedral directions, exact FP16
  radii/shaping values, zero/HDR RGB packing and triangle half-pair bit order;
- all 5,368 environment nodes in each of three cases, independent quadtree
  construction, packed color error bounds, FP32 weights and lookup coverage;
- local counts/PDF queries, reservoir/MIS contracts, proxy multiplicities,
  frustum/intensity boost, moving/reordered source IDs and environment remaps.

Both backends passed all three lighting tests. Full RR scene regression also
passed on both (`failures=0`, 448 RR executions, 6 creates, 13 history resets),
exercising mode/resolution changes and scene resource recreation.
Vulkan's existing NRD/TAA/SR RGB10A2-UAV limitation is
reported as skipped coverage, not a pass for those modes.

### Vulkan initialization submission

The initial migration reproduced `VK_ERROR_DEVICE_LOST` on Vulkan before the
first packed-table preparation. Disabling NEE and using reference mode did not
avoid it. Independent lighting tests and GPU-assisted validation did not
report packed-buffer bounds errors. Temporary GPU drains isolated the issue
to the initialization bake/submission boundary.

Submitting **after** the initial emissive bake resolves the reproduction;
submitting only before the bake does not. The final change flushes the scene
initialization commands before subsequent resize/settings updates can recreate
pass resources. It adds no GPU idle wait and no per-frame flush. Temporary
diagnostic drains and shader ablations were removed. The precise backend or
driver retirement mechanism was not established; this is the verified local
submission-order fix, not a claim that the packed format itself caused an
out-of-bounds access.

## Performance protocol

RTX 5060, D3D12 Release, convergence-test, camera `(0,2,8)`, FOV `0.660595`,
1920x1080 output / 1114x626 input, RR Balanced preset E, 18/2 bounces, 1 SPP,
3 stable planes, native FP16 and inline visibility, 5 NEE candidates / 1 full
sample. Each capture warms for 35 seconds and samples eight window titles
two seconds apart. Report GPU timestamps from the final 1,000 frames; title
FPS is not the timing oracle.

The baseline uses a saved pre-change executable **and** shader tree because
the CPU/GPU ABI changed. AT runs before/after and is repeated after/before;
Power runs after/before. Logs
must agree on settings, contain distinct before/after closest-hit hashes, and
have finite, nonnegative, unique-frame timestamps with total >= FILL + RR.

Raw executables, shader snapshot, logs, CSVs, hashes and the capture/audit script
are under `%TEMP%/rtxpt-perf-diagnosis/nee5-*` on the test machine.

## Results (2026-09-23)

All six port captures passed the timestamp/settings/hash checks. Each contains
3,769–3,907 valid frames; statistics below use only the final 1,000. The two AT
captures per version have matching shader and executable hashes. Graphics
clocks recorded at capture end were 2827–2835 MHz for the port.

Mean GPU time in milliseconds:

| Capture | BUILD | LightsBaker | FILL | RR | Total |
|---|---:|---:|---:|---:|---:|
| AT before, first | 0.319 | 0.438 | 6.810 | 2.789 | 10.964 |
| AT after, first | 0.319 | 0.433 | 7.093 | 2.794 | 11.245 |
| AT after, reverse-order repeat | 0.320 | 0.436 | 7.096 | 2.781 | 11.241 |
| AT before, reverse-order repeat | 0.318 | 0.426 | 6.755 | 2.795 | 10.905 |
| Power before | 0.320 | 0.398 | 6.575 | 2.781 | 10.678 |
| Power after | 0.321 | 0.400 | 6.778 | 2.779 | 10.885 |

AT's paired means are **FILL 6.782 → 7.095 ms (+0.312 ms, +4.6%)** and
**Total 10.935 → 11.243 ms (+0.308 ms, +2.8%)**. Power also regresses:
FILL +0.203 ms (+3.1%). BUILD, LightsBaker and RR do not account for this
increase. This is an upstream layout alignment, **not a measured speedup**.

A fresh upstream AT capture used clean `RTXPT-fork` commit `67b35a2` and the
same camera, internal resolution, bounces, SPP, NEE counts, stable planes and
RR preset (verified from its CSV settings sidecar). It contains 5,846 valid
frames; its final 1,000 frames give:

| Implementation | BUILD | LightsBaker | FILL | RR | Total |
|---|---:|---:|---:|---:|---:|
| Port after, two-capture mean | 0.319 | 0.434 | 7.095 | 2.787 | 11.243 |
| RTXPT-fork | 0.390 | 0.361 | 2.869 | 2.829 | 6.952 |

The remaining **Total gap is 4.291 ms, with 4.226 ms in FILL**. The upstream
end-of-capture clock was 2820 MHz. Window-title rates were approximately
88 FPS for the port and 134 FPS upstream; these include presentation/CPU
overhead and must not be confused with reciprocal GPU-total time.

The measurements rule out treating the old unpacked storage as a sufficient
explanation of the FILL gap. They do not independently separate unpacking ALU
cost, changed quantized inputs/proxy weights, shader register pressure or
control flow. Those require another measured ablation/profile; source layout
similarity alone does not establish the cause.

The follow-up [material/HitObject/SER ablation](MATERIAL_HITOBJECT_PERFORMANCE.md)
measures these execution-path differences directly and changes the next priority
to scatter-ray HitObject plus reordering, rather than material specialization.

Capture integrity:

- Before EXE SHA-256: `a83c638f37f7adc3f0dd172a7b7d974f0761afa235d6a7814536d20bd7389607`.
- After EXE SHA-256: `7da6aa2539ce4e14a728a9da4efa928d9d1529774b7f48068b8bf0f425b5ca57`.
- Per-capture CSV hashes, shader hashes, means, medians, P95 and standard
  deviations are retained in `nee5-final-summary.json`; `nee5-audit.py`
  verifies the six port captures, upstream capture and final RR regressions.
