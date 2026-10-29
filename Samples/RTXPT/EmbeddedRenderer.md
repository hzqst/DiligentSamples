# Embedded reference renderer

`RTXPTEmbeddedRenderer` provides the static reference path used by the Sample (`--embedded_reference true`) and by external hosts. It shares the Sample's scene/material loaders, acceleration structures, light baking, ray tracing, accumulation and tone mapping passes. It does not create a window, device, swap chain or presentation loop.

Initialize it with an existing device and immediate context, an uploaded `RTXPTScene` and `RTXPTMaterials`, an explicit shader source factory and output dimensions. Keep the scene and materials alive through GPU completion. The shader factory must resolve the RTXPT shader tree and its include directories; scene environment textures use `RTXPTScene::GetFileSource()` instead. Initialization can submit GPU work and waits for completion. The host must synchronize before destroying or replacing a renderer that has rendered frames.

Pass `PathTracerCameraData`, `PathTracerViewData` and host time to `Render`. Camera and projection changes restart accumulation; pass `ResetHistory=true` for cuts or discontinuities. Create a new instance for a new scene/version or output size. Keep one instance per stable view. Counters advance only after successful tracing and output processing.

The path includes static BLAS/TLAS, alpha-tested textured materials, scene analytic lights (including instantiated glTF punctual lights), emissive triangles, and scene environment maps. No environment means black, including miss rays; the Sample's fallback sky is suppressed. Static exported objects remain frozen. Skinning is rejected. No PVS or primary-camera visibility pruning is performed.

Radiance and processed color use RGBA16F, progressive accumulation uses RGBA32F. Reinhard tone mapping with fixed exposure is followed by one sRGB RTV encoding. `GetOutput()` returns an RGBA8 typeless texture whose default UNORM SRV exposes display bytes. A host copying into an sRGB RTV should use an sRGB SRV to avoid double encoding. UI belongs after this output and must not enter accumulation.

Required enabled features are compute, bindless resources, and standalone ray tracing shaders. The component uses the supplied primary context. It does not require denoising, DLSS/RR, SHARC, asynchronous queues, Sample UI, or probe lighting. Failure returns a diagnostic; fallback and presentation are host policy. `GetTraceCount()` and `GetAccumulationCount()` provide execution evidence for tests.
