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

Noisy radiance is current-frame linear RGBA16F; reference accumulation is linear RGBA32F. Display uses an sRGB RTV to encode once and a UNORM SRV to copy encoded bytes. Depth/motion are not published as validated realtime guides. `RTXPTPostProcessPipeline` independently takes an explicit tone-mapping RTV format and does not require a swapchain. The full interactive sample still includes its existing reconstruction dependencies; realtime orchestration and dynamic temporal guides are separate follow-up work.
