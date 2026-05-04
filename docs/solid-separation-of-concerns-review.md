# SOLID And Separation Of Concerns Review

Status: implementation in progress
Date: 2026-05-04
Scope: repository-wide review before implementing the multi-technique rendering
plan in `docs/render-techniques-architecture-plan.md`.

Implementation progress:

- Milestone 0 complete: `render_technique_registry_tests.cpp` includes
  source-scan guardrails that block future ray/path/splat/radiance-field pass
  registration inside `FrameRecorder.cpp`.
- Milestone 1 complete: `RenderTechnique`, `RenderTechniqueRegistry`,
  `RenderSystemContext`, and `DeferredRasterTechnique` exist. The renderer
  selects the configured technique through the registry and currently registers
  deferred raster only.
- Milestone 2 complete: deferred raster graph registration moved out of
  `FrameRecorder` and into `DeferredRasterTechnique`; `FrameRecorder` now
  executes the graph prepared by the active technique.
- Milestone 3 started: shared deferred frame-state predicates and policy helpers
  live in `DeferredRasterFrameState` instead of being duplicated across graph
  registration and command recording. Post-process frame-state policy,
  push-constant assembly, fallback exposure resolution, and the fullscreen
  post-process pass scope now live in `DeferredRasterPostProcess`.
  Transform-gizmo state and deferred gizmo pass recording now live in
  `TransformGizmoState` and `DeferredRasterTransformGizmo`;
  screenshot copy recording now lives in `ScreenshotCaptureRecorder`.
  Shared negative-height viewport/scissor setup now lives in `SceneViewport`
  and is consumed by `FrameRecorder`, post-process, and gizmo helpers.
  BIM draw-compaction slot/source planning now lives in
  `renderer/bim/BimDrawCompactionPlanner`, keeping `FrameRecorder` on the
  command-recording side of the boundary.
  BIM native point/curve primitive pass planning now lives in
  `renderer/bim/BimPrimitivePassPlanner`, including native-vs-placeholder
  routing, GPU-compaction slot selection, and opacity/width sanitization.
  BIM surface draw routing now lives in
  `renderer/bim/BimSurfaceDrawRoutingPlanner`, including aggregate-vs-split
  CPU fallback selection and GPU-visibility fallback suppression.
  BIM frame draw routing now lives in
  `renderer/bim/BimFrameDrawRoutingPlanner`, including GPU-visibility
  eligibility, CPU-filtered draw-list handoff, native point/curve source
  priority, and primitive pass enablement.
  Shadow cascade CPU draw-list planning now lives in
  `renderer/shadow/ShadowCascadeDrawPlanner`, including cascade activity,
  visible-run splitting, GPU-shadow-cull CPU bypasses, BIM CPU fallback
  suppression, and draw-plan cache signatures.
  Deferred lighting frame policy now lives in `DeferredRasterLighting`,
  including wireframe/object-normal primary path selection, tiled-vs-stencil
  point-light routing, transparent OIT gating, and debug overlay enablement.
  BIM lighting overlay planning now lives in
  `renderer/bim/BimLightingOverlayPlanner`, including point/curve style route
  selection, floor-plan overlay gating, hover/selection styling, native
  point/curve highlight sizing, selection outline readiness, and overlay
  opacity/line-width sanitization.
  Additional Vulkan pass-recorder extraction remains.
- Milestone 4 started: `FrameResourceRegistry` and `PipelineRegistry` provide
  technique-scoped registration points, with guardrails keeping future
  algorithm resources out of the deferred compatibility structs.
  `RenderGraphBuilder` now gives techniques a graph registration facade instead
  of requiring direct `FrameRecorder::graph_` access. `RenderGraph` also
  publishes a UI-neutral `RenderGraphDebugModel` for pass state.
- Milestone 5 started: backend-neutral `SceneProvider` contracts now define
  mesh, BIM, gaussian-splatting, and radiance-field provider slots. Renderer-side
  extraction interfaces define raster batches, ray-tracing build inputs,
  splatting dispatch inputs, and radiance-field dispatch inputs without
  depending on deferred draw lists. `RendererFrontend` now carries a
  `SceneProviderRegistry` through `RenderSystemContext`.
