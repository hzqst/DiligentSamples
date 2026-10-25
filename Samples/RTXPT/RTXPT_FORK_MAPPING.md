# RTXPT Fork Mapping

## SHaRC integration (#23, 2026-10-26)

Reference: RTXPT-fork `9503910839f013f7ee0a2590a1131d84160ca712` and its
shader-only SHaRC SDK `4e21b585c33c83d723ca9a1e11bbb1090d145793` (1.8.3).
The sample fetches the pinned SDK, or accepts `RTXPT_SHARC_ROOT`; SDK sources
and Diligent public APIs are unchanged. Headers are deployed alongside shaders.

`SharcIntegration` owns 64-bit hash entries (8 bytes), accumulation (16 bytes),
resolved radiance (16 bytes), and a 64-byte scratch slot per training ray.
The scratch state survives TraceRay/HitObject without expanding the payload.
Frame order: BUILD, LightsBaker, sparse reference-mode training, resolve,
FILL/query, guides and post-processing. Explicit UAV barriers order producers
and consumers. Training never writes surface exports or the displayed image.
Reference always bypasses the cache. Training uses 8 surface / 4 diffuse bounces,
no Russian roulette, per-segment throughput, separate emission, material
demodulation, and a roughness floor only on non-delta surfaces.
Separate emission is MIS-weighted before both cache training and lookup, avoiding
double counting with the incoming path's NEE. Training bypasses the sample's
separate Reference closest-hit implementation so cache updates actually execute.
Path-tracing and SHaRC shaders explicitly use DXC O3 even in Debug; the known
unoptimized DXC path must not be used for correctness validation.

Preserved differences: Diligent binds named resources instead of an NVRHI
descriptor heap. Constants use a separate 64-byte constant buffer. FILL cannot
query on the deterministic denoising-layer branch, whose contribution BUILD
already owns. Existing light sampling, material specialization and native NGX
remain in place. No asynchronous compute or IntroSample integration is added.

Support is currently D3D12 SM 6.6+ with native FP16 buffer access. The main
path-tracer math may still use FP32. Vulkan explicitly reports unavailable and
renders without SHaRC: the current backend does not enable shaderInt64 or
shaderBufferInt64Atomics. This is an unsupported configuration, not a Vulkan
SHaRC runtime pass. The SDK spin-lock/compact-key fallbacks are not substituted.

Controls: `--sharc`, `--sharc_capacity_log2` (16..24), `--sharc_downscale` (2..5),
`--sharc_update`, `--sharc_resolve`, `--sharc_debug` (0..6), plus runtime controls
for scale, history, roughness and termination. Default is off. Debug views are
radiance, hash, occupancy, collisions, voxel size and level.

Scene reload, camera selection/cut, material/environment/grid/training changes,
history reset and resize invalidate the cache. Ordinary camera motion uses
world-space grid history. Successful skinning/BLAS updates clear the cache each
frame conservatively; animated scenes therefore lose temporal-cache benefit.
`--scene_animations true` enables the existing animation path for validation.
Disabling SHaRC or entering Reference releases cache/scratch resources; Diligent
retains in-flight allocations as necessary. Query/debug/update switches reset
image histories without destroying the cache being inspected. Clear/resolve use
a 2D dispatch to keep the 2^24 capacity below the per-axis group limit.
Shader/PSO cache is `RTXPTCache_016.bin`.

