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
scene data.

## Frame Flow

```text
Depth prepass
  -> Hi-Z / occlusion cull
  -> G-buffer
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
- `LightingManager`, `ShadowManager`, `GpuCullManager`, `EnvironmentManager`,
  `OitManager`, and `BloomManager` own their focused rendering resources.

Prefer adding new behavior to the narrowest manager that already owns the data
being mutated. Keep `RendererFrontend` as orchestration code rather than a home
for feature-specific Vulkan resources.