- Milestone 6 started: `TechniqueDebugModel`, `RenderGraphDebugModel`, and
  `SceneDebugModel` provide UI-neutral debug/settings surfaces. Techniques can
  publish debug controls without adding concrete state to `GuiManager`, and
  scene provider snapshots can be converted to `SceneDebugModel`.
- Milestone 7 started: `RendererDeviceCapabilities` is part of
  `RenderSystemContext`, reports missing ray/path tracing requirements, and lets
  technique availability reject unsupported capabilities before technique
  selection succeeds. Known technique descriptors list future algorithms without
  registering unavailable implementations by default.

This review was produced from four focused read-only agent passes:

- Renderer core: `RendererFrontend`, `FrameRecorder`, `RenderGraph`, frame
  resources, render passes, pipelines, telemetry.
- Utility/GPU/UI: Vulkan wrappers, swapchain, allocation, pipeline manager,
  window/input, GUI, CMake target shape.
- Scene/assets/ECS: glTF, BIM, materials, geometry, `SceneManager`,
  `SceneController`, ECS world, future splat/radiance-field/ray-tracing data
  providers.
- Tests/docs/build: architecture docs, render graph tests, visual regression,
  guardrails missing before technique implementation.

## Executive Summary

The codebase already has useful separation at the CMake target level and a good
manager-oriented runtime model. The current renderer can be evolved safely, but
the next change must be architectural, not a new rendering algorithm.

The main issue is that the current deferred raster pipeline is encoded as the
renderer itself:

- `RendererFrontend` is the composition root, frame loop, resize coordinator,
  descriptor updater, GUI bridge, screenshot coordinator, telemetry coordinator,
  and builder of per-frame render parameters.
- `FrameRecorder` is effectively the current deferred raster technique. It
  builds the graph, owns concrete pass recording, reads GUI state, prepares
  culling and shadow draw data, and records post-process/debug work.
- `FrameResources`, `PipelineTypes`, and `RenderGraph` encode a fixed deferred
  frame shape.
- `SceneManager`, `SceneController`, and `BimManager` mix asset import,
  scene-domain state, GPU upload, draw extraction, descriptors, and picking.
- `GuiManager` owns ImGui integration and also stores renderer-specific control
  state for BIM, lighting, bloom, exposure, pass toggles, telemetry, and scene
  inspection.

Adding ray tracing, path tracing, splatting, or radiance fields directly to this
shape would violate SRP and OCP further. First, make the current deferred path a
pluggable technique, then add new algorithms behind the same interfaces.

## Current Responsibility Map

### Application Layer

Current owner:

- `Application` owns top-level lifetime: window, input bridge, Vulkan context,
  pipeline manager, swapchain, command buffers, allocation manager, and
  `RendererFrontend`.

Concern:

- This is acceptable as the application composition root. It should not grow
  rendering-technique knowledge. Technique selection can live in `AppConfig`,
  but technique lifecycle should remain inside renderer architecture.

### Renderer Core

Current owners:

- `RendererFrontend`: renderer composition root and frame submission owner.
- `FrameRecorder`: graph construction and concrete Vulkan command recording.
- `RenderGraph`: scheduling, pass/resource dependency validation, execution
  status, telemetry-facing pass data.
- `FrameResourceManager`: deferred attachments, framebuffers, descriptor layouts
  and descriptor updates for lighting, post-process, and OIT.
- `RenderPassManager`: fixed Vulkan render-pass set for the current frame flow.
- `GraphicsPipelineBuilder` and `PipelineTypes`: fixed graphics pipeline set.
- `RendererTelemetry` and `RenderPassGpuProfiler`: frame and pass timing.

Primary concerns:

- `FrameRecorder` has technique-specific knowledge and should not receive new
  algorithm branches.
- `RenderGraph` is generic in execution but closed in identity because
  `RenderPassId` and `RenderResourceId` are static enums.
- `FrameResourceManager` and `FrameResources` are deferred-resource bundles, not
  general frame-resource infrastructure.