See [SHaRC validation](tests/README.md#sharc-validation-issue-23).

## v1.9.0 existing-feature synchronization (2026-10-26)

Target: RTXPT-fork `9503910839f013f7ee0a2590a1131d84160ca712` (v1.9.0 plus
fork fixes). Local starting point: DiligentSamples `50774cdc`. The local renderer
was already an adapted subset, not a complete v1.8.1 implementation. This section
supersedes the historical naming/layout contracts below where they conflict.
Algorithm changes in this synchronization are not subject to the earlier
byte-identical-image requirement. Pure refactors retain their existing contracts.

| Area | Status and current mapping |
| --- | --- |
| BSDF and hit organization | Ported: `BSDFs/`, `Surface/ShadingData.hlsli`, `PathTracerHandleHit.hlsli`. `StandardBSDFData` holds four packed words; `SurfaceBSDFData` is the local adapter for shading state. |
| Material transport | Ported: sRGB8/A8 material packing, 0.04 roughness threshold, surface-owned interior/exterior IOR, scalar position-error ray offset, geometric-normal shadow termination. |
| Denoising layers | Ported: `DenoisingLayers.hlsli` and `PathTracerDenoisingLayers.hlsli`, BUILD/FILL constants, payloads, resources and UI. Layer stride remains 80 bytes, with signed world-space motion in `PackedThpAndMVs`. NRD prepare projects it to the local screen-space convention. |
| Albedo guides | Ported: raw layer estimates permit zero; NRD applies 0.04 at both demodulation and remodulation; RR uses 0.0001. |
| RR preparation | Ported shader entry: `DenoisingGuidesBaker.hlsl::PrepareDLSSRRGuides`, with common code in `DenoisingGuidesRR.hlsli`; adds RG16F specular motion and native NGX binding. Invalid source data falls back to ordinary motion. |
| NEE-AT | Ported: 2,097,152 total light descriptors, 3,145,728 maximum proxy storage, 64-triangle source-local clusters, distance/brightness LOD, end trimming, leaf expansion, forward/reverse PDF and feedback. Cluster descriptors count against the 2M limit. CPU rejects overflow before narrowing to 32 bits. |
| Environment hierarchy | Ported: 20 subdivision steps and 12 boosted levels (2,812 nodes). Existing environment bake and Diligent texture upload remain. |
| Tone mapping | Ported: disabled tone mapping also bypasses auto exposure. GPU luminance reduction remains local. |
| Cache | `RTXPTCache_014.bin`; old 013 shader/PSO caches are not reused. |
| Realtime and profiling | Realtime is the default; existing asynchronous timestamp collection and CLI are retained. No second profiler or synchronous timing path was added. |
| Existing integrations | Kept: 32-byte packed vertices, local coordinate/matrix conventions, two-slot SBT, native HitObject/SER with TraceRay fallback, material specialization, native NGX, linear HDR and independent RR output. |

The host RR pass still uses `RTXPTPostProcessPass` for its tested resource
validation, barriers and dispatch; its shader now comes from the guide baker.
Mode 9 remains a compatibility entry. Existing `stable_planes` CLI arguments
remain supported despite the internal renaming. No public engine API changes.

Cluster membership is retained for inactive clusters so a previous local table
can still contain cluster and leaf proposals during an LOD transition. Local
leaf PDF sums both contributions; global proxies select only the current LOD.
Feedback uses luminance and the expanded leaf PDF, then routes to an active
cluster. Source history distinguishes cluster ranges and invalidates them when
the original triangle count changes. Clusters are rebuilt after emissive geometry
updates, including the existing skinning path; there is no new geometry owner.

Intentionally unimplemented in the original synchronization: SHaRC (subsequently
added above), IntroSample, RTXDI, OMM, Tracy, sub-1spp,
async-compute overlap, GPU TLAS, and BLAS pooling. Streamline/NVRHI scheduling,
upstream CMake/Agility/dependency upgrades are not copied. The existing pinned
NRD 4.17.3-series dependency is unchanged (its current runtime reports 4.17.4).
This is not a claim of complete v1.9.0 feature parity.

Validation and outstanding image/performance limits are recorded in
[the regression notes](tests/README.md#v190-synchronization-validation-2026-10-26).

## Native scatter HitObject (2026-09-23)

`PathTracerSample.rgen::nextHit` now follows upstream `PathTracerSample.hlsl`:
RayQuery scatter traversal, inline miss, HitObject, optional termination-hint
reorder, and Invoke of the generic closest-hit shader. BUILD does not reorder.
`PathTracerVisibility.hlsli::traceScatterRay` shares the production alpha predicate;
`makeScatterHit` translates the result to Diligent's two-slot geometry SBT layout.
Vulkan and D3D12 devices below SM 6.9 retain TraceRay. Material specialization is
unchanged. See [runtime requirements, validation and timings](tests/HITOBJECT_PERFORMANCE.md).

This document records the RTXPT-fork <-> DiligentEngine-port correspondence for
the RTXPT Phase R0.5 reference path tracer migration. Its purpose is to make
future upstream RTXPT re-ports close to a mechanical diff/merge while preserving
the current DiligentSamples rendering behavior.

Reference source spec: `docs/superpowers/specs/2026-05-30-rtxpt-reference-pathtracer-completion-design.md`
(relative to the superproject root, not the `DiligentSamples` submodule root), goal G0.5.

The original R0.5 sections below are historical. Their migration tasks applied exact
whole-word, case-sensitive renames and file relocations without changing shader
math, CPU/GPU struct byte layouts, or sample-framework glue behavior.

## Naming Rule

"Full alignment" does not mean copying RTXPT-fork's implementations or byte
layouts. The local reference path tracer now uses the shared
`PathState`/`PathPayload` transport spine, while several RTXPT-fork structs are
still broader than the Diligent port, such as `PathTracerConstants` fields for
ReSTIR, stable-plane, and DLSS features with no active Diligent analog yet.

Full alignment means:

1. Use the exact RTXPT-fork name when our symbol computes the same quantity with
   a compatible signature. Examples: our GGX NDF maps to `evalNdfGGX`; our
   cosine-hemisphere PDF helper maps to the closest matching upstream-style
   helper; `Hash32`/`Hash32Combine` already match.
2. Use RTXPT-fork naming style and the closest RTXPT-fork file/folder when there
   is no exact analog. Style includes PascalCase types/functions,
   `m_`/`g_`/`t_`/`u_`/`s_`/`k`/`c_` prefixes, camelCase locals and
   parameters, HLSL `namespace` plus inline helpers, and traditional include
   guards. Examples: our bespoke `RTXPTPathTracerSettings`, legacy
   compatibility payload structs, and our hybrid `RTXPTSurface`.
3. Never rename a symbol to an RTXPT-fork name whose semantics differ. Example:
   our `RTXPTVisibilitySmithGGX` returns `G/(4*NoV*NoL)` as combined
   visibility, while RTXPT-fork's `evalMaskingSmithGGXCorrelated` returns the
   masking term `G` alone. Renaming ours to that upstream name would be a lie.
   Use a port-style name such as `evalVisibilitySmithGGXCorrelated` and record
   the divergence.
4. Byte-identical output is a hard invariant. Apply renames as whole-word,
   case-sensitive find/replace and relocations only. Do not improve math,
   reorder operations, change literals, or touch struct byte layout. Every
   `static_assert(sizeof(...) == N)` must still hold unchanged.

Hard constraints:

- Files under `DiligentSamples/` keep Diligent's Apache/Diligent copyright
  header where one exists today, which applies to the C++ files.
- Shaders currently carry no header; keep them header-less.
- Do not add NVIDIA copyright headers anywhere.
- C++ sample-framework glue with no RTXPT-fork analog keeps Diligent
  conventions. This includes sample lifecycle, pass classes, and the Diligent
  binding model.
- Only GPU-facing mirrored structs and resource-binding variable names align
  with RTXPT-fork, because those are the CPU/GPU contract boundary that upstream
  merges cross.

## File-Move Map

| Current `shaders/...` | New `shaders/...` | RTXPT-fork analog |
|---|---|---|
| `RTXPTReference.rgen` | `PathTracer/PathTracerSample.rgen` | `PathTracer/PathTracerSample.hlsl` (raygen) |
| `RTXPTReference.rchit` | `PathTracer/PathTracerClosestHit.rchit` | hit handling in `PathTracer.hlsli`/`Scene/HitInfo.hlsli` |
| `RTXPTReference.rmiss` | `PathTracer/PathTracerMiss.rmiss` | miss handling in `PathTracer.hlsli` |
| `RTXPTReference.rahit` | `PathTracer/PathTracerAnyHit.rahit` | any-hit / alpha test |
| `RTXPTShaderShared.hlsli` | `PathTracer/PathTracerShared.h` | `PathTracer/PathTracerShared.h` |
| `RTXPTSceneBridge.hlsli` | `PathTracer/PathTracerBridge.hlsli` | `PathTracer/PathTracerBridge.hlsli` |
| `RTXPTBSDF.hlsli` | `PathTracer/Rendering/Materials/BxDF.hlsli` | `PathTracer/Rendering/Materials/BxDF.hlsli` |
| `RTXPTMaterialBridge.hlsli` | `PathTracer/Rendering/Materials/MaterialBridge.hlsli` | material loads in `PathTracerBridge*.hlsli` |
| `RTXPTLightSampling.hlsli` | `PathTracer/Lighting/PolymorphicLight.hlsli` | `PathTracer/Lighting/PolymorphicLight.hlsli` |
| `RTXPTEnvironment.hlsli` | `PathTracer/Lighting/EnvMap.hlsli` | `PathTracer/Lighting/EnvMap.hlsli` |
| `RTXPTRandom.hlsli` | `PathTracer/Utils/SampleGenerators.hlsli` | `PathTracer/Utils/SampleGenerators.hlsli` |
| `RTXPTCommon.fxh` | `RTXPTCommon.fxh` (stays flat - port compat wrapper) | none |
| `RTXPTDebugCompute.csh` | `RTXPTDebugCompute.csh` (stays flat) | none |
| `RTXPTBlit.vsh` / `RTXPTBlit.psh` | unchanged (stay flat) | none |

## Authoritative Rename Tables

### T-A. Macros, Include Guards, Constants

| Current | New | Notes |
|---|---|---|
| `RTXPT_ENABLE_HIT_BRIDGE` | `ENABLE_HIT_BRIDGE` (style) | HLSL-only define |
| `RTXPT_ENABLE_MATERIAL_TEXTURES` | `ENABLE_MATERIAL_TEXTURES` (style) | also a C++ `ShaderMacroHelper` string |
| `RTXPT_MATERIAL_TEXTURE_COUNT` | `MATERIAL_TEXTURE_COUNT` (style) | also a C++ `ShaderMacroHelper` string |
| include guards `RTXPT_*_HLSLI` | `__<NAME>_HLSLI__` (= RTXPT-fork form, e.g. `__BXDF_HLSLI__`) | RTXPT-fork uses `__NAME_HLSLI__` traditional guards |
| `RTXPT_PI` | `K_PI` = RTXPT-fork | `Utils/Math/MathConstants.hlsli` |
| `RTXPT_INV_PI` | `K_1_PI` = RTXPT-fork | |
| `RTXPT_VISIBILITY_RAY_TMIN` / `_TMAX` | `kVisibilityRayTMin` / `kVisibilityRayTMax` (style) | |
| `kRTXPTSubInstanceFlagIndexed` | `kSubInstanceFlagIndexed` (style) | also C++ `kRTXPTSubInstanceFlag_Indexed` -> `kSubInstanceFlag_Indexed` |
| `kRTXPTMaterialFlag*` (5) | `kMaterialFlag*` (style) | also C++ `kRTXPTMaterialFlag_*` |

### T-B. Utils Layer (`Utils/SampleGenerators.hlsli`)

| Current | New | Notes |
|---|---|---|
| `Hash32` / `Hash32Combine` / `Hash32ToFloat` | `Utils/NoiseAndSequences.hlsli` | R5 aligns reference-mode sample conversion; record if any hash constants intentionally remain port-specific |
| `struct RTXPTRandom` | `struct SampleGenerator` (style) | RTXPT-fork uses `SampleGeneratorType`; we keep one concrete struct |
| `RTXPTRandom_Init` | `SampleGenerator_make` (style) | |
| `NextFloat` | `sampleNext1D` = RTXPT-fork | param `inout SampleGenerator sg` |
| `NextFloat2` | `sampleNext2D` = RTXPT-fork | |
| `BuildOrthonormalBasis` | `BranchlessONB` = RTXPT-fork | params `normal, out tangent, out bitangent` |
| `SampleCosineHemisphere` | `sampleCosineHemisphere` (style) | RTXPT-fork's local-frame helper differs; keep ours and record divergence |
| (R1/G3 new) | `SampleGenerator_makeStateless` | stateless per-(pixel,vertex,sample) seed; mirrors RTXPT `UniformSampleSequenceGenerator::make` |
| `StatelessSampleGenerators.hlsli` | `Utils/StatelessSampleGenerators.hlsli` | BSDF-only Sobol/Owen sample blocks through `SampleSequenceGenerator::Generate` |
| `UniformSampleSequenceGenerator` | `UniformSampleSequenceGenerator` | Uniform fallback for BSDF sample blocks after the diffuse-bounce LD window or when the macro is disabled |
| (R1/G3 new) | `kSampleEffect_*` | mirrors RTXPT `SampleGeneratorEffectSeed` (Base/ScatterBSDF/NextEventEstimation/NEELightSampler/RussianRoulette) |
| (R2/G4 new) | `kSampleEffect_NEEEmissive` | fills the previously unused effect slot 4 for emissive-triangle NEE |

### T-C. Materials Layer (`Rendering/Materials/BxDF.hlsli`)

| Current | New | Notes |
|---|---|---|
| `struct RTXPTSurface` | `struct StandardBSDFData` (style) | carries shading normal `N`; RTXPT-fork splits `ShadingData`/`StandardBSDFData` |
| field `.N` | `.N` = RTXPT-fork | |
| field `.DiffuseAlbedo` | `.diffuse` = RTXPT-fork style | plain public field, no fp16 packing |
| field `.F0` | `.specular` = RTXPT-fork style | |
| field `.Alpha` | `.alpha` (style) | GGX alpha (= roughness^2) |
| `RTXPTMakeSurface` | `MakeStandardBSDFData` (style) | |
| `RTXPTFresnelSchlick(F0, VoH)` | `evalFresnelSchlick(f0, f90, cosTheta)` = RTXPT-fork | call with `f90=float3(1,1,1)` |
| `RTXPTDistributionGGX(NoH, Alpha)` | `evalNdfGGX(alpha, cosTheta)` = RTXPT-fork | param order swaps |
| `RTXPTVisibilitySmithGGX(NoV, NoL, Alpha)` | `evalVisibilitySmithGGXCorrelated(alpha, cosThetaO, cosThetaI)` (divergent) | returns `G/(4*NoV*NoL)`, not bare masking `G` |
| `kMinGGXAlpha` | `kMinGGXAlpha` | Near-mirror specular collapses to delta reflection |
| `sampleGGX_BVNDF` / `evalPdfGGX_BVNDF` | `Microfacet.hlsli` | Bounded-VNDF GGX reflection sampling and matching pdf |
| `evalDiffuseFrostbiteWeight` | `DiffuseReflectionFrostbite::evalWeight` | Frostbite/Disney energy-conserving diffuse |
| `DiffuseReflectionFrostbite` | `DiffuseReflectionFrostbite` | Frostbite diffuse reflection component; Diligent wrapper maps world `wo` to local reference `wi` |
| `DiffuseTransmissionLambert` | `DiffuseTransmissionLambert` | Opposite-hemisphere Lambertian diffuse transmission |
| `SpecularReflectionMicrofacet` | `SpecularReflectionMicrofacet` | GGX BVNDF reflection, active-lobe gated, delta reflection when roughness falls below `kMinGGXAlpha` |
| `SpecularReflectionTransmissionMicrofacet` | `SpecularReflectionTransmissionMicrofacet` | GGX dielectric reflection/transmission, thin-surface eta override for transmission only |
| `FalcorBSDF` | `FalcorBSDF` | Four-component mixture with active-lobe gating, reference mixture pdf additions, sampled delta `pdf = 0` |
| `FalcorBSDF::evalDeltaLobes` | `ActiveBSDF::evalDeltaLobes` bridge | Exports transmission delta slot `0` and reflection delta slot `1` for stable-plane branch IDs |
| `MaterialHeader` PSD accessors | `Scene/Material/MaterialData.hlsli` plus `StablePlaneMaterialState` | Minimum PSD compatibility surface: exclude, block motion vectors at surface, dominant delta lobe plus one |
| `MultiScatterSpecularApprox` | `MultiScatterSpecularApprox` | Turquin multi-scatter specular compensation |
| `RTXPTLuminance` | `luminance` = RTXPT-fork | |
| `RTXPTSpecularProbability` | `getSpecularProbability` (style) | |
| `RTXPTEvalBSDF(S, Wo, Wi, SpecProb, out FTimesNoL, out Pdf)` | `EvalBSDF(bsdfData, wo, wi, specProb, out f, out pdf)` (style) | combined helper |
| `RTXPTSampleBSDF(S, Wo, sample, ..., out Wi, out Weight, out Pdf)` | `SampleBSDF(bsdfData, wo, sample, out wi, out weight, out pdf, out lobe, out lobeP)` (style) | R5 consumes pregenerated BSDF samples and returns sampled lobe plus sampled-lobe probability |

### T-D. Lighting Layer (`Lighting/PolymorphicLight.hlsli`, `Lighting/EnvMap.hlsli`)

The packed-storage migration supersedes the historical unpacked-light field
rename tables below. GPU storage now follows upstream's 32-byte
`PolymorphicLightInfo` plus separate 16-byte `PolymorphicLightInfoEx`; see
[storage, validation and performance notes](tests/PACKED_LIGHT_STORAGE_PERFORMANCE.md).

| Current | New | Notes |
|---|---|---|
| `struct RTXPTLightSample` | `struct LightSample` = RTXPT-fork | |
| fields `.Wi/.Distance/.Radiance/.Valid` | `.dir/.distance/.radiance/.valid` (style) | |
| `RTXPTInvalidLightSample` | `LightSample_make_empty` (style) | |
| `RTXPTNormalizeDirection` | `tryNormalize` (style) | |
| `RTXPTEvalAnalyticLight(Light, SurfacePos)` | `SampleAnalyticLight(light, random, surfacePos)` (style) | |
| `RTXPTEvalSky(RayDir)` | `EnvMapSampler::Eval(worldDir, lod)` | R4 routes miss and env NEE through baked env-map resources; procedural sky is a source/fallback path |
| `TriangleLight` (struct) | `Lighting/LightPacking.hlsli` + decoded `EmissiveTriangle` | Packed GPU record: FP32 centroid, three FP16 edge pairs, RGB8/log16 radiance. `EmissiveTriangle` is now a local decoded value, not a storage layout. |
| `PathTracer/Lighting/LightSampler.hlsli` `NEEWeightedReservoirSampler` / `GenerateLightSample` | `PathTracer/Lighting/LightSampler.hlsli` `NEEWeightedReservoirSampler` / `GenerateDirectLightCandidate` / `SampleDirectLightNEE` | R3/G5 ports the RIS/WRS math as part of the full `LightsBaker` parity target |
| `LightSampler::SampleGlobal` proxy table | `RTXPTLightsBaker` `t_LightProxyCounters` + `t_LightSamplingProxies` | Unified light index space and packed storage `[environment | analytic | emissive triangles]`. |
| `LightSampler::SampleLocal` local proxy table | `RTXPTLightsBaker` `t_LocalSamplingBuffer` | populated by baker-owned local sampling passes |
| `PrepareLights.hlsl` / `LightsBaker.hlsl` `BakeEmissiveTriangles` emissive-texture sampling | `EmissiveTriangleBuild.hlsl` `computeTriangleRadiance` | centroid + anisotropic `SampleGrad`; identical short/long-edge gradient logic |
| `TriangleLight::CalcSample` (one-sided) | `GenerateDirectLightCandidate` emissive branch + `PathTracerClosestHit.rchit` frontFacing emission/pdf gates | R2-done: one-sided (front-face only) NEE/MIS/emission; winding fix on mirrored instances |
| `LightsBaker.hlsl` `ComputeWeight` / `ComputeWeights` / `ComputeProxyCounts` / `CreateProxyJobs` / `ExecuteProxyJobs` | `LightProxyBuild.hlsl` `ComputeWeightsCS` / `ComputeProxyCountsCS` / `ScatterProxiesCS` (+ `ResetProxyBuildCS`) driven by `RTXPTLightProxyBuildPass` | GPU per-light power weights + power-proportional proxy table; atomic running-offset scatter replaces upstream prefix-sum + task system (same per-light proxy counts; only the proxy-table order differs, which sampling is invariant to) |
| `TriangleLight::GetPower` (`area*PI*Luminance`) | `LightProxyBuild.hlsl` `ComputeEmissiveTriangleWeight` (`pow(area*PI*Luminance, 0.8)`) | analytic weights keep the prior `EstimateAnalyticWeight` heuristic |
| `SampleTriangleUniform` / `pdfAtoW` / `MAX_SOLID_ANGLE_PDF` | `SampleTriangleUniform` / `pdfAtoW` / `kMaxSolidAnglePdf` (style) | |
| `LightsBaker` emissive triangle list | `RTXPTLights::UploadEmissiveTriangles` + `RTXPTEmissiveTrianglePass` (GPU build from current geometry) | full `LightsBaker` parity target; keep the Diligent-native scene plumbing |

### T-E. PathTracer Core (`PathTracer/PathTracer.hlsli`, `PathTracerHelpers.hlsli`, Raygen)

| Current | New | Notes |
|---|---|---|
| `RTXPTPowerHeuristic(PdfA, PdfB)` | `PowerHeuristic(nf, fPdf, ng, gPdf)` = RTXPT-fork | call with `(1, pdfA, 1, pdfB)` |
| (R1/G1 new) | `ComputeNewScatterFireflyFilterK` / `FireflyFilter` / `FireflyFilterShort` / `Average` = RTXPT-fork | runtime-gated by `fireflyFilterThreshold==0` instead of RTXPT's compile-time `RTXPT_FIREFLY_FILTER`; uses `acos`/`sqrt` not `FastACos`/`FastSqrt` |
| `RTXPTTraceVisibility(Origin, Dir, TMax)` | `PathTracer::TraceVisibilityRay(origin, dir, tMax)` (style) | |
| `RTXPTSampleAnalyticNEE(...)` | `PathTracer::SampleAnalyticNEE(...)` (style) | |
| `RTXPTSampleEnvNEE(...)` | `PathTracer::SampleEnvironmentNEE(...)` (style) | |
| `LightSampler::ComputeLightVsBSDF_MIS_ForBSDF` | `ComputeLightVsBSDFMISForBSDF` | Shared balance/approximate MIS for emissive hits and environment misses, including FILL. |
| `EnvironmentQuadLight` reverse lookup | `t_EnvironmentLightLookup` + `EnvironmentQuadSolidAnglePdf` | Environment-first indices; triangles start at `EnvmapQuadNodeCount + AnalyticLightCount`. |
| `ToneMapACES` | `ToneMapACES` (unchanged) | port-specific |

Raygen locals become camelCase:

| Current | New |
|---|---|
| `WorldPos4` | `worldPos4` |
| `Origin` | `origin` |
| `RayOrigin` | `rayOrigin` |
| `RayDir` | `rayDir` |
| `Throughput` | `throughput` |
| `PathRadiance` | `pathRadiance` |
| `PrevBsdfPdf` | `prevBsdfPdf` |
| `PrevNormal` | `prevNormal` |
| `PrevDidEnvNEE` | `prevDidEnvNEE` |
| `MaxBounces` | `maxBounces` |
| `MaxNEEBounces` | `maxNEEBounces` |
| `EnableNEE` | `enableNEE` |
| `EnableEnvNEE` | `enableEnvNEE` |
| `Bounce` | `bounce` |
| `Pixel` | `pixel` |
| `Dimensions` | `dimensions` |
| `SampleIndex` | `sampleIndex` |
| (R1/G1 new) | `fireflyFilterK` |
| (R1/G1 new) | `ffThreshold` |
| (R1/G3 new) | `vertexIndex` |
| (R1/G3 new) | `sgCamera` / `sgNEELight` / `sgEnvNEE` / `sgScatter` / `sgRR` |
| `Jitter` | `jitter` |
| `UV` | `uv` |
| `NDC` | `ndc` |
| `NextDir` | `nextDir` |
| `Weight` | `weight` |
| `Pdf` | `pdf` |
| `Wo` | `wo` |
| `Surface` | `bsdfData` |
| `Bias` | `bias` |
| `VisibilityOrigin` | `PathTracer::MakeVisibilityOrigin(...)` |
| `UseNEE` | `useNEE` |
| `Survive` | `survive` |
| `Accumulated` | `accumulated` |
| `Reset` | `reset` |
| `Frame` | `frame` |
| `InvN` | `invN` |
| `Previous` | `previous` |
| `MISWeight` | `misWeight` |
| `EnvRadiance` | `envRadiance` |
| (R1/G1 new) | `environmentEmission` |
| (R1/G1 new) | `surfaceEmission` |
| (R1/G1 new) | `lobeP` |

### T-F. Bridge Namespace (`PathTracerBridge.hlsli`, `MaterialBridge.hlsli`)

| Current `Bridge::` | New `Bridge::` | Notes |
|---|---|---|
| `GetLightCount` | `getLightCount` (style) | |
| `GetLight` | `getLight` (style) | |
| `GetSubInstanceIndex` | `getSubInstanceIndex` (style) | |
| `GetSubInstanceData` | `getSubInstanceData` (style) | |
| `HasSubInstanceTable` | `hasSubInstanceTable` (style) | |
| `GetTriangleIndices` | `getTriangleIndices` (style) | |
| `GetTriangleVertices` | `getTriangleVertices` (style) | |
| `InterpolateNormal` / `InterpolateTexCoord` | `interpolateNormal` / `interpolateTexCoord` (style) | |
| `ComputeGeometricNormal` | `computeGeometricNormal` (style) | |
| `ComputeWorldHitPosition` | `computeWorldHitPosition` (style) | |
| `ComputeWorldTangent` | `computeWorldTangent` (style) | |
| `HasMaterialTable` / `GetMaterialCount` / `GetMaterial` | `hasMaterialTable` / `getMaterialCount` / `getMaterial` (style) | |
| `SampleMaterialTexture` | `sampleMaterialTexture` (style) | |
| `GetBaseColor` / `GetEmission` / `GetMetallicRoughness` / `GetTangentNormal` | `getBaseColor` / `getEmission` / `getMetallicRoughness` / `getTangentNormal` (style) | |
| `AlphaTestPasses` | `alphaTestPasses` (style) | |

### T-G. Shared CPU/GPU Structs - Type Names

| Current type | New type | RTXPT-fork |
|---|---|---|
| `RTXPTSubInstanceData` | `SubInstanceData` = RTXPT-fork | `Shaders/SubInstanceData.h` |
| `RTXPTPathTracerSettings` | `PathTracerConstants` = RTXPT-fork (name) | `PathTracerShared.h` (fields differ - divergent) |
| `RTXPTFrameConstants` | `SampleConstants` (style) | RTXPT-fork `SampleConstantBuffer.h` |
| `RTXPTMaterialHitPayload` | `RTXPTMaterialHitPayload` | Legacy compatibility payload; reference primary and visibility rays now use packed `PathPayload.hlsli` |
| (Realtime G4/G5 new, reference-unified) | `PathPayload` / `PathState` = RTXPT-fork | Shared path-state payload for reference, build, and fill variants |
| `RTXPTPrimaryPayload` | `PrimaryPayload` (style) | compatibility-only |
| `RTXPTMaterialData` | `MaterialPTData` = RTXPT-fork (name) | `Materials/MaterialPT.h` (fields differ - divergent) |
| `RTXPTLightData` | `Lighting/PolymorphicLight.h` `PolymorphicLightInfo` / `PolymorphicLightInfoEx` | Upstream 32-byte base + separate 16-byte extension; CPU authoring values use `AnalyticLightData`. |
| `RTXPTVertex` | `GeometryVertexData` (style) | RTXPT-fork uses separate attribute buffers - divergent |

### T-H. Shared Struct Field Names

| Struct | Current | New |
|---|---|---|
| `SubInstanceData` | `MaterialID` | `MaterialID` |
| `SubInstanceData` | `Flags` | `Flags` |
| `SubInstanceData` | `FirstIndex` | `IndexOffset` |
| `SubInstanceData` | `IndexCount` | `IndexCount` |
| `SubInstanceData` | `FirstVertex` | `VertexOffset` |
| `SubInstanceData` | `VertexCount` | `VertexCount` |
| `SubInstanceData` | `Padding0` / `Padding1` | `_padding0` / `_padding1` |
| `PathTracerConstants` | `MaxBounces` | `bounceCount` |
| `PathTracerConstants` | `AccumulationFrame` | `sampleIndex` |
| `PathTracerConstants` | `ResetAccumulation` | `resetAccumulation` |
| `PathTracerConstants` | `MinBounces` | `minBounceCount` |
| `PathTracerConstants` | `EnableNEE` | `NEEEnabled` |
| `PathTracerConstants` | `EnableEnvNEE` | `environmentNEEEnabled` |
| `PathTracerConstants` | `EnvIntensity` | `environmentIntensity` |
| `PathTracerConstants` | `LightIntensityScale` | `lightIntensityScale` |
| `PathTracerConstants` | `MaxNEEBounces` | `maxNEEBounceCount` |
| `PathTracerConstants` | `AnalyticLightCount` | `analyticLightCount` |
| `PathTracerConstants` | `Padding1` / `Padding2` | `_padding0` / `exposureScale` |
| `SampleConstants` | `ViewProj` | `viewProj` |
| `SampleConstants` | `ViewProjInv` | `viewProjInv` |
| `SampleConstants` | `CameraPosition_Time` | `cameraPositionAndTime` |
| `SampleConstants` | `ViewportSize_FrameIdx` | `viewportSizeAndFrameIndex` |
| `SampleConstants` | `PathTracer` | `ptConsts` |
| `RTXPTMaterialHitPayload` | `WorldPos` | `worldPos` |
| `RTXPTMaterialHitPayload` | `HitDistance` | `hitDistance` |
| `RTXPTMaterialHitPayload` | `WorldNormal` | `worldNormal` |
| `RTXPTMaterialHitPayload` | `HitFlag` | `hitFlag` |
| `RTXPTMaterialHitPayload` | `BaseColor` | `baseColor` |
| `RTXPTMaterialHitPayload` | `Metallic` | `metallic` |
| `RTXPTMaterialHitPayload` | `Emission` | `emission` |
| `RTXPTMaterialHitPayload` | `Roughness` | `roughness` |
| `MaterialPTData` | `BaseColorFactor` | `baseColorFactor` |
| `MaterialPTData` | `EmissiveFactor` | `emissiveFactor` |
| `MaterialPTData` | `AlphaCutoff` | `alphaCutoff` |
| `MaterialPTData` | `Flags` | `flags` |
| `MaterialPTData` | `BaseColorTextureIndex` | `baseColorTextureIndex` |
| `MaterialPTData` | `EmissiveTextureIndex` | `emissiveTextureIndex` |
| `MaterialPTData` | `MetallicFactor` | `metallicFactor` |
| `MaterialPTData` | `RoughnessFactor` | `roughnessFactor` |
| `MaterialPTData` | `BaseColorTextureSlice` | `baseColorTextureSlice` |
| `MaterialPTData` | `EmissiveTextureSlice` | `emissiveTextureSlice` |
| `MaterialPTData` | `MetallicRoughnessTextureIndex` | `metallicRoughnessTextureIndex` |
| `MaterialPTData` | `MetallicRoughnessTextureSlice` | `metallicRoughnessTextureSlice` |
| `MaterialPTData` | `NormalTextureIndex` | `normalTextureIndex` |
| `MaterialPTData` | `NormalTextureSlice` | `normalTextureSlice` |
| `MaterialPTData` | `NormalScale` | `normalScale` |
| `MaterialPTData` | `Padding0` / `Padding1` / `Padding2` / `Padding3` | `_padding0` / `_padding1` / `_padding2` / `_padding3` |
| `PolymorphicLightInfo` | `ColorType` | `colorType` |
| `PolymorphicLightInfo` | `PositionRadius` | `positionRadius` |
| `PolymorphicLightInfo` | `DirectionRange` | `directionRange` |
| `PolymorphicLightInfo` | `Shaping` | `shaping` |
| `GeometryVertexData` | `Position` | `position` |
| `GeometryVertexData` | `Normal` | `normal` |
| `GeometryVertexData` | `TexCoord0` | `texCoord0` |

### T-I. Resource-Binding Variable Names

| Current HLSL global | New | C++ touch points |
|---|---|---|
| `g_FrameConstants` | `g_Const` | `RTXPTRayTracingPass.cpp`, `RTXPTCommon.fxh`, all shaders |
| `g_TLAS` | `t_SceneBVH` | `RTXPTRayTracingPass.cpp` |
| `g_OutputColor` (path-trace output UAV) | `u_OutputColor` | `RTXPTRayTracingPass.cpp` |
| `g_AccumColor` | `u_AccumulationBuffer` | `RTXPTRayTracingPass.cpp` |
| `g_Lights` | `t_Lights` | `RTXPTRayTracingPass.cpp` |
| `g_EmissiveTriangles` | `t_EmissiveTriangles` | `RTXPTRayTracingPass.cpp`, `RTXPTLights.cpp` |
| `g_SubInstanceData` | `t_SubInstanceData` | `RTXPTRayTracingPass.cpp` |
| `g_VertexBuffer` | `t_VertexBuffer` | `RTXPTRayTracingPass.cpp` |
| `g_IndexBuffer` | `t_IndexBuffer` | `RTXPTRayTracingPass.cpp` |
| `g_Materials` | `t_PTMaterialData` | `RTXPTRayTracingPass.cpp` |
| `g_MaterialTextures` | `t_BindlessTextures` | `RTXPTRayTracingPass.cpp` |
| `g_MaterialSampler` | `s_MaterialSampler` | `RTXPTRayTracingPass.cpp` |
| `g_InputColor` (debug-compute) | `t_InputColor` | optional/debug scope |
| `g_OutputColor` (debug-compute) | `u_Output` | optional/debug scope |

## Divergences

- Everything marked `(style)` in T-A..T-I is a lexical or casing alignment
  only. The exceptions below are the cases where the port must keep its own
  semantics or layout.
- Macro and guard renames drop the port prefix or adopt RTXPT-fork casing. R5
  uses `kMinGGXAlpha` as the GGX alpha floor and collapses near-mirror
  specular events to delta reflection.
- `SampleGenerator` and the camelCase sampling helpers keep the port's concrete
  RNG implementation while R5 routes hash-to-float conversion through
  `Hash32ToFloat`. `sampleCosineHemisphere` still
  returns a basis-rotated world-space direction, unlike RTXPT-fork's local-frame
  helper.
- Reference mode uses the shared `PathState`/`PathPayload` loop and commits raw
  HDR radiance through `PathTracer::CommitPixel`.
- Reference-specific branches in shared spine functions preserve the local
  estimator while BUILD/FILL stable-plane side effects remain mode-guarded.
- Reference diffuse-bounce classification is aligned with upstream/realtime:
  diffuse reflection, diffuse transmission, or roughness above
  `kSpecularRoughnessThreshold` count as diffuse-like, with diffuse transmission
  incrementing the counter only every other vertex.
- `Bridge::` runs over Diligent structured buffers, not Donut/NVRHI. There is
  no `PathTracerBridgeDonut.hlsli` equivalent here.
- `StandardBSDFData` carries the shading normal `N`, while R5 aligns the
  reflection model with bounded-VNDF GGX, Frostbite/Disney diffuse, and Turquin
  multi-scatter specular compensation.
- `evalVisibilitySmithGGXCorrelated` returns `G/(4*NoV*NoL)`, not RTXPT-fork's
  bare masking `G`.
- Emissive-triangle area lights are now **one-sided** like RTXPT-fork's `TriangleLight`:
  signed `cosTheta` rejects back-face NEE samples, the BSDF-hit emissive pdf and the
  surface emission are gated on `frontFacing`, and `EmissiveTriangleBuild.hlsl` swaps
  edges on mirrored instances so the emitter normal matches the geometric front face.
  Textured-emissive materials are always promoted to area lights: bindless is a hard device
  requirement (`Features.BindlessResources = DEVICE_FEATURE_STATE_ENABLED` in
  `ModifyEngineInitInfo`), so the build shader can always bake `emissiveFactor *
  emissiveTexture` per triangle (matching `PrepareLights.hlsl`). There is no factor-only
  *capability* fallback — factor-only radiance is used only for scenes that contain no
  material textures at all.
  Emissive triangles are importance-sampled **per-triangle** proportional to power via the
  GPU proxy build (`LightProxyBuild.hlsl` / `RTXPTLightProxyBuildPass`), replacing the
  earlier single "emissive bucket". The September 2026 sampler alignment restores
  upstream's minimum proxy budget (`12 * max(lightCount, MAX_LIGHTS / 10)`), sphere /
  point power and spot flux factors. NEE-AT mixes per-frame feedback counts into
  the global weights. Proxy scatter now uses 256-entry jobs and a two-dimensional
  indirect dispatch; atomic job allocation replaces upstream's prefix sum without
  changing per-light multiplicities. Separate typed buffers hold environment,
  analytic and triangle records in one logical index space.

  Broader baker differences remain explicit: Diligent retains analytic directional
  lights instead of baking their disks into the source environment. Frustum and
  intensity-change importance boosts, source history-ID remapping and environment
  split/merge mapping are implemented (see the history alignment section below).
  Separate typed light storage and atomic proxy allocation still differ from the
  upstream representation; this is not bit-for-bit equivalence of the entire baker.
  Emissive-NEE enable/dirty state is gated in-shader via `Bridge::getEmissiveTriangleCount`,
  which reads the *enabled* triangle count packed into `ptConsts.environmentNEEEnabled`
  (0 when the UI toggle is off or the emissive pass is dirty), rather than the raw baked
  `LightingControlData::TriangleLightCount`.
- Resource/global names follow the RTXPT-fork `t_/u_/s_/g_` prefix scheme, but
  the set here is the reference-mode subset only. `t_PTMaterialData` is the
  material-buffer resource/global name backed by the local `MaterialPTData`
  type.
- `MaterialPTData`, `PolymorphicLightInfo`, `GeometryVertexData`,
  legacy `RTXPTMaterialHitPayload`, `PathTracerConstants`, and
  `SampleConstants` reuse RTXPT-fork names or style as local backing layouts.
  `MaterialPTData` remains port-specific and is not field-compatible with
  upstream `PTMaterialData`; the other local backing layouts also remain
  port-specific. `PathPayload` is the shared packed path-state bridge documented
  in G4/G5.
- **Material texture sampling (single-UV / single-wrap / no atlas transform) is an
  intentional deviation that matches upstream behavior, not an outstanding task.**
  `MaterialBridge.hlsli` samples every material texture with one interpolated UV set
  (TEXCOORD_0), one immutable wrap-address sampler (`s_MaterialSampler`), and a
  carried-but-unapplied atlas slice. Upstream `PathTracerBridgeDonut.hlsli` does the
  same: one baked texcoord channel per geometry (`texCoord1Offset`), one global
  `m_AnisotropicWrapSampler` bound as `s_MaterialSampler` for all material textures,
  and standalone bindless `Texture2D` loads with no atlas, no UV scale/bias, and no
  KHR_texture_transform. DiligentFX's glTF loader exposes `TextureShaderAttribs` UV
  selectors, wrap modes, and `AtlasUVScaleAndBias`, but these have no upstream analog,
  so there is nothing to re-port; honoring them would be a from-scratch addition and is
  intentionally not done while it matches upstream for the single-UV/wrap case. Revisit
  only if a scene needs per-texture UV sets, per-material wrap modes, or atlas/UV
  transforms. (Orthogonal, separately tracked: upstream computes texture LOD via ray
  cones / STF, while the port samples mip 0.)
- Shared struct fields follow T-H's upstream-style casing/alignment where
  applicable. Fields kept identical in T-H, such as `MaterialID`, `Flags`,
  `IndexCount`, and `VertexCount`, remain intentionally unchanged, and every
  `static_assert(sizeof(...) == N)` byte-layout contract stays unchanged.
- R4 routes environment lighting through `RTXPTEnvMapBaker` and
  `EnvMapSampler`: HDR/procedural input baking, processed cubemap output,
  importance/radiance maps, BRDF LUT generation, and Diligent-native RT
  resource binding. The procedural gradient remains as the `==PROCEDURAL_SKY==`
  source/fallback path instead of a shader-only runtime environment.
- `BxDF.hlsli` keeps the Fresnel/Microfacet helpers together in one file, while
  RTXPT-fork splits them into separate helper headers.
- `RTXPTCommon.fxh`, `RTXPTDebugCompute.csh`, and `RTXPTBlit.vsh` / `psh` keep
  their `RTXPT`-prefixed names because they sit outside the algorithm layer.

## NEE-AT local table performance contract (2026-09-14)

The initial stored-count repair (superseded by the temporal path below) used
`Lighting/LightsBaker.hlsl::FillLocalSamplingCS`, dispatching one 128-thread
workgroup per tile. It preserves the port's proxy multiset and first-maximum
feedback selection, sorts light indices with a groupshared bitonic network, and
packs each light's full occurrence count into every corresponding entry. The
pass writes every tile entry, including the zero-proxy case, so no separate
local-table clear is dispatched. Invalid/out-of-range feedback is rejected.

`LightSampler.hlsli::SampleLocalLightIndex` reads the packed count directly;
`SampleLocalPDF` uses `LightingTypes.hlsli::LocalLightBinarySearch`, matching the
upstream stored-count/sorted-search contract. Local candidates reuse their known
local PDF when forming the mixture. This replaces repeated 128-entry scans in
the path-tracing loop. Sorting changes the random-number-to-light mapping, but
not the local distribution or its mixed global/local PDF. The port's existing
feedback collection used a simplified maximum-weight tile representative. This
shader remains a deterministic table fixture for the PDF regression; production
now dispatches `LightFeedback.hlsl::FillFeedbackTile`.

`RTXPTGpuProfiler.hpp` provides opt-in asynchronous timestamp pairs, including
separate RR preparation/evaluation measurements without an active query across
NGX. See `tests/README.md` for GPU regression coverage, timing commands and cache
invalidation requirements.

### Unified sampler, MIS and temporal feedback (2026-09-14)

`GenerateDirectLightCandidate` evaluates only the BSDF PDF. The selected visible
sample performs cross-proposal lookup, late WRS balance MIS and full BSDF evaluation.
Candidate counts, global-first/local-second stratification and the ray-cone-width /
path-length coherence test follow upstream. Approximate light/BSDF MIS uses bias 3;
Full uses the sum of participating conditional PDFs. `NEEMISType` now specializes
each RT variant and can be changed in the UI. Both Reference and FILL apply reverse
MIS on emissive hits and environment misses. Specular radiance and feedback use
the upstream BSDF value and throughput-average conventions.

`RTXPTEnvironmentLights` ports base quadtree subdivision (16 + 24 * 3 = 88 leaves),
20 boost splits per base leaf (61 leaves each, 5368 total), packed FastSqrt priorities,
equal-area-octahedral sampling, baked radiance/importance and direction-to-leaf lookup.
Environment quads participate in the same reservoir as all other lights; the separate
environment NEE shadow ray is removed. The quadtree is rebuilt on environment changes,
with old/current lookup maps preserving feedback through changing subdivisions.
Unchanged trees use identity history mapping instead of repeating subdivision work.

`RTXPTLightFeedback` translates upstream preprocessing, SSC/WSC separation, global
feedback counting, depth/motion reprojection, 2x2 blended reservoirs, 8x8 windows,
64 top-up samples, R2 tile jitter and history decay. Active feedback/table dimensions
follow the internal render resolution. Resource ping-pong is represented by explicit
filtered/blended/processed textures. World-coherent feedback contributes to global
weights but is removed before local-table construction. No CPU readback is used.

The shared two-ray TLAS stride remains valid with inline visibility: the unused
visibility hit-group slot is explicitly null-bound. Cache version 007 invalidates
all preceding shader implementations. Regressions and measured limitations are in
`tests/README.md`.

### LightsBaker importance and history alignment (2026-09-14)

`LightProxyBuild.hlsl::ImportanceBooster` now follows upstream's five-plane frustum
distance, multiplier 8, fade distance 5, and half-strength boost for distant lights.
The camera planes are uploaded every rendered frame. The near-plane extraction is
translated for Diligent's forward-Z projection; copying upstream's reversed-Z near
plane would incorrectly use the far plane. Light centers use analytic positions or
triangle centroids. The intensity-delta boost uses multiplier 64 and the upstream
1.1 threshold against the previous **boosted** weight, before global-feedback mixing.
`RTXPTLightProxyBuildPass` retains those weights on the GPU between builds.

`RTXPTLightSourceHistory` matches analytic source nodes and emissive primitive ranges
independently of light-table offsets, transforms and radiance. `RTXPTLightHistory`
uploads both source maps and translates `EnvLightsMapPastToCurrent` plus the reverse
lookup from `EnvLightsSubdivideBoost`: each node's upper-left coordinate is looked
up in the other frame's map. This preserves non-bijective split/merge semantics.
Missing sources and changed triangle counts invalidate history. Maps only link the
immediately preceding rendered frame, so a removed/reappearing light cannot recover
an obsolete two-frame-old ID. Unchanged tables return to identity after a remap is
consumed. Environment snapshots and boosted weights stay on the GPU; no readback or
GPU-idle wait is added to rendering.

Feedback preprocessing remaps selected historic reservoirs before accumulating
global counts. Historic local-table fallback also remaps before entering current
reservoirs. Scene reload, resolution changes and explicit resets still invalidate
feedback; camera motion, ordinary animated geometry and environment UI changes no
longer unconditionally discard it. Image-denoiser resets remain independent.
Cache version **011** is required for the new bindings and constant layouts.

The environment history snapshot is allocated from the live environment buffer
description at first use, alongside the lookup snapshot. Allocating it earlier in
scene initialization triggered a Vulkan device-loss regression in the convergence
scene even with history dispatch disabled; SRB-only and constants-only controls
did not. First-use allocation passes the scene regressions without a GPU-idle wait.
The underlying allocation-order interaction in the initial scene GPU work remains
unexplained; this is not claimed as a general Vulkan backend fix. See the validation
record in `tests/README.md`.

### NEE sample-count shader specialization (2026-09-15)

`RTXPTRayTracingPass` supplies upstream's `RTXPT_NEE_FULL_SAMPLE_COUNT` and
`RTXPT_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT` for each path-tracing variant. NEE loops
and the outgoing BSDF MIS metadata consume the same compiled counts. The explicit
`[loop]` annotations are removed so DXC can optimize the default 1 full / 5 candidate
case. Shader consumers without these macros retain the runtime fallback.

Before dispatch, the sample compares the clamped UI counts with the pipeline's
compiled counts and rebuilds on changes. The macros participate in render-state
cache keys, including when restoring a previously used setting. Local/global
proposal allocation still follows the live sampling ratio and coherence state.
Cache version **012** invalidates the prior shader implementation. The opt-in scene
regression also exercises 0 full samples, 1 candidate / 2 full samples, 8 / 2,
Reference 1 / 1, and restoration of the realtime 5 / 1 defaults.

### Native FP16 BSDF path (2026-09-15)

`RTXPTRayTracingPass` defaults to upstream's
`RTXPT_LP_TYPES_USE_16BIT_PRECISION=1` and DXC `-enable-16bit-types` on capable
devices. `--fp16 false` selects the FP32 permutation. Diligent's opt-in
`SHADER_COMPILE_FLAG_ENABLE_16BIT_TYPES` forwards the argument for both DXIL and
SPIR-V; other samples and non-BSDF compute passes retain their existing settings.

`PathTracerHelpers.hlsli` mirrors `Utils.hlsli`'s native float/integer aliases and
FP32 fallback. `StandardBSDFData` in the port's `BxDF.hlsli` uses upstream's low
precision material fields and getters; `ActiveBSDF::estimateSpecDiffBSDF` mirrors
the low precision albedo arithmetic in upstream `StandardBSDF.hlsli`. The stable
plane roughness view uses the same precision. Lobe evaluation, Fresnel/GGX, world
directions, and PDFs retain upstream's FP32 computation types. This does not
replace the port's BSDF interfaces or change sampling algorithms.

The sample requests optional `ShaderFloat16` and `ResourceBuffer16BitAccess` and
requires both for this permutation. The latter identifies native 16-bit support
on D3D12 (minimum precision alone is insufficient) and enables Vulkan shaderInt16
used by `lpuint`. Unsupported devices fall back to FP32 with a startup log.
Cache **013** invalidates the previous shader implementation. See `tests/README.md`
for the compiler test, scene regressions and measured performance.

## Phase R4 EnvMapBaker Mapping

| RTXPT-fork | Diligent RTXPT | Notes |
|---|---|---|
| `Lighting/Distant/EnvMapBaker.*` | `src/RTXPTEnvMapBaker.*` | Diligent-native resource owner; uses DiligentFX cubemap/BRDF precompute and local compute importance-map passes |
| `Lighting/Distant/EnvMapImportanceSamplingBaker.*` | `src/RTXPTEnvMapBaker.*` + `Lighting/EnvMapImportanceBaker.hlsl` | Builds R32 importance and RGBA16F radiance mip chains for MIP descent |
| `SampleProceduralSky` | procedural source path in `RTXPTEnvMapBaker` | Current port bakes the existing procedural gradient into the same env-map path; source sentinel is `==PROCEDURAL_SKY==` |
| `PathTracer/Lighting/EnvMap.hlsli` | `PathTracer/Lighting/EnvMap.hlsli` | Runtime `EnvMapSampler`, MIP-descent sampling, and env pdf evaluation |
| global env bindings `t_EnvironmentMap`, `t_EnvironmentImportanceMap`, `t_EnvironmentRadianceMap` | RT static resources in `RTXPTRayTracingPass` | Bound for raygen and miss with stable fallback views; BRDF LUT is generated by the baker and remains available for the later composite path |

## Phase R6 Transmission and Nested Dielectrics Mapping

| Diligent port | RTXPT-fork source | R6 notes |
|---|---|---|
| `Rendering/Materials/BxDF.hlsli` transmission helpers | `Rendering/Materials/BxDF.hlsli::SpecularReflectionTransmissionMicrofacet` | Rough dielectric reflection/transmission with Fresnel lobe selection and refraction Jacobian |
| `Rendering/Materials/LobeType.hlsli` | `Rendering/Materials/LobeType.hlsli` | Reflection/transmission lobe bit layout kept compatible with RTXPT-fork |
| `Rendering/Materials/InteriorList.hlsli` | `Rendering/Materials/InteriorList.hlsli` | Two-slot priority stack; raygen-local in Diligent instead of packed into RTXPT `PathState` |
| `PathTracerNestedDielectrics.hlsli` | `PathTracerNestedDielectrics.hlsli` | False-hit rejection, outside-IoR computation, and stack updates after transmission scatter |
| `Rendering/Volumes/HomogeneousVolumeSampler.hlsli` | `Rendering/Volumes/HomogeneousVolumeSampler.hlsli` | Absorption-only Beer-Lambert transmittance; no scattering in R6 |
| `MaterialPTData` R6 fields | `Materials/MaterialPT.h::PTMaterialData` | Diligent layout differs but carries transmission, IoR, nested priority, and attenuation data |
| `PathTracerClosestHit.rchit` two-sided payload | `PathTracerBridgeDonut.hlsli::loadSurface` | Diligent keeps closest-hit payload return style instead of RTXPT-fork `SurfaceData` |
| `PathTracerSample.rgen` interior-list loop | `PathTracer.hlsli::HandleHit` | Diligent raygen loop owns path state; rejected hits do not consume bounce count |
| `PathTracerAnyHit.rahit` primary / visibility entries | `PathTracerBridgeDonut.hlsli::AlphaTestImpl / AlphaTestVisibilityRay` | Upstream texture-alpha filtering; the former port-only stochastic BLEND filter has been removed (see inline visibility below) |

### Inline visibility and upstream alpha semantics (2026-09-14)

`PathTracerVisibility.hlsli::Bridge::traceVisibilityRay` translates upstream
`PathTracerBridgeDonut.hlsli::traceVisibilityRay`: accept-first-hit inline
RayQuery, mask 0xFF, and explicit non-opaque triangle candidate filtering.
Sub-instances use `CandidateInstanceID() + CandidateGeometryIndex()`; triangle
UV fetches retain the port's indexed/non-indexed and static/skinned layouts.
Vulkan texel-buffer format annotations match the actual 16/32-bit index view.
Opacity micromaps and SER are not introduced by this change.

`AlphaTestImpl` samples base texture alpha at mip 0 without multiplying material
opacity; CPU upload quantizes cutoff to UNORM8, matching upstream
`MaterialsBaker::UpdateSubInstanceData` / `SubInstanceData::AlphaCutoff`.
Loaded material extensions override glTF MASK classification. No enabled/loaded
base texture means no alpha test. BLEND alone does not request non-opaque BLAS
geometry or stochastic acceptance; glass still uses the existing transmission BSDF.
`ExcludeFromNEE` is loaded from `.material.json`, stored in MaterialPTData flags
(the port keeps its own buffer layout), and makes geometry non-opaque so shadow
candidates can be rejected. Primary rays still accept these surfaces subject to
their alpha test. It does not remove emitters from light sampling.

The default RT pipeline uses inline visibility when the adapter advertises
`RAY_TRACING_CAP_FLAG_INLINE_RAY_TRACING`; unsupported adapters and
`--inline_shadows false` use TraceRay. The fallback has a separate visibility
any-hit entry sharing the exact inline predicate. Primary and visibility entry
points are compiled separately for Diligent's single-entry shader reflection.
Inline pipelines have recursion depth 1 and omit visibility SBT records/miss;
the existing primary-ray/TLAS hit-group stride of 2 remains intact. Fallback
Reference/FILL pipelines retain recursion depth 2. Cache version is now 005.

## Phase R7 - Shadow/AA Polish

| Diligent port | RTXPT-fork source | R7 notes |
|---|---|---|
| `PathTracerHelpers.hlsli::ComputeRayOrigin` | `PathTracer/PathTracerHelpers.hlsli::ComputeRayOrigin` | Same robust offset algorithm, expressed without RTXPT-fork's `select()` helper |
| `PathTracerHelpers.hlsli::ComputeLowGrazingAngleFalloff` | `PathTracer/PathTracerHelpers.hlsli::ComputeLowGrazingAngleFalloff` | Same direct-light shadow terminator fadeout formula |
| `PathTracer::MakeVisibilityOrigin` | `PathTracer/PathTracerNEE.hlsli::ComputeVisibilityRay` | Diligent keeps visibility tracing in raygen helpers; face-normal side is still selected by the shading normal |
| `PathTracerClosestHit.rchit::LoadCurrentSurfaceData` vertex normal | `Scene/ShadingData.hlsli::vertexN` | Shared local surface construction carries the corrected pre-normal-map vertex normal because Diligent does not materialize upstream `ShadingData` |
| `MaterialPTData.shadowNoLFadeout` | `Materials/MaterialPT.h::ShadowNoLFadeout` | Stored in existing Diligent padding at offset 136; material record stays 144 bytes |
| `PathTracerCameraData` + `ComputeRayThinlens` | `PathTracerShared.h::PathTracerCameraData` + `PathTracerHelpers.hlsli::ComputeRayThinlens` | Same camera basis and thin-lens math; Diligent stores the camera block at top-level `SampleConstants.camera` |
| `RTXPTSample` UI labels `Aperture` / `Focal Distance` | `SampleUI.cpp` camera section | Same labels, defaults, and clamp ranges; aperture 0 is the default pinhole path |

## Phase 6 Post-Processing Pipeline Mapping

Phase 6 ports the RTXPT-fork post-processing display contract. This section is the ownership contract for P1-P8 implementation tasks; it does not imply that the listed Diligent files already exist.

### Phase 6 Source-to-Destination Map

| RTXPT-fork source | Diligent RTXPT destination | Phase | Contract |
|---|---|---:|---|
| `SampleCommon/RenderTargets.h` | `src/RTXPTRenderTargets.hpp` | P1 | Resource names, accessors, and ownership comments for `OutputColor`, `AccumulatedRadiance`, `ProcessedOutputColor`, `LdrColor`, `LdrColorScratch`, and later temporal/denoiser targets. |
| `SampleCommon/RenderTargets.cpp` | `src/RTXPTRenderTargets.cpp` | P1 | Diligent texture creation and fallback logic. `OutputColor` is HDR radiance, `AccumulatedRadiance` is `RGBA32F`, `ProcessedOutputColor` is HDR post-AA/post-accumulation, and `LdrColor`/`LdrColorScratch` are display-format ping-pong targets. |
| `SampleCommon/RenderTargets.h` `RenderSize` / `DisplaySize` | `src/RTXPTRenderTargets.{hpp,cpp}` `RTXPTRenderTargetDimensions` | P6 | Splits render-size raw/guide resources from display-size `ProcessedOutputColor`, `LdrColor`, and presentation. |
| `SampleCommon/RenderTargets.h` `TemporalFeedback1`, `TemporalFeedback2` | `src/RTXPTRenderTargets.{hpp,cpp}` | P6 | Display-size temporal feedback resources reserved for P6/P7 temporal contracts. |
| `SampleCommon/RenderTargets.h` `ScreenMotionVectors`, `Depth` | `src/RTXPTRenderTargets.{hpp,cpp}`, `shaders/PathTracer/PathTracerSample.rgen` | P6 | Render-size depth and screen motion-vector resources for future temporal contracts; Reference PathTracer writes primary depth and zero motion vectors but does not feed them to SR. |
| `SampleCommon/RenderTargets.h` `CombinedHistoryClampRelax` | `src/RTXPTRenderTargets.{hpp,cpp}` | P6 | Display-size history-relax resource reserved for temporal AA/denoiser handoff. |
| `Sample.cpp::CreateRenderPasses` | `src/RTXPTPostProcessPipeline.{hpp,cpp}` plus `src/RTXPTSample.cpp` | P1 | Diligent-native pass construction and readiness. `RTXPTSample` owns lifetime and frame orchestration; the pipeline owns post-process pass objects. |
| `Sample.cpp::PostProcessAA` | `src/RTXPTAccumulationPass.{hpp,cpp}` and later TAA/DLSS-specific classes | P2, P6, P8 | Reference mode runs accumulation from `OutputColor` into `AccumulatedRadiance` and `ProcessedOutputColor`. TAA/DLSS/DLSS-RR are later gated modes. |
| `Sample.cpp::PostProcessAA` `RealtimeAA == 0` | `src/RTXPTSample.cpp::PresentRealtimeFinalOutput` + `src/RTXPTPostProcessPipeline.cpp::CopyRealtimeOutputToProcessed` | Realtime G10 | Copies/passes merged realtime `OutputColor` into HDR `ProcessedOutputColor` before bloom/tone mapping. |
| `Sample.cpp::PostProcessAA` `RealtimeAA == 1` | `src/RTXPTTemporalAAPass.{hpp,cpp}` + `src/RTXPTPostProcessPipeline.cpp::RunTemporalAA` | Realtime G10 | Uses DiligentFX `TemporalAntiAliasing` with RTXPT depth, previous depth, motion vectors, current/previous camera state, and reset-history flags. |
| `Sample.cpp::PostProcessAA` `RealtimeAA == 2` | `src/RTXPTSuperResolutionPass.{hpp,cpp}` + `src/RTXPTPostProcessPipeline.cpp::RunRealtimeSuperResolution` | Realtime G10 | Uses Diligent `ISuperResolution` from merged realtime `OutputColor` to display-size `ProcessedOutputColor`. |
| `Sample.cpp::PostProcessAA` `RealtimeAA == 3` / `EvaluateDLSSRR` | `RTXPTPostProcessPass::RunDLSSRRDenoiserPrepareInputs`, then future `IRayReconstruction` integration | RR stage 1 | Input preparation is implemented below; resource ownership and frame scheduling remain #19/#20. The port uses native NGX through Diligent, without Streamline. |
| RTXPT-fork DLSS/TAA scheduling hooks | `src/RTXPTSuperResolutionPass.{hpp,cpp}`, `src/RTXPTPostProcessPipeline.{hpp,cpp}` | P6 | Reserves Diligent `ISuperResolution` for future non-reference temporal upscaling; Reference PathTracer does not schedule SR/TAA/DLSS/AA. |
| `ProcessingPasses/AccumulationPass.h` | `src/RTXPTAccumulationPass.hpp` | P2 | Public Diligent accumulation pass interface, status, and resource binding contract. |
| `ProcessingPasses/AccumulationPass.cpp` | `src/RTXPTAccumulationPass.cpp` | P2 | Diligent compute PSO/SRB creation and dispatch. No Donut/NVRHI API copy. |
| `ProcessingPasses/AccumulationPass.hlsl` | `shaders/PostProcessing/RTXPTAccumulation.csh` | P2 | Blend raw `OutputColor` into `AccumulatedRadiance`; write HDR `ProcessedOutputColor`. Preserve blend-factor semantics and render/display-size resampling hook. |
| `ToneMapper/ToneMappingPasses.h` | `src/RTXPTToneMappingPass.hpp` and `src/RTXPTSample.hpp` | P3 | Tone-mapping parameter model: operator, enable flag, exposure mode, exposure compensation/value/range, auto exposure, white balance, white luminance/scale, clamp. |
| `ToneMapper/ToneMappingPasses.cpp` | `src/RTXPTToneMappingPass.cpp` | P3 | Diligent tone-map pass setup, luminance resources, constants upload, pass-through when disabled, and frame advance. |
| `ToneMapper/ToneMapping_cb.h` | `shaders/PostProcessing/ToneMapper/ToneMappingShared.h` and matching C++ struct in `src/RTXPTToneMappingPass.hpp` | P3 | CPU/GPU constants layout. Add `static_assert` checks when the C++ side is introduced. |
| `ToneMapper/ToneMapping.hlsl` | `shaders/PostProcessing/ToneMapper/ToneMapping.hlsl` | P3 | Diligent shader entry points for tone mapping and optional luminance capture. |
| `ToneMapper/ToneMapping.ps.hlsli` | `shaders/PostProcessing/ToneMapper/ToneMapping.ps.hlsli` | P3 | Tone-map operators: `Linear`, `Reinhard`, `ReinhardModified`, `HejiHableAlu`, `HableUc2`, `Aces`. |
| `ToneMapper/luminance_ps.hlsl` | `shaders/PostProcessing/ToneMapper/Luminance.psh` | P3 | Auto-exposure luminance prepass. CPU readback is optional and must be gated if implemented. |
| `External/Donut/include/donut/render/BloomPass.h` | `src/RTXPTBloomPass.hpp` | P4 | Diligent bloom pass interface, settings, stats, and temporary-resource ownership. |
| `External/Donut/src/render/BloomPass.cpp` | `src/RTXPTBloomPass.cpp` | P4 | Two half-resolution downscales via `RTXPTBloomCopy.psh`, quarter-resolution horizontal/vertical blur, and blend-constant apply into `ProcessedOutputColor`. |
| `External/Donut/include/donut/shaders/bloom_cb.h` | `shaders/PostProcessing/RTXPTBloomShared.h` | P4 | `RTXPTBloomConstants` layout matching Donut `BloomConstants`. |
| `External/Donut/shaders/passes/bloom_ps.hlsl` | `shaders/PostProcessing/RTXPTBloomBlur.psh` | P4 | Gaussian blur shader formula. |
| `Shaders/TestRaygenPP.hlsl` | `shaders/PostProcessing/RTXPTPostProcess.csh` | P4 | HDR test circle and LDR Sobel edge-detection compute shader. |
| `ProcessingPasses/PostProcess.h` | `src/RTXPTPostProcessPass.hpp` | P7, P8 | P4 introduced the HDR test and LDR edge hooks; G7 extends `RTXPTPostProcessPassId` with stable-plane debug, RELAX/REBLUR prepare, RELAX/REBLUR final merge, and no-denoiser final merge modes. |
| `ProcessingPasses/PostProcess.cpp` | `src/RTXPTPostProcessPass.cpp` | P7, P8 | P4 introduced legacy post-process dispatch; G7 initializes PSO/SRB state per mode and dispatches stable-plane debug, prepare/final wrappers, and no-denoiser final merge through `pMergeOutputUAV` selected by `GetAccumulationOutputUAV()`. Full NRD orchestration remains G8+. |
| `ProcessingPasses/PostProcess.hlsl` | `shaders/PostProcessing/RTXPTPostProcess.csh` | P7, P8 | P4 shader started with HDR test and LDR edge paths; G7 adds `RTXPT_POST_PROCESS_MODE` branches for stable-plane debug, RELAX/REBLUR prepare, RELAX/REBLUR final merge, and no-denoiser final merge. NRD dependency-backed dispatch remains G8+. |
| `Shaders/Bindings/ShaderResourceBindings.hlsli` | `shaders/PostProcessing/RTXPTPostProcessBindings.hlsli` or local declarations in each post-process shader | P4-P8 | Resource naming reference: `t_LdrColorScratch`, `u_OutputColor`, `u_ProcessedOutputColor`, `u_PostTonemapOutputColor`. Diligent binding slots may differ, but names and ownership should remain recognizable. |
| `Sample.cpp::PostProcessPreToneMapping` | `src/RTXPTPostProcessPipeline.cpp`, `src/RTXPTBloomPass.cpp` | P4 | HDR post-process scheduling: bloom. |
| `Sample.cpp::PostProcessPostToneMapping` | `src/RTXPTPostProcessPipeline.cpp`, `src/RTXPTPostProcessPass.cpp` | P4 | LDR post-process scheduling after tone mapping, including `LdrColor` to `LdrColorScratch` copy. |
| `Sample.cpp` final `m_CommonPasses->BlitTexture` | `src/RTXPTBlitPass.{hpp,cpp}` plus `RTXPTRenderTargets::GetPresentationSRV()` | P5 | Final swapchain copy. `GetPresentationSRV()` returns `LdrColor`, and no debug/compute output may replace the normal swapchain source. |
| `SampleUI.h` / `SampleUI.cpp` tone-mapping and post-process controls | `src/RTXPTSample.{hpp,cpp}` | P3-P5 | Existing disabled controls become live as each pass lands. UI changes must request only the histories they invalidate. |
| `SampleUI.h::SampleUIData` realtime fields | `src/RTXPTRealtimeSettings.hpp`, `src/RTXPTSample.hpp` | Realtime G1 | Diligent-local realtime state mirrors `RealtimeMode`, `RealtimeSamplesPerPixel`, `RealtimeAA`, `StandaloneDenoiser`, stable-plane controls, firefly controls, NRD UI settings, and reset-request flags without taking a compile dependency on NRD. |
| `SampleUI.h::ActualUseStandaloneDenoiser` | `src/RTXPTRealtimeSettings.hpp::RTXPTRealtimeSettings::ActualUseStandaloneDenoiser` | Realtime G1 | Preserves RTXPT-fork semantics: true only when `RealtimeMode && RealtimeAA < 3 && StandaloneDenoiser`. |
| `SampleUI.cpp` realtime UI controls | `src/RTXPTSample.cpp::UpdateUI` | Realtime G1 | Mode, realtime setup, AA/SR/denoiser selection, stable-plane controls, and NRD controls are visible. G1 introduced the controls before execution; G4/G5 PathTrace orchestration is documented below. |
| `Sample.cpp::UpdatePathTracerConstants` realtime fields | `src/RTXPTSample.cpp::UpdateFrameConstants`, `src/RTXPTFrameConstants.hpp::PathTracerConstants` | Realtime G2 | Diligent now uploads render dimensions, `sampleBaseIndex`, `frameIndex`, `invSubSampleCount`, realtime jitter scale, texture LOD bias, pre-exposed gray luminance, denoiser flags, stable-plane controls, and generic tiled-storage strides through the shared constant buffer. Auto-exposure currently uploads neutral pre-exposed gray until a shared CPU/GPU exposure state is ported. |
| `Shaders/PathTracer/PathTracerShared.h::PathTracerConstants` | `shaders/PathTracer/PathTracerShared.h`, `src/RTXPTFrameConstants.hpp` | Realtime G2 | C++ and HLSL field order is synchronized with `static_assert` layout guards on the C++ side. Diligent reference compatibility fields are retained after the RTXPT-fork realtime fields. |
| `Sample.cpp` realtime `m_sampleIndex` semantics | `src/RTXPTSample.cpp::UpdateFrameConstants` | Realtime G2 | Reference mode keeps accumulation sample indexing. Realtime mode derives the active sample from `frameIndex % 8192` and uploads `sampleBaseIndex = realtimeSampleIndex * ActualSamplesPerPixel()`. |
| `Shaders/SampleConstantBuffer.h::view/previousView` | `src/RTXPTFrameConstants.hpp::PathTracerViewData`, `shaders/PathTracer/PathTracerShared.h::PathTracerViewData` | Realtime G2 | Current and previous view constants are available for future motion-vector, denoiser guide, and NRD common-settings ports. |
| `Shaders/PathTracer/PathTracer.hlsli::CommitPixel` | `shaders/PathTracer/PathTracer.hlsli` | P2 | Reference mode commits raw `PathState::GetL().rgb` to `u_OutputColor`; accumulation and tone mapping are not raygen responsibilities. |

## Realtime G3 Render Target Map

| RTXPT-fork source | Diligent owner | Notes |
|---|---|---|
| `SampleCommon/RenderTargets.h` `RenderTargets::StableRadiance` | `src/RTXPTRenderTargets.hpp` `m_StableRadiance` | Render-size `RGBA16_FLOAT`, SRV/UAV. |
| `SampleCommon/RenderTargets.h` `RenderTargets::StablePlanesHeader` | `src/RTXPTRenderTargets.hpp` `m_StablePlanesHeader` | Render-size 4-layer `R32_UINT` texture array, SRV/UAV. |
| `SampleCommon/RenderTargets.h` `RenderTargets::StablePlanesBuffer` | `src/RTXPTRenderTargets.hpp` `m_StablePlanesBuffer` | Structured buffer of `RTXPTStablePlaneData`, element count = generic TS plane stride * `kRTXPTStablePlaneCount`. |
| `SampleCommon/RenderTargets.h` `RenderTargets::Throughput` | `src/RTXPTRenderTargets.hpp` `m_Throughput` | Render-size `R32_UINT`, SRV/UAV. |
| `SampleCommon/RenderTargets.h` `RenderTargets::SpecularHitT` | `src/RTXPTRenderTargets.hpp` `m_SpecularHitT` | Render-size `R32_FLOAT`, SRV/UAV. |
| `SampleCommon/RenderTargets.h` `RenderTargets::ScratchFloat1` | `src/RTXPTRenderTargets.hpp` `m_ScratchFloat1` | Render-size `R32_FLOAT`, SRV/UAV. |
| `SampleCommon/RenderTargets.h` denoiser input textures | `src/RTXPTRenderTargets.hpp` `m_Denoiser*` input members | Render-size NRD input resources with names preserved for G7-G9 bindings. |
| `SampleCommon/RenderTargets.h` `DenoiserOutDiffRadianceHitDist[cStablePlaneCount]` | `src/RTXPTRenderTargets.hpp` `m_DenoiserOutDiffRadianceHitDist` | Three per-plane output textures. |
| `SampleCommon/RenderTargets.h` `DenoiserOutSpecRadianceHitDist[cStablePlaneCount]` | `src/RTXPTRenderTargets.hpp` `m_DenoiserOutSpecRadianceHitDist` | Three per-plane output textures. |
| `SampleCommon/RenderTargets.h` `DenoiserOutValidation` | `src/RTXPTRenderTargets.hpp` `m_DenoiserOutValidation` | Optional validation texture, disabled by default. |
| `SampleCommon/RenderTargets.h` `DenoiserAvgLayerRadianceHalfRes` | `src/RTXPTRenderTargets.hpp` `m_DenoiserAvgLayerRadianceHalfRes` | Half render-size `RGBA16_FLOAT`, SRV/UAV. |
| `Shaders/PathTracer/StablePlanes.hlsli` `struct StablePlane` | `src/RTXPTRenderTargets.hpp` `RTXPTStablePlaneData` | CPU mirror guarded at 80 bytes with important offsets checked. |

## Realtime G4-G5 PathTrace Variants and Orchestration

| RTXPT-fork source | Diligent owner | Status |
|---|---|---|
| `AdvancedSample.cpp::CreateRTPipelines` REF/BUILD/FILL variants | `src/RTXPTRayTracingPass.{hpp,cpp}` `RTXPTPathTraceVariant` | Diligent RT PSO/SRB/SBT variants with `PATH_TRACER_MODE` macros. |
| `Sample.cpp::PathTrace` BUILD pre-pass | `src/RTXPTSample.cpp::DispatchPathTracePrePass` | Realtime-only dispatch to `BuildStablePlanes`, then UAV barriers. |
| `Sample.cpp::PathTrace` `LightsBaker.UpdateEnd(... Depth, MotionVectors)` | `src/RTXPTLightsBaker::UpdateEnd(... pDepthSRV, pMotionVectorsSRV)` | Call-order and data contract preserved; current Diligent feedback implementation accepts but does not yet consume the views. |
| `Sample.cpp::PathTrace` FILL/REFERENCE sub-sample loop | `src/RTXPTSample.cpp::DispatchPathTraceLoop` | Uses `SampleMiniConstants.params.x` for sub-sample index. |
| `Shaders/PathTracer/PathPayload.hlsli` | `shaders/PathTracer/PathPayload.hlsli` | Upstream-compatible five-`uint4` packed path-state payload for reference and realtime variants. |
| `Shaders/PathTracer/PathState.hlsli` | `shaders/PathTracer/PathState.hlsli` | Stable-plane flags/counters and path state. |
| `Shaders/PathTracer/StablePlanes.hlsli` | `shaders/PathTracer/StablePlanes.hlsli` | Stable-plane buffer/header/radiance logic. |
| `Shaders/PathTracer/PathTracerStablePlanes.hlsli` | `shaders/PathTracer/PathTracerStablePlanes.hlsli` | Build/fill stable-plane hit/miss/scatter logic. |
| `Sample.cpp::PathTrace` RTXDI final hooks | status UI only | Disabled in G4/G5; not silently treated as implemented. |
| `Sample.cpp::Denoise` | G8/G9 plans | Excluded from this plan. |

### Reference Unified PathState Spine

Local reference mode is intentionally routed through `PathState`,
`PathPayload`, `PathTracer::HandleHit`, and `PathTracer::HandleMiss`.

Remaining intentional fork differences:

- `PathTracerBridge.hlsli` uses Diligent resources and scene adapters instead
  of upstream Donut bridge calls.
- Reference `CommitPixel` writes raw HDR output; depth, motion vectors, and
  throughput are exported through the Diligent bridge.
- Reference branches inside `HandleHit`, `HandleMiss`, `GenerateScatterRay`,
  `HandleRussianRoulette`, and `HandleNEE` preserve the local reference
  estimator: full-sample NEE, emissive/environment MIS, firefly filtering,
  camera jitter, aligned diffuse-bounce classification, `minBounceCount`
  Russian roulette, volumes, and nested dielectrics.
- Reference raygen keeps a safety iteration ceiling in addition to normal path
  termination.
- `PathState`, `PathPayload`, stable-plane exploration storage, and the RT PSO
  use the upstream 80-byte payload layout in all path-tracer modes.

Parity notes:

- Reference, build, and fill variants share the upstream packed `PathPayload` /
  `PathState` layout and fp16 transport fields.
- `PathTracer::GenerateScatterRay` follows the upstream split-overload shape and
  keeps the upstream non-delta ray-cone spread expansion plus thread-reorder flag
  side effects.
- Stable-plane material and BSDF handling is a Diligent-native shim/translation
  toward RTXPT-fork naming and data flow. It should not be read as full NRD,
  RTXDI final-shading, or final merge parity.
- G4/G5 ports realtime PathTrace orchestration through `BuildStablePlanes` and
  `FillStablePlanes` dispatch. G7 adds the post-process prepare/final-merge
  mapping documented below; full NRD denoise orchestration remains in later
  phases.

### Phase 6 Resource Contract

| Resource | Diligent owner | Format target | Size | Producer | Consumer | Notes |
|---|---|---|---|---|---|---|
| `OutputColor` | `RTXPTRenderTargets` | Prefer `TEX_FORMAT_RGBA16_FLOAT`; allow a documented closest supported HDR UAV fallback | render size | Raygen reference path tracer in P2; realtime path-trace variants before post-process merge | Accumulation, TAA/DLSS/DLSS-RR, HDR post-process | Raw HDR radiance only. It is not display-ready and must not contain ACES output. G7 final/no-denoiser merge does not write this target. |
| `AccumulatedRadiance` | `RTXPTRenderTargets` | `TEX_FORMAT_RGBA32_FLOAT` | render size | `RTXPTAccumulationPass` | `RTXPTAccumulationPass` | Reference-mode accumulation history. Unsupported UAV format disables accumulation with a visible reason. |
| `ProcessedOutputColor` | `RTXPTRenderTargets` | Same HDR format family as `OutputColor` | display size after P6, render size until render/display split exists | Accumulation, TAA, DLSS, DLSS-RR, or G7 final/no-denoiser merge selected by `GetAccumulationOutputUAV()` | HDR post-process and tone mapping | This is the HDR image that tone mapping reads. `GetAccumulationOutputUAV()` resolves here when SR is inactive; with SR active, G7 `pMergeOutputUAV` targets the SR input path before SR produces `ProcessedOutputColor`. |
| `LdrColor` | `RTXPTRenderTargets` | `TEX_FORMAT_RGBA8_UNORM` or Diligent-supported sRGB equivalent when needed | display size | `RTXPTToneMappingPass` | LDR post-process, final blit | Normal final display source. |
| `LdrColorScratch` | `RTXPTRenderTargets` | Match `LdrColor` | display size | Copy/ping-pong before LDR effects | LDR post-process | Copy/ping-pong scratch for LDR post-process effects. |
| `ComputeColor` | `RTXPTRenderTargets` | Existing debug format | swapchain size | Legacy `RTXPTComputePass` only when explicitly reintroduced outside P5 | No normal presentation consumer | Diagnostic scratch only; P5 does not request or present it. |

### Phase 6 Behavioral Contracts

- `PathTracer::CommitPixel` writes one raw HDR reference sample to `u_OutputColor`.
- `PathTracerSample.rgen` must not write `u_AccumulationBuffer` after P2.
- `PathTracerSample.rgen` must not call `ToneMapACES` after P3.
- `PathTracerConstants::exposureScale` remains a temporary bridge until P3, then tone-mapping exposure data moves into tone-map pass state.
- Disabling tone mapping is a post-process pass-through from `ProcessedOutputColor` to `LdrColor`; it is not a raygen macro or branch.
- Tone mapping and auto exposure must not feed back into `AccumulatedRadiance`.
- Bloom, LDR edge/test effects, TAA, NRD, DLSS, and DLSS-RR are optional consumers/producers around the base `OutputColor -> ProcessedOutputColor -> LdrColor` chain.
- Final presentation reads only `LdrColor` through `RTXPTRenderTargets::GetPresentationSRV()`.
- P1-P5 must stay backend-neutral for D3D12 and Vulkan. P8 integrations may be D3D12/NVIDIA-only, but must compile out cleanly elsewhere.

## Realtime G6 Denoising Guides Baker

| RTXPT-fork source | Diligent port | Notes |
|---|---|---|
| `ProcessingPasses/DenoisingGuidesBaker.h` | `src/RTXPTDenoisingGuidesBaker.hpp` | Diligent pass owner for guide compute dispatch and stats. |
| `ProcessingPasses/DenoisingGuidesBaker.cpp::DenoiseSpecHitT` | `src/RTXPTDenoisingGuidesBaker.cpp::Bake` | Preserves ping then pong dispatch ordering using `SpecularHitT` and `ScratchFloat1`. |
| `ProcessingPasses/DenoisingGuidesBaker.cpp::ComputeAvgLayerRadiance` | `src/RTXPTDenoisingGuidesBaker.cpp::Bake` | Dispatches at half render resolution and writes `DenoiserAvgLayerRadianceHalfRes`. |
| `ProcessingPasses/DenoisingGuidesBaker.cpp::RenderDebugViz` | `src/RTXPTDenoisingGuidesBaker.cpp::Bake` + `src/RTXPTSample.cpp::PresentRealtimeGuideDebug` | Diligent debug visualization writes to `ProcessedOutputColor` for optional presentation because no RTXPT-fork `ShaderDebug` texture owner is ported. |
| `ProcessingPasses/DenoisingGuidesBaker.hlsl` | `shaders/PathTracer/DenoisingGuidesBaker.hlsl` | Shader algorithms use Diligent resource names and existing `StablePlanesContext`. |
| `Sample.cpp::PathTrace` guide bake call point | `src/RTXPTSample.cpp::PathTrace` | Runs after FILL stable-plane loop and before later final merge/NRD work. |

G6 intentionally does not port `ShaderDebug`, `StablePlanesDebugViz`, NRD prepare/final merge, or no-denoiser final merge. G7 adds stable-plane debug plus prepare/final-merge mapping below while full NRD integration remains owned by later realtime denoise phases.

## Realtime G7 PostProcess Denoiser Prepare and Final Merge

| RTXPT-fork source | Diligent port | Notes |
|---|---|---|
| `ProcessingPasses/PostProcess.h::ComputePassType` | `src/RTXPTPostProcessPass.hpp::RTXPTPostProcessPassId` | Diligent enum mirrors stable-plane debug, RELAX/REBLUR prepare, RELAX/REBLUR final merge, and no-denoiser final merge. |
| `ProcessingPasses/PostProcess.cpp::PostProcess` | `src/RTXPTPostProcessPass.cpp::Initialize` | Creates one Diligent compute PSO/SRB per G7 mode using `RTXPT_POST_PROCESS_MODE`. |
| `ProcessingPasses/PostProcess.cpp::Apply` | `src/RTXPTPostProcessPass.cpp::DispatchPass` | Uses the static frame constants SRB binding, updates `SampleMiniConstants`, binds dynamic stable-plane and denoiser resources, validation SRV, and merge work `pMergeOutputUAV` selected by `GetAccumulationOutputUAV()`. |
| `ProcessingPasses/PostProcess.hlsl::DENOISER_PREPARE_INPUTS` | `shaders/PostProcessing/RTXPTPostProcess.csh` prepare modes | Writes NRD input resources and initializes merge work output from `StableRadiance` on the first processed plane. |
| `ProcessingPasses/PostProcess.hlsl::DENOISER_FINAL_MERGE` | `shaders/PostProcessing/RTXPTPostProcess.csh` final merge modes | Reads per-plane NRD outputs, remodulates with stable-plane BSDF estimates, and adds radiance into `pMergeOutputUAV` selected by `GetAccumulationOutputUAV()`. |
| `ProcessingPasses/PostProcess.hlsl::NO_DENOISER_FINAL_MERGE` | `shaders/PostProcessing/RTXPTPostProcess.csh` no-denoiser mode | Combines stable radiance plus noisy stable-plane radiance into the same downstream `pMergeOutputUAV` target selected by `GetAccumulationOutputUAV()`. |
| `ProcessingPasses/PostProcess.hlsl::STABLE_PLANES_DEBUG_VIZ` | `shaders/PostProcessing/RTXPTPostProcess.csh` stable-plane debug mode | Provides lightweight stable-plane debug output without porting RTXPT-fork `ShaderDebug`. |
| `NRD/DenoiserNRD.hlsli` | `shaders/PostProcessing/RTXPTDenoiserNRD.hlsli` | Wrapper compiles before G8 and can forward to NRD headers after the NRD dependency gate exists. |

## DLSS-RR Stage 1: Input Preparation (#18)

`RTXPTPostProcessPass::RunDLSSRRDenoiserPrepareInputs` dispatches mode 9 of
`RTXPTPostProcess.csh`. The caller supplies four distinct output UAVs through
`RTXPTDenoiserPostProcessAttribs`; #19 adds resource ownership plus depth/motion outputs, and frame
scheduling / `IRayReconstruction` execution belongs to #20. The existing RR UI
gate remains until that integration is ready. No NRD result is read by this pass.

### Reference correspondence

| RTXPT-fork source | Diligent producer / implementation | Meaning |
|---|---|---|
| `PathTracerStablePlanes.hlsli` stable-plane surface export | `PathTracerStablePlanes.hlsli::StablePlanesHandleHit` surface export, `PathTracerTypes.hlsli::ActiveBSDF::estimateSpecDiffBSDF` | Diffuse reflection plus diffuse transmission; specular GGX directional integral plus specular transmission. These are material BSDF estimates, not base-color texture values alone. |
| `StablePlanes.hlsli::StoreStablePlane` | Same local symbol | BSDF estimates retain the reference 0.04 reflectance floor and fp16 packing, without multiplying throughput or exposure. Normal is oct-encoded after the path image transform, in virtual world space for replaced/reflected surfaces. Roughness is the linear material roughness, with the existing blocked-motion-surface heuristic. |
| `StablePlanes.hlsli::CommitDenoiserRadiance` | Same local symbol | FILL noisy radiance already includes path throughput and the `invSubSampleCount` sample average from `Bridge::getNoisyRadianceAttenuation`. Do not multiply either again. BUILD supplies stable sky/emissive radiance. |
| `ProcessingPasses/PostProcess.hlsl` `DENOISER_DLSS_RR`, lines 205–370 | `RTXPTPostProcess.csh::DLSSRRDenoiserPrepareInputs` | Port of the enabled defaults: `MIX_BY_THROUGHPUT`, `MIX_STABLE_RADIANCE`, `ALLOW_MIX_NORMALS`. No experimental layer-radiance weighting or specular-MV synthesis is enabled. |
| `Sample.cpp::UpdatePathTracerConstants` | `RTXPTSample.cpp::UpdateFrameConstants`, `Bridge::computeCameraRay` | RR micro-jitter already drives `perPixelJitterAAScale`; the random offset is added to the temporal camera jitter in ray generation. No extra jitter is applied by prepare. |

Guide weights use 0.2 throughput weight, 0.01 equalization per available plane,
and a 0.05 dominant-plane bias, then normalize. Secondary throughput subtracts
from the primary residual. Sky branches participate in availability/weights,
as upstream does, but contribute no surface guides or noisy radiance. Color sums
stable radiance and every valid surface plane's noisy radiance without guide
weights. Diffuse guidance additionally mixes `sqrt(ReinhardMax(stableRadiance))`
toward 0.5 gray by 20%, using the reference stable-albedo blending factor.

Explicit robustness differences from the reference:

- Primary availability is checked, active count is capped at three, and an
  out-of-range dominant index is ignored. An empty weight sum is safe.
- Negative/NaN transport values become zero; positive infinity becomes 65504
  before arithmetic. Guide albedos are limited to [0,1] and roughness is linear
  [0,1]. This intentionally clips extreme BSDF estimates exceeding one.
- Mixed normals are renormalized; tiny or invalid vectors use world +Z. When
  diffuse plus specular guidance is nearly black, diffuse receives the upstream
  0.05 floor. All pixels, including the background, write every output.
- A nonpositive/nonfinite brightness clamp disables the user clamp instead of
  clearing color to black. The final max-channel clamp still guarantees fp16
  representability and preserves the color ratio of the finite combined sum.

### Radiance and material-guide output contract

All four outputs are full 2D, single-mip, single-sample `RGBA16_FLOAT` textures
at **render/input resolution**, not display resolution. All are written by this
compute pass after BUILD/FILL. The dispatch wrapper rejects missing outputs,
wrong dimensions/formats, output aliasing, and aliasing with StableRadiance.

| UAV | Space / unit / encoding | RR consumer |
|---|---|---|
| `u_OutputColor` (`pMergeOutputUAV`) | Scene-linear HDR RGB radiance, sample averaged, not pre-exposed or demodulated; alpha 1 | `pColorSRV` |
| `u_RRDiffuseAlbedo` | Linear dimensionless RGB [0,1], including stable sky/emission guidance; alpha 1 | `pDiffuseAlbedoSRV` |
| `u_RRSpecAlbedo` | Linear dimensionless RGB [0,1]; alpha 1 | `pSpecularAlbedoSRV` |
| `u_RRNormalsAndRoughness` | RGB unit world-space normal [-1,1], including stable-plane virtual image transform; W linear roughness [0,1], not GGX alpha | `pNormalsSRV`; set `RoughnessPacked = True`, `pRoughnessSRV = nullptr` |

Radiance is **not pre-exposed** in this port. `preExposedGrayLuminance` affects
firefly/clamp thresholds, not the units of stored radiance. The CPU supplies
`DLSSRRBrightnessClampK = UIClamp * preExposedGrayLuminance` when positive; the
prepare shader treats that as a threshold in the stored radiance domain. Set
`PreExposure = 1`, `ExposureScale = 1` at execution. Do not apply tone-map
exposure or its inverse during preparation. `pExposureSRV = nullptr` until an
explicit exposure-texture producer is integrated; NGX AutoExposure selection
remains the consumer's responsibility.

Optional `pDiffuseHitDistanceSRV`, `pSpecularHitDistanceSRV` and
`pSpecularMotionVectorsSRV` must explicitly be `nullptr` in fresh per-frame
execute attributes. Existing `SpecularHitT` is the NRD dominant-plane heuristic;
it is not exported here as a validated RR guide. No zero-filled placeholder
texture or NRD-packed guide should be passed as a substitute.

The #19 wrapper preserves BUILD/FILL UAV ordering before prepare and transitions
all six outputs to SRV. RR uses a separate display-size output. This stage does
not change NRD/SR frame scheduling or their resource encodings.

Validation: Debug RTXPT build; DXC DXIL and SPIR-V compilation of mode 9;
byte-identical DXIL for existing modes 3–8 (without NRD headers); ten production
shader GPU readback cases on D3D12 / RTX 5060 (see `tests/README.md`). Full scene
and camera-motion validation, Vulkan runtime validation and C++ wrapper dispatch
integration remain outstanding until #19/#20 supply resources and scheduling.

## DLSS-RR Stage 2: Resources and Frame Contract (#19)

`RTXPTRayReconstructionInputs` adapts successful native factory source queries
to `RayReconstructionDesc`, `RTXPTRenderTargetCreateInfo`, and fresh
`ExecuteRayReconstructionAttribs`. It does not own a factory or execute RR;
the #20 scheduler must supply each frame's query result, jitter and reset signal.
Never substitute a hardcoded Quality scale: a 640x360 query on the tested device
returned **427x240**, not a truncated 426x240.

`CreateRayReconstructionResources` requires realtime resources and excludes
reference accumulation and NRD validation. It allocates the RR guides below and
the shared stable-plane resources, without allocating NRD-only textures. Changing
the flag or dimensions rebuilds the requested resource set; unchanged requests
reuse it. `Reset()` and mode changes release the owner's references. The prepare
SRB clears dynamic bindings after dispatch so it does not retain obsolete guides.
GPU command ownership still governs destruction of in-flight allocations.

| RR binding | Owned/reused texture | Format and extent | Contract |
|---|---|---|---|
| `pColorSRV` | Reuse `OutputColor` | RGBA16F, input | Noisy scene-linear radiance from prepare, not NRD output or accumulated color |
| `pDepthSRV` | `RRDepth` | R32F, input | Positive linear view-space Z; sky/invalid depth = 5,000,000 world units |
| `pMotionVectorsSRV` | `RRMotionVectors` | RG16F, input | Previous minus current in render pixels, X right/Y down, includes camera motion; scale (1,1) |
| `pDiffuseAlbedoSRV` | `RRDiffuseAlbedo` | RGBA16F, input | Linear RGB material/stable-radiance guide from #18 |
| `pSpecularAlbedoSRV` | `RRSpecAlbedo` | RGBA16F, input | Linear RGB material guide from #18 |
| `pNormalsSRV` | `RRNormalsAndRoughness` | RGBA16F, input | Unit virtual-world normal in RGB, linear roughness in W |
| `pOutputUAV` | Reuse `ProcessedOutputColor` | RGBA16F, display | Distinct from every input, including at native resolution |

Every texture and default view covers a full 2D, single-mip, single-sample image.
`SuperResolutionActive` in the target dimensions records differing input/display
extents; it does not authorize running conventional SR after RR.

### Depth, motion and camera correspondence

The port's `Bridge::ExportSurface` writes **ray distance** to the existing
`Depth`; it is neither hardware depth nor linear view Z. RR prepare reconstructs
the virtual world point with `ComputePathTracerCameraRay`, then transforms it by
`view.MatWorldToView`. BUILD uses `sampleBaseIndex` with mini-constant offset zero.
The shared camera helper retains BUILD's temporal jitter, micro-jitter, aperture
sample and near-plane origin offset. No perspective-depth approximation or NRD
view-Z buffer is reused. Nonpositive/nonfinite depth falls back to finite sky Z.
Set `LinearDepth=True`, `DepthInverted=False`; reverse hardware Z is inapplicable.

`ComputePathTracerMotionVector` is the existing BUILD projection calculation
extracted unchanged from `Bridge::computeMotionVector`: unjittered current and
previous world-to-clip transforms, perspective divide, previous minus current,
then `(width/2,-height/2)`. Virtual object motion and previous camera transforms
both contribute. RR copies XY to RG16F without normalization or sign reversal;
nonfinite vectors become zero. `MotionVectorsJittered=False`.

`Frame.view.PixelOffset` is RR's pixel-space jitter, with the same values returned
by `IRayReconstruction::GetJitterOffset`. Camera storage is that value times
`(1,-1)`, following `MakePathTracerCameraData`; the adapter rejects mismatches.
`MakePathTracerViewData` supplies unjittered CPU `MatWorldToView`/`MatViewToClip`.
They are copied directly as row-major Diligent row-vector transforms; only the
native provider converts them to NGX's matrix convention.

The descriptor fixes HDR, automatic exposure and packed roughness. Each execute
starts from zero/default attributes, explicitly sets `PreExposure=ExposureScale=1`,
and sets separate roughness, exposure, diffuse/specular hit distance and specular
motion SRVs to null. Size/camera mismatch rejects the frame and clears bindings.

### Synchronization and validation boundary

`RunDLSSRRDenoiserPrepareInputs(Context, Targets)` selects the owned outputs,
unbinds render targets, orders stable-plane UAV writes, and transitions raw depth
and motion through the regular compute commit. It transitions all six prepared
outputs to SRV after dispatch. The execute adapter requests automatic Core
transitions; Core transitions the separate output to UAV and emits an NGX-write
UAV barrier. The #20 consumer must commit its output SRV with transitions and
rebind state after RR execution; failed RR output must never be consumed.

Validation is documented in `tests/README.md`: native NGX size queries, resource
rebuild/reuse/reset, production C++ prepare dispatch and rebind, alias rejection,
14 shader readback cases including planar scene/camera motion and lens jitter.
Tests run on D3D12 and Vulkan with validation enabled. The existing NRD
`RGB10A2_UNORM` UAV is unsupported on the tested Vulkan device; RR no longer
depends on it. NRD resource-mode regression runs on D3D12; Vulkan tests switch
RR/reference instead and report the NRD limitation. Existing DXC warnings for
the NRD `rgb10_a2` annotation remain outside this stage's changes.

No #20 UI/frame scheduling, native RR evaluation or real path-traced scene image
quality is claimed by these tests. Interactive window resize, history behavior,
and producer→NGX→post-processing validation require that next stage.

## DLSS-RR Stages 3 and 4: Execution and Runtime (#20, #21)

`RTXPTRayReconstructionPass` owns the native factory and reconstruction object.
It enumerates the DLSS variant, queries the requested output/quality dimensions,
creates a fixed-resolution object, obtains pixel jitter and executes the #19
adapter. Output size, quality or preset changes recreate the object. Release
submits pending work and waits for the owning immediate context before destroying
NGX resources; mode exit and sample shutdown use the same path. Creation and
execution failures are latched until configuration changes or explicit retry.
Unavailable capability, source-query failure, creation failure and execution
failure have separate status values and readable UI/log messages.

`RTXPTSample::UpdateRenderTargetDimensions` resolves RR before target allocation
and frame constants. `GetJitterOffset` supplies the same pixel jitter to ray
generation and the RR adapter. Target recreation happens before the frame's reset
scope is consumed. Existing scene/camera-selection, render-target, material,
lighting and realtime-cache reset signals invalidate RR history. Continuous
camera and skinned-object motion preserve temporal history; skipped/failed RR
frames invalidate it. Reference rendering never enables RR denoising constants.

The realtime RR route is BUILD/FILL and shared guides -> RR input preparation ->
native Execute -> ProcessedOutputColor -> bloom -> tone mapping -> blit. It skips
standalone NRD, no-denoiser merge, TAA and ordinary SR. Execute runs outside render
passes; neither the sample scheduler nor SampleBase opens a query across it.
Following passes bind their pipeline/resources with transitions after native
execution invalidates context state. A failed frame follows the existing fallback
clear path and never consumes the previous RR output.

CMake conditionally links `Diligent-RayReconstruction-static` and uses Core's
`copy_diligent_ngx_runtime` / `copy_dlssd_dlls` helpers. The optional
`RTXPT_RAY_RECONSTRUCTION_SHARED=ON` selects and deploys the shared RR module.
`DILIGENT_NO_RAY_RECONSTRUCTION=ON` builds a capability-unavailable stub. SR and RR
share Core's pinned SDK and NGX runtime; no Streamline or additional SDK is added.
Vulkan sets `EngineVkCreateInfo::EnableRayReconstruction` before device creation;
`--vk_rr false` allows launching other modes without requesting those extensions.
Missing explicitly requested extensions remain a device-creation error.

Preset F remains the default on every backend. Preset E is selected explicitly in
the UI or with `--rr_preset E`; Vulkan F's known SDK layout error is not suppressed.
The UI's retry action rebuilds the capability factory and invalidates history.
See `tests/README.md` for command-line execution and the opt-in real-scene sequence.

The integration exposed a Core Vulkan cross-module bug: RR's internal inline
`GetCommandBuffer().EndRenderScope()` used the provider module's uninitialized
Vulkan dispatch pointers when flushing pending barriers from an engine-DLL
context. `DLSSProviderVk::PrepareContext` now submits through virtual
`IDeviceContext::Flush`; subsequent native handles also come through the public
virtual interface. `RayReconstructionTest.EvaluateAndReadback` covers queued
barriers and a shared RR object. This is independent of the known SDK preset-F
layout failure and does not alter the public RR interface.

Real-scene Vulkan validation also exposed a vertex storage ABI mismatch predating
RR. CPU GLTF vertices are 32 bytes (position/normal/UV at offsets 0/12/24), while
default SPIR-V layout of adjacent float3 members produced offsets 0/16/32 and
stride 48. `GeometryVertexData` is now only an unpacked shading value;
`PackedGeometryVertexData` stores two float4 values with matching 32-byte layout
on both backends. Shared pack/unpack helpers are used by hit, emissive-triangle
and skinning shaders. The CPU stream and BLAS layout remain the same. The GPU
regression checks the production skinning shader against independent CPU vertex
values and catches the old Vulkan mismatch. See the validation record for the
separate reference-startup device-loss limitation.

## Skinned glTF Current Geometry

The Diligent RTXPT port uses a Diligent-native current-geometry path for skinned glTF:

- static primitives read the original GLTF vertex buffer 0
- skinned glTF node instances write current-frame vertices into `RTXPTSkinnedGeometry`
- `SubInstanceData::Flags & kSubInstanceFlag_Skinned` selects the skinned vertex arena in `PathTracerBridge.hlsli`
- skinned BLAS records update from that same arena before ray dispatch

This intentionally differs from RTXPT-fork's scene framework and keeps the invariant needed by emissive-triangle R2 work: BLAS, closest-hit fetch, and future emissive-triangle extraction must all consume the same current-frame GPU geometry. Bind-pose fallback is not allowed for skinned emissive meshes.
