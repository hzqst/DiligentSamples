# RTXPT

RTXPT is a staged DiligentEngine port of NVIDIA RTXPT.

The first runnable version is a capability and fallback-view sample. Ray tracing, scene resources, shaders and post-processing are restored incrementally.

## Embedding the reference renderer

Include `cmake/RTXPTLibrary.cmake` to build the scene and embedded renderer without configuring the interactive sample, fetching assets, or requiring NRD/NGX. Both the sample and external hosts use its source list:

```cmake
include(path/to/RTXPT/cmake/RTXPTLibrary.cmake)
add_rtxpt_library(MyRTXPT NO_UI PATH_TRACING_ONLY DEPENDENCIES MyDiligentSDK)
target_link_libraries(MyHost PRIVATE MyRTXPT)
rtxpt_stage_shaders(MyHost "$<TARGET_FILE_DIR:MyHost>/shaders/rtxpt")
rtxpt_install_shaders("shaders/rtxpt")
```

`MyDiligentSDK` supplies Diligent include directories, link directories/libraries and platform definitions, using source targets or an installed SDK interface target. Installed shared archiver builds also require `DILIGENT_ARCHIVER_SHARED=1` on the library. `PATH_TRACING_ONLY` propagates to consumers because it affects public class layouts. `NO_UI` excludes ImGui calls. Neither option changes the reference tracing algorithm. Hosts enabling SHARC additionally provide `SHARC_SHADER_DIR` and deploy its includes. Reconstruction hosts may use `rtxpt_deploy_reconstruction_runtimes`; it reuses Core's enabled provider deployment helpers. Call it for the executable performing GPU work, including a separate render server when applicable.

`RTXPTEmbeddedScene` shares scene preparation across views. Each `RTXPTEmbeddedView` takes texture-local `RenderSize` and `DisplaySize`. Reference currently requires equal nonzero sizes and `DisplayRTVFormat = TEX_FORMAT_RGBA8_UNORM_SRGB`; unsupported requests return an explicit error. Host viewport origins belong only to final composition. Resize by reinitializing/replacing a view after its previous GPU use has completed; it starts fresh history and leaves the shared scene intact.

`GetOutput()` still returns the allocated display target, including before rendering for command recording. It is not proof of a successful frame. After checking `Render`, use `GetOutputInfo` for `Display`, `NoisyRadiance`, or `AccumulatedRadiance`. The descriptors identify texture size, resource/SRV formats, encoding, successful frame index and validity. Failed rendering invalidates all results; recovery resets reference accumulation. Failed reinitialization invalidates old targets. Descriptors and textures are borrowed until the next render/reinitialization/destruction; deferred work must hold its own resource references and obey GPU ordering.

Noisy radiance is current-frame linear RGBA16F; reference accumulation is linear RGBA32F. Display uses an sRGB RTV to encode once and a UNORM SRV to copy encoded bytes. `RTXPTPostProcessPipeline` independently takes an explicit tone-mapping RTV format and does not require a swapchain. The full interactive sample still includes its existing reconstruction dependencies.

## Shared RealtimeRaw pipeline

Set `RTXPTEmbeddedViewCreateInfo::Mode = RTXPTViewMode::RealtimeRaw` to use the same `RTXPTRealtimeRawPipeline` as the Sample's `--realtime_raw true` validation preset. Both use one sample per pixel, equal render/display sizes, an ordered graphics context, and manual exposure. Raw does not allocate progressive radiance history or run NRD, TAA, SR, RR, SHARC, Bloom, or auto exposure. The core library remains independent of NRD/NGX providers.

The pipeline runs BuildDenoisingLayers, current light-buffer/feedback preparation, FillDenoisingLayers, current-frame guide baking, no-denoiser final merge, then the existing tone mapper. Layer radiance guides explicitly skip history reads. The view host owns scene preparation, ray bindings and light sampling, and supplies its frame constants; the common pipeline uploads those constants and owns layer/guide/merge targets. Raw hosts configure Uniform or Power+ sampling, so NEE-AT history is not consumed. Per-view frame/mini constants are bound to SRBs rather than shared cached PSO static variables.

After successful rendering, `GetOutputInfo` also exposes `RayDistance`, `PrimaryMotion`, `SpecularHitDistance`, `AverageLayerRadiance` and `LayerHeader`. `GetRealtimeLayerBuffer()` exposes the corresponding packed layer surface/material records. RayDistance is the tracer's scene ray distance, **not view-space Z**. These are debug inputs, not a complete validated reconstruction input set: `GetRealtimeRawStats().DynamicMotionValidated` remains false until previous dynamic surfaces and reflection correspondence are implemented. Failure invalidates color and guides together; `AccumulatedRadiance` is unavailable in Raw. Completion counters advance only after the full chain succeeds.

The Sample preset freezes rendering controls and disables its settings UI for validation; normal camera input and requested scene animation still work. It disables Vulkan RR capability requests and rejects combinations with embedded reference or advanced validation presets. Its existing advanced realtime route remains available when `--realtime_raw` is omitted. For four separate current-frame HDR captures, run from the Sample source directory with an absolute output path:

```text
RTXPT.exe --mode d3d12 --scene convergence-test.scene.json --width 320 --height 240 --realtime_raw true --show_ui false --hdr_capture_path <absolute-output-prefix> --hdr_capture_start 2 --hdr_capture_count 4 --hdr_capture_sequence true --exit_after_frames 10
```

Each capture is a single frame. Capture readbacks wait for the GPU only when requested; the regular Raw frame has no CPU wait. Scene loading support is unchanged: the standalone Sample can animate its imported scenes, while `RTXPTEmbeddedScene` retains its existing initial static-import restriction. Dynamic runtime instances can still be prepared through the shared scene runtime; correct dynamic motion remains separate work.