- `PipelineTypes` will become unbounded if every technique adds fields.

### Utility, GPU, Window, UI

Current owners:

- `VulkanInstance`, `VulkanDevice`: instance/device and queues.
- `SwapChainManager`: swapchain images, image views, framebuffers, present.
- `VulkanMemoryManager`, `AllocationManager`: VMA allocator, buffer/image
  creation, uploads, texture decoding/upload, transfer command submission.
- `PipelineManager`: descriptor layouts, pools, pipeline layouts, pipeline
  caches, raw graphics/compute pipelines.
- `WindowManager`, `Window`, `InputManager`, `WindowInputBridge`: platform
  window and input state/callbacks.
- `GuiManager`: ImGui backend plus renderer-specific controls and state.

Primary concerns:

- `VulkanDevice` asks `SwapChainManager` for queue/surface suitability helpers,
  crossing device selection with swapchain ownership.
- `SwapChainManager` owns framebuffers tied to one render pass, which couples
  presentation images to a concrete render-pass compatibility path.
- `AllocationManager` mixes memory allocation, upload scheduling, image
  decoding, image creation, and synchronous queue waits.
- `WindowInputBridge` and `Window` both depend on GLFW user-pointer callback
  state.
- `GuiManager` is not just UI infrastructure; it is a renderer state container.
- The renderer target publicly depends on window and UI, making headless or
  offscreen techniques harder.

### Scene, Assets, ECS, Materials

Current owners:

- `SceneManager`: primary glTF/default scene loading, material and texture
  setup, Vulkan descriptors, GPU material buffers, bounds, light extraction,
  and scene graph population.
- `SceneController`: scene graph build/reload, ECS sync, GPU geometry upload,
  object SSBO creation/upload, draw-command extraction, picking, diagnostic
  geometry.
- `BimManager`: BIM import dispatch, metadata, material conversion, GPU upload,
  draw lists, meshlet/visibility/compaction resources, picking.
- `World`: EnTT registry mirror of `SceneGraph` renderables, lights, and active
  camera.
- Geometry loaders: glTF, dotbim, IFC, IFCX, USD.

Primary concerns:

- `SceneManager` mixes asset import, domain conversion, material management,
  GPU upload, descriptor layout/sets, and light extraction.
- `SceneController` mixes scene-domain updates, GPU resource upload, draw
  extraction, and picking.
- `BimManager` mixes importer orchestration, BIM metadata/domain logic, GPU
  resources, visibility filtering, draw compaction, and picking.
- `geometry::Vertex` exposes Vulkan vertex input descriptions, making CPU asset
  data depend on the renderer backend.
- ECS components include GPU ABI types from `SceneData.h`, so ECS is not a
  backend-neutral domain layer.
- `SceneData.h` is a large GPU ABI bucket covering object, material, lighting,
  shadow, exposure, post-process, culling, bloom, and push constants.

### Tests And Documentation

Current strengths:

- Architecture and refactoring docs already define the high-level ownership
  chain.
- Render graph tests cover ordering, dependencies, resource access, skip
  reasons, active plans, and current frame order.
- Telemetry, rendering conventions, scene graph, ECS, loaders, material, BIM,
  and visual regression tests exist.

Missing guardrails:

- No implemented `RenderTechnique`, `RenderTechniqueRegistry`,
  `RenderSystemContext`, or `FrameResourceRegistry`.
- No test prevents new algorithms from being added directly to
  `FrameRecorder`.
- No dependency-direction test prevents renderer-core from gaining UI/window
  coupling.
- No CMake/CI target currently enforces clang-tidy, include budget,
  dependency direction, or coverage thresholds.
- GPU visual regression is opt-in and should remain opt-in by default, but it
  needs a named required local profile before major renderer changes.

## SOLID Findings

### Single Responsibility Principle

High-risk classes:

- `RendererFrontend`: orchestration, lifecycle, frame loop, descriptor update,
  render parameter assembly, GUI sync, telemetry, screenshots.
- `FrameRecorder`: graph construction, pass recording, deferred raster
  technique, GUI-mode gates, debug overlay, screenshot copy.
