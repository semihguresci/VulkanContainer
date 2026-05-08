# Architecture

Runtime ownership is intentionally layered:

```text
Application
  -> VulkanContextInitializer / window setup
  -> RendererFrontend
  -> subsystem managers
  -> FrameRecorder render graph
```

`RendererFrontend` owns renderer lifetime and frame submission. `FrameRecorder`
owns command-buffer pass order. Specialized managers own Vulkan resources for
lighting, shadows, frame resources, culling, OIT, bloom, environment maps, and
scene data. `BimManager` owns sidecar geometry and draw data so IFC, IFCX,
dotbim, and USD/USDZ content can be rendered without being folded
into the primary glTF scene buffers.

## Frame Flow

```text
Depth prepass
  -> BIM depth prepass
  -> Hi-Z / occlusion cull
  -> G-buffer
  -> BIM G-buffer
  -> shadow cascades
  -> tile light cull
  -> GTAO
  -> deferred lighting + transparent OIT
  -> bloom
  -> post-process + debug UI
```

## Ownership Boundaries

- `Application` owns window lifetime and drives the main loop.
- `VulkanContextInitializer` builds the Vulkan instance, device, queues, and
  surface-dependent context.
- `RendererFrontend` wires together managers, frame resources, pipelines, scene
  state, synchronization, and per-frame submission.
- `FrameRecorder` records passes in render-graph order and keeps Vulkan layout
  transitions near the passes that require them.
- `SceneController` owns the shared ECS `World`. Renderable draw extraction,
  active camera data, and generated point lights are mirrored through that
  registry for renderer-side queries.
- `FrameResourceManager` owns per-swapchain-image attachments, descriptor sets,
  samplers, and OIT storage.
- `SceneManager` owns model loading, scene descriptors, materials, texture
  resources, and draw data.
- `BimManager` owns `.bim`, `.ifc`, `.ifcx`, `.usd`, `.usda`, `.usdc`, `.usdz`, and
  fallback glTF sidecar loading, GPU buffers, sidecar object data, and BIM draw
  lists.
- `LightingManager`, `ShadowManager`, `GpuCullManager`, `EnvironmentManager`,
  `OitManager`, and `BloomManager` own their focused rendering resources.

Prefer adding new behavior to the narrowest manager that already owns the data
being mutated. Keep `RendererFrontend` as orchestration code rather than a home
for feature-specific Vulkan resources.

## BIM GPU Filtering

The BIM renderer uses GPU visibility masks and compacted indirect draw streams
for large model filtering and LOD. The safety and performance contracts are
documented in [BIM GPU Visibility And Draw Compaction](bim-gpu-visibility-compaction.md).
