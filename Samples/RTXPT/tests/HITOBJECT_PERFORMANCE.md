# Native scatter HitObject and reordering

This change keeps the generic material closest-hit shader and ports upstream's
scatter traversal: inline RayQuery with the existing alpha predicate, inline miss,
HitObject construction, optional termination-hint reordering, then payload
pack / Invoke / unpack. BUILD never reorders. Reference and FILL only reorder
when `PathFlags::enableThreadReorder` is set, matching upstream.

The Diligent SBT differs from upstream: each geometry reserves two ray slots.
The HitObject shader index is instance contribution + geometry index * 2 + primary
ray index. Primary miss and any-hit shader exports are omitted in HitObject mode;
the optional TraceRay visibility path retains its own miss and any-hit exports.
Raygen receives geometry/material/texture bindings needed by inline alpha tests.

## Runtime and capability selection

RTXPT on Windows x64 pins Agility SDK 1.619.0 and DXC v1.9.2602.24 using downloads
with SHA-256 verification. The runtime is local to this application; other samples
retain their existing runtime selection. Core's D3D12 device discovery now probes
SM 6.9 and 6.8 before the older versions. On D3D12 / SM 6.9, all three modes use
SM 6.9 to avoid conflating a shader-model change with reordering. Other devices
and Vulkan use the existing SM 6.5 TraceRay path.

Default: `--hit_object true --thread_reorder true`.

| Mode | Arguments |
| --- | --- |
| TraceRay | `--hit_object false` |
| HitObject | `--hit_object true --thread_reorder false` |
| HitObject + reorder | `--hit_object true --thread_reorder true` |

The startup log reports the selected path. `MaybeReorderThread` is a hardware hint;
SM 6.9 support does not imply the same performance benefit on every GPU.

**DirectSR compatibility:** the existing DirectSR preview DLL cannot create its
device factory with retail SDK 619 on this machine. The old SDK 716 run initialized
DirectSR successfully. NGX DLSS SR/RR remains available; this upgrade does not
preserve the preview DirectSR provider. Its DLL is still packaged separately from
the pinned retail runtime, without replacing the retail D3D12Core DLL.

## Validation

`VisibilityRayTest` has an optional `hitobject` mode that executes the production
scatter query and HitObject construction helpers. Its 48 rays cover alpha cutoff,
BLEND, excluded-from-NEE materials (still hit by scatter rays), miss/range limits,
multiple custom instance IDs, four geometries, indexed/non-indexed vertices and
skinned texture coordinates. Primary and shadow hit shaders return distinct
sentinels, so an incorrect two-slot SBT index fails the readback assertions.
Both uint16 and uint32 index modes are tested with graphics validation enabled.
This checks numerical ray behavior, not screenshot equivalence.

Release build passed. D3D12 and Vulkan full scene validation each reported
`failures=0, RR executions=448, creates=6, history resets=13` across 19 stages.
Vulkan confirmed TraceRay fallback; its existing RGB10A2 UAV limitation still
skips standalone NRD/TAA/SR validation stages. D3D12 ran those stages.
The first short scene run did not reach the final stage before closing and is
excluded; the complete runs are `ho-verified-d3d12` and `ho-verified-vk`.

Build using `tests/build_visibility_test.bat` after the RTXPT Release build. The
script copies the application's pinned runtime beside the test executable.

```bat
VisibilityRayTest.exe <GraphicsEngineD3D12_64r.dll> <shader-root> uint32 hitobject
VisibilityRayTest.exe <GraphicsEngineD3D12_64r.dll> <shader-root> uint16 hitobject
```

## Measurement protocol

RTX 5060; Release D3D12; convergence-test; fixed default camera; output 1920x1080,
input 1114x626; RR Balanced preset E; 18 total / 2 diffuse bounces; 1 spp;
3 stable planes; NEE-AT with 5 candidates and 1 full sample; native FP16;
inline visibility; generic materials. All three modes use the same executable,
compiler and runtime. GPU applications run serially.

Each mode is captured twice in forward/reverse order. Warmup is 35 seconds,
followed by eight window-title samples at two-second intervals. Reported GPU
means use the last 1,000 frames of each capture, with at least 2,000 valid frames
required. Window FPS is not substituted for GPU timestamp results.

Raw logs, CSVs and settings are under `%TEMP%/rtxpt-perf-diagnosis/ho-delivery-*`.
The measurement script, validation logs and binary hashes are under
`%TEMP%/rtxpt-hitobject`. Earlier `ho-smoke` and `ho-final` captures are exploratory
and excluded from the delivery measurements.

## Results (2026-09-23)

Milliseconds, average of the two capture means:

| Mode | BUILD | LightsBaker | FILL | RR | GPU total |
| --- | ---: | ---: | ---: | ---: | ---: |
| TraceRay | 0.323 | 0.431 | **7.785** | 2.868 | **12.027** |
| HitObject | 0.323 | 0.440 | **7.783** | 2.853 | **12.010** |
| HitObject + reorder | 0.322 | 0.434 | **3.852** | 2.850 | **8.070** |

Per-capture FILL means: TraceRay **7.749 / 7.821**, HitObject **7.833 / 7.732**,
reorder **3.896 / 3.809**. Per-capture total means: **11.976 / 12.077**,
**12.071 / 11.949**, **8.141 / 8.000**, respectively.

Relative to TraceRay, reordering saves **3.933 ms FILL (50.5%)** and
**3.956 ms total (32.9%)**. Relative to HitObject without reordering, it saves
**3.930 ms FILL**. HitObject alone has no established gain here: its two-run
mean differs from TraceRay by only 0.002 ms, much less than run variation.
Thus the measured improvement is primarily the reordering step. BUILD,
LightsBaker and RR remain similar. The inverse GPU totals correspond to about
83 and 124 frames/s; they are not independent CPU/presentation FPS measurements.

These are fresh baselines with the upgraded compiler/runtime. Do not subtract
them from old compiler captures to infer an isolated HitObject benefit. Likewise,
the earlier upstream material/SER experiment was a separate session; it is not
a new matched upstream measurement in this report.

The audit checked identical camera/render/settings logs, finite nonnegative GPU
durations, unique frame IDs, stable per-mode raygen cache hashes across repetitions,
three distinct selected raygen variants, and unchanged executable/runtime hashes.
Measurement executable SHA-256:
`5df656f24a95a5a637dfeca3df0beee2a5002518aa8fc1a4397557f94e7b1365`.
Full hashes and individual means are recorded in `hashes.json` / `audited.json`.

Final numerical ray tests passed D3D12 HitObject/reorder and Vulkan fallback with
both index widths. A validation-enabled D3D12 smoke run also passed with
`--inline_shadows false`, exercising HitObject scatter together with TraceRay
visibility and its separate miss/any-hit shader exports. No image comparison was
performed. Formatting and `git diff --check` passed.