- `SceneManager`: asset loading, material import, texture loading, descriptors,
  GPU buffers, lights, bounds, scene graph population.
- `SceneController`: scene graph sync, ECS sync, geometry upload, object buffer
  upload, draw extraction, picking.
- `BimManager`: importer dispatch, BIM metadata, renderer resources, draw
  extraction, meshlet residency, visibility filtering, picking.
- `GuiManager`: ImGui backend plus renderer domain state and controls.

### Open/Closed Principle

Current extension pressure:

- Adding a new render technique currently requires edits to `RendererFrontend`,
  `FrameRecorder`, `RenderGraph`, `PipelineTypes`, `FrameResources`,
  `GraphicsPipelineBuilder`, `GuiManager`, and tests.
- Adding a new scene representation currently does not have a provider slot. It
  must either become mesh-like draw commands or get special handling.

Target:

- Techniques should be registered, not edited into central classes.
- Scene representations should expose provider/extractor interfaces, not force
  every asset through `PrimitiveRange` and `DrawCommand`.

### Liskov Substitution Principle

Main concern:

- There are not many polymorphic renderer abstractions yet. The next interface
  must define capability and lifecycle contracts clearly so unsupported
  techniques can be unavailable without partial object behavior.

Target:

- A technique either declares all requirements and can be instantiated, or it is
  unavailable with a clear reason.

### Interface Segregation Principle

Current issue:

- Renderer code receives concrete managers with broad APIs. `FrameRecordParams`
  and `GuiManager::drawSceneControls` are large, multi-purpose interfaces.

Target:

- Use smaller service/context interfaces: device, allocator, upload, pipeline
  registry, presentation target, scene GPU data, debug model.

### Dependency Inversion Principle

Current issue:

- High-level renderer flow depends on concrete low-level managers and UI
  classes.
- CPU scene and ECS layers include GPU ABI data.
- Device selection depends on swapchain helper methods.

Target:

- High-level renderer systems depend on renderer-facing interfaces.
- CPU asset/domain layers stay free of Vulkan.
- UI depends on debug/view models, not concrete renderer managers.

## Target Logical Boundaries

### 1. Platform And Application

Purpose:

- Own process startup, window creation, app config, main loop, and top-level
  lifetime.

Should contain:

- `Application`
- command-line config parsing
- window lifecycle wiring

Should not contain:

- render technique implementation
- graph pass selection
- technique resource creation

### 2. GPU Core

Purpose:

- Own Vulkan instance/device, queues, physical-device capabilities, memory
  allocator, uploads, pipeline cache, descriptors, and sync primitives.

Target submodules:

- `DeviceContext`
- `RendererDeviceCapabilities`
- `SurfaceCapabilitiesProvider`
- `GpuAllocator`
- `BufferAllocator`
- `ImageAllocator`
- `TransferScheduler`
- `PipelineRegistry`
- `DescriptorRegistry`

Separation work:

- Move queue-family and swapchain support queries out of `SwapChainManager`.
- Split `AllocationManager` responsibilities into allocation, upload, texture
  decode, and transfer scheduling.
- Keep raw Vulkan wrappers, but expose small renderer-facing service interfaces.

### 3. Presentation

Purpose:

- Own swapchain images, presentation, and final target metadata.

Target submodules:

- `SwapchainPresenter`
- `PresentationTarget`
- `PresentationFramebufferSet` for render-pass based paths
- dynamic-rendering target compatibility for future paths

Separation work:

- Remove render-pass framebuffer ownership from `SwapChainManager`.
- Let techniques or a presentation target builder create compatible final
  framebuffers.

### 4. Render Graph Core

Purpose:

- Schedule and execute passes based on declared dependencies and resources.

Should contain:

- graph nodes
- pass/resource declarations
- dependency validation
- active/skipped status
- execution hooks for telemetry/profiling

Should not contain:

- deferred-specific pass/resource enum expansion as the only extension path
- technique-specific pass recording

Target:

- Add `RenderGraphBuilder`.
- Keep existing enum IDs during migration.
- Add dynamic technique-local pass/resource IDs.

### 5. Renderer Core And Techniques

Purpose:

- Select, create, update, resize, and execute render techniques.

Target submodules:

- `RenderTechnique`
- `RenderTechniqueRegistry`
- `RenderSystemContext`
- `FrameBuildContext`
- `TechniqueDebugModel`
- `DeferredRasterTechnique`
- `ForwardRasterTechnique`
- later `RayTracingTechnique`, `PathTracingTechnique`,
  `GaussianSplattingTechnique`, `RadianceFieldTechnique`

Separation work:

- `FrameRecorder` becomes graph execution and common recording utilities only.
- Current deferred graph registration and pass recorders move into
  `DeferredRasterTechnique`.
- New algorithms must not add branches to `FrameRecorder`.

### 6. Technique-Owned Resources And Pipelines

Purpose:

- Prevent global structs from growing for every algorithm.

Target submodules:

- `FrameResourceRegistry`
- `FrameImageDesc`
- `FrameBufferDesc`
- `TechniquePipelineSet`
- `PipelineRecipe`

Technique-owned examples:

- `DeferredRasterResources`
- `DeferredRasterPipelines`
- `ForwardRasterResources`
- `ForwardRasterPipelines`
- `RayTracingResources`
- `RayTracingPipelines`
- `PathTracingAccumulationResources`
- `SplattingResources`
- `RadianceFieldResources`

Separation work:

- Keep existing `FrameResources` and `PipelineTypes` as deferred compatibility
  adapters only during migration.
- Do not add path-tracing, ray-tracing, splat, or radiance-field fields to the
  current deferred structs.

### 7. Assets And Scene Domain

Purpose:

- Own backend-neutral scene data, asset import, material definitions, hierarchy,
  instances, and lights.

Target submodules:

- `geometry/assets`: pure CPU import and canonical asset data, no Vulkan.
- `scene`: instance ownership and one source of truth for hierarchy/ECS.
- `material`: CPU material and texture references, no `VkImage`/`VkSampler`.
- `scene/providers`: mesh, BIM, splat, radiance-field provider interfaces.

Separation work:

- Move Vulkan vertex input descriptions out of `geometry::Vertex`.
- Split `SceneManager` into asset/material loading and renderer resource upload.
- Decide whether `SceneGraph` or ECS is the scene source of truth. Until then,
  keep ECS explicitly as a render mirror and avoid adding domain ownership to it.
- Move GPU ABI structs out of ECS-facing components where possible.

### 8. Render Extraction

Purpose:

- Convert backend-neutral scene/provider data into technique-specific render
  batches.

Target extractors:

- `MeshRasterExtractor`
- `BimRasterExtractor`
- `RayTracingSceneExtractor`
- `SplattingExtractor`
- `RadianceFieldExtractor`

Outputs:

- raster draw batches
- transparent/opaque buckets
- GPU object/material tables
- BLAS/TLAS build inputs
- splat sort/bin inputs
- radiance-field raymarch inputs

Separation work:

- Draw extraction should not live inside asset importers.
- Ray tracing should consume mesh/BIM provider data through build inputs, not
  through deferred draw lists.
- Splats and radiance fields should not be forced through triangle
  `PrimitiveRange`.

### 9. UI And Debug

Purpose:

- Render UI from view models and publish user changes back through settings
  models.

Target submodules:

- `GuiBackend`
- `RendererDebugPanel`
- `TechniqueDebugPanel`
- `TechniqueDebugModel`
- `RenderGraphDebugModel`
- `SceneDebugModel`

Separation work:

- Keep ImGui setup/rendering in `GuiManager`.
- Move renderer-specific state into debug/settings models owned by renderer or
  techniques.
- Techniques publish controls; `GuiManager` renders generic controls.
- UI should not include or depend on concrete BIM/renderer managers.

## Proposed Dependency Direction

Target direction:

```text
app
  -> platform/window/input
  -> gpu-core
  -> assets/material/scene
  -> renderer-core/interfaces
  -> renderer-resources
  -> renderer-techniques
  -> ui-debug-models
```

More concretely:

```text
geometry/assets        no Vulkan, no renderer
material               no Vulkan image/sampler ownership
scene                  no Vulkan resource ownership
gpu-core               no scene-specific import logic
renderer-core          depends on gpu-core + scene interfaces
renderer-techniques    depend on renderer-core + extractors + resources
ui                     depends on debug view models, not concrete managers
app                    composes everything
```

Current direction violations to unwind:

- `geometry::Vertex` exposes Vulkan vertex input state.
- ECS components include GPU ABI structs.
- `VulkanDevice` depends on swapchain helper methods.
- renderer target publicly depends on UI/window.
- `GuiManager` includes renderer/BIM-specific state.
- `SceneManager` owns Vulkan descriptor and GPU upload work.
- `BimManager` is both domain provider and renderer resource owner.

## Refactor Milestones Before Implementing New Algorithms

### Milestone 0: Guardrail Document And Tests

Tasks:

- Add this document.
- Add architecture guardrail tests before implementation:
  - `render_technique_registry_tests.cpp`
  - dependency fallback tests
  - deferred graph order through a technique registration test
  - source-scan guard that rejects ray/path/splat/radiance-field pass
    registration inside `FrameRecorder.cpp`

Acceptance:

- New tests run in the normal CPU CTest profile.
- The repo has a documented rule: no new rendering algorithm code in
  `FrameRecorder`.

### Milestone 1: Technique API Skeleton

Tasks:

- Add `RenderTechniqueId`, `RenderTechnique`, `RenderTechniqueRegistry`.
- Add `RenderSystemContext`.
- Add `FrameBuildContext`.
- Register only `DeferredRasterTechnique`.
- Add UI/config field for active technique, but keep only deferred available.

Acceptance:

- Current renderer output is unchanged.
- Technique selection exists without adding new algorithms.
- `RendererFrontend` asks the registry for the active technique.

### Milestone 2: Move Deferred Graph Registration

Tasks:

- Move `FrameRecorder::buildGraph()` pass registration into
  `DeferredRasterTechnique`.
- Keep `RenderGraph` execution unchanged.
- Keep pass names/order compatible with existing tests.

Acceptance:

- Existing render graph tests still pass.
- `FrameRecorder` no longer owns deferred graph construction.
- `RendererFrontend` delegates graph construction through the active technique.

### Milestone 3: Split Deferred Recording From Common Recording

Tasks:

- Move deferred-specific pass recorders into `DeferredRasterTechnique` or
  deferred pass helper files.
- Keep only generic graph execution/profiling hooks in `FrameRecorder`.
- Move GUI display-mode decisions into technique settings/state prepared before
  command recording.

Acceptance:

- `FrameRecorder` stops including high-level managers for lighting, OIT,
  environment, bloom, exposure, scene, BIM, and GUI.
- Deferred raster behavior remains unchanged.

### Milestone 4: Resource And Pipeline Registries

Tasks:

- Add `FrameResourceRegistry`.
- Add `PipelineRegistry` / `PipelineRecipe`.
- Wrap current `FrameResources` as `DeferredRasterResources`.
- Wrap current `PipelineBuildResult` as `DeferredRasterPipelines`.

Acceptance:

- Adding `ForwardRasterTechnique` does not require fields in
  `GraphicsPipelines`.
- Adding a path-tracing accumulation image does not require fields in
  `FrameResources`.

### Milestone 5: Scene Provider And Extraction Split

Tasks:

- Introduce backend-neutral provider outputs:
  - `MeshSceneAsset`
  - `BimSceneAsset`
  - future `SplatSceneAsset`
  - future `RadianceFieldAsset`
- Add renderer-side extractors:
  - mesh/BIM raster extraction
  - ray-tracing build input extraction
- Move GPU upload/descriptors out of scene import classes incrementally.

Acceptance:

- Asset import can be tested without Vulkan.
- Ray tracing can consume mesh/BIM geometry without depending on deferred draw
  lists.
- Splats/radiance fields have a planned provider slot that is not triangle-only.

### Milestone 6: UI Debug Model Split

Tasks:

- Add `TechniqueDebugModel`, `RenderGraphDebugModel`, and `SceneDebugModel`.
- Make techniques publish settings and pass toggles as models.
- Keep `GuiManager` as ImGui renderer of generic models.

Acceptance:

- `GuiManager` no longer needs concrete renderer/BIM manager headers for new
  technique controls.
- Technique-specific controls can be added without editing one giant UI method.

### Milestone 7: Device Capabilities

Tasks:

- Add `RendererDeviceCapabilities`.
- Move surface/swapchain support queries out of `SwapChainManager`.
- Record ray-tracing/ray-query/acceleration-structure support.
- Teach `RenderTechniqueRegistry` to hide or reject unsupported techniques.

Acceptance:

- Unsupported techniques fail at registration/selection time with a clear
  reason.
- Ray/path tracing work has a capability path before any shader work starts.

## Provider-Specific Boundaries For Future Algorithms

### Mesh

Input:

- glTF/default mesh assets, CPU materials, texture references, lights, hierarchy.

Provider output:

- backend-neutral `MeshSceneAsset`
- primitive ranges
- instances
- material references
- bounds

Extractor outputs:

- raster draw batches
- object/material GPU tables
- ray-tracing triangle geometry build inputs

### BIM

Input:

- dotbim, IFC, IFCX, USD, glTF fallback.

Provider output:

- backend-neutral BIM asset with geometry, native points/curves, meshlets,
  metadata, element IDs, bounds, georeferencing, materials.

Extractor outputs:

- BIM raster batches
- visibility/filter batches
- ray-tracing build inputs for mesh geometry
- metadata lookup tables for picking/inspection

### Gaussian Splatting

Input:

- splat cloud files or generated splat data.

Provider output:

- structure-of-arrays splat data:
  - position
  - covariance or scale/rotation
  - opacity
  - SH/radiance coefficients
  - bounds/LOD

Extractor outputs:

- cull inputs
- sort/bin inputs
- splat draw or compute dispatch batches

Rule:

- Do not force splats through `PrimitiveRange` or indexed mesh draw commands.

### Radiance Fields / NeRF

Input:

- density/color grids, sparse voxel grids, hash-grid/MLP weights, occupancy
  data, or baked feature fields.

Provider output:

- field bounds
- occupancy data
- feature/density resources
- raymarch settings

Extractor outputs:

- raymarch dispatch inputs
- composition inputs against mesh depth/color

Rule:

- Do not model this as triangle geometry. It is a volumetric provider.

### Ray Tracing / Path Tracing

Input:

- mesh and BIM providers, material references, transforms, lights, environment.

Provider/extractor output:

- backend-neutral acceleration-structure build inputs:
  - triangle buffers
  - instance transforms
  - material IDs
  - geometry flags
  - revision counters

Renderer-side owner:

- `RayTracingResourceManager` owns Vulkan BLAS/TLAS and shader binding tables.

Rule:

- Ray tracing consumes provider data. It should not depend on deferred G-buffer
  or raster draw-list internals.

## Immediate Do-Not-Do Rules

- Do not add ray tracing, path tracing, splatting, or radiance-field passes to
  `FrameRecorder::buildGraph()`.
- Do not add new algorithm-specific fields to the current `GraphicsPipelines`
  or `FrameResources` structs.
- Do not make `GuiManager` own new technique-specific state directly.
- Do not route splats or radiance fields through triangle `PrimitiveRange` just
  to reuse raster draw code.
- Do not add ray-tracing Vulkan extensions as unconditional required
  extensions before capability fallback exists.
- Do not move more CPU asset/domain concepts into `SceneData.h`.

## Recommended First Implementation Slice

The first code slice should be deliberately boring:

1. Add `RenderTechnique` and `RenderTechniqueRegistry`.
2. Add `DeferredRasterTechnique` as a wrapper around current behavior.
3. Add CPU tests for registration, fallback, and deferred graph order.
4. Keep visual output unchanged.
5. Stop there and verify.

Only after that slice should the renderer begin moving pass recording, resource
ownership, and pipeline ownership out of the fixed deferred structs.
