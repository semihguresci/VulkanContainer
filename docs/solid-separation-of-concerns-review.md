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
- Milestone 3 complete for the current migration slice: deferred graph
  registration is technique-owned and many deferred, scene, BIM, shadow,
  post-process, overlay, transition, and command-buffer helpers have moved into
  grouped renderer helpers. Shared
  deferred frame-state predicates and policy helpers live in
  `DeferredRasterFrameState` instead of being duplicated across graph
  registration and command recording. Post-process frame-state policy,
  push-constant assembly, fallback exposure resolution, and the fullscreen
  post-process pass scope now live in `DeferredRasterPostProcess`.
  Deferred lighting pass orchestration, scene and BIM depth/G-buffer frame-pass
  delegation, transform-gizmo pass/overlay recording, transparent-pick
  frame-pass recording, and shadow cascade frame-pass cache/prep are no longer
  owned by `FrameRecorder`.
  Deferred frustum-cull pass readiness, draw-count policy, object-descriptor
  update policy, and freeze/unfreeze action selection now live in
  `DeferredRasterFrustumCullPassPlanner`; the public `GpuCullManager` upload and
  dispatch sequence now lives in `DeferredRasterFrustumCullPassRecorder`.
  Deferred tile-cull pass readiness, screen/depth/camera dispatch inputs, and
  tiled-lighting availability policy now live in `DeferredRasterTileCullPlanner`;
  the `LightingManager` timer/dispatch command sequence now lives in
  `DeferredRasterTileCullRecorder`.
  Transform-gizmo state and deferred gizmo pass/overlay recording now live in
  `TransformGizmoState`, `DeferredRasterTransformGizmo`, and the deferred
  technique graph callbacks instead of `FrameRecorder`;
  screenshot copy recording now lives in `ScreenshotCaptureRecorder`.
  Shared negative-height viewport/scissor setup now lives in `SceneViewport`
  and is consumed by `FrameRecorder`, post-process, and gizmo helpers.
  Deferred depth-to-read-only transition planning and command recording now
  lives in `DeferredRasterDepthReadOnlyTransitionRecorder`, keeping the
  depth/stencil and hidden-shadow-atlas image barrier contracts out of
  `DeferredRasterTechnique`.
  Shared image-barrier step emission now lives in `DeferredRasterImageBarrier`
  so deferred pass-specific recorders can own policy while reusing the same
  Vulkan barrier command path.
  Hi-Z depth round-trip transition planning and command recording now lives in
  `DeferredRasterHiZDepthTransitionRecorder`, keeping the attachment-to-sampling
  and sampling-to-attachment barrier contracts out of the Hi-Z graph lambda.
  Hi-Z graph readiness now lives in `DeferredRasterHiZPassPlanner`, keeping
  manager, frame, depth-view, sampler, and depth-image gates in one reusable
  pass policy.
  Deferred scene-color read barrier planning and command recording now lives in
  `DeferredRasterSceneColorReadBarrierRecorder`, keeping the shared
  color-attachment-to-compute-read barrier contract out of exposure adaptation
  and bloom graph lambdas.
  BIM draw-compaction slot/source planning now lives in
  `renderer/bim/BimDrawCompactionPlanner`, keeping `FrameRecorder` on the
  command-recording side of the boundary.
  BIM native point/curve primitive pass planning now lives in
  `renderer/bim/BimPrimitivePassPlanner`, including native-vs-placeholder
  routing, GPU-compaction slot selection, and opacity/width sanitization.
  BIM native point/curve primitive pass command recording now lives in
  `renderer/bim/BimPrimitivePassRecorder`, keeping shared wireframe pass
  binding, GPU-compaction draw submission, and CPU wireframe emission out of
  `FrameRecorder`.
  BIM surface draw routing now lives in
  `renderer/bim/BimSurfaceDrawRoutingPlanner`, including aggregate-vs-split
  CPU fallback selection and GPU-visibility fallback suppression.
  BIM opaque surface pass planning now lives in
  `renderer/bim/BimSurfacePassPlanner`, including depth/G-buffer and BIM
  transparent pass readiness, stable mesh/point-placeholder/curve-placeholder
  source ordering, mesh-only GPU compaction eligibility, GPU-visibility CPU
  fallback ownership, and semantic-color push-constant policy.
  BIM surface pass command recording now lives in
  `renderer/bim/BimSurfacePassRecorder`, including descriptor/geometry binding,
  shared GPU-compacted draw submission, and CPU fallback draw emission for BIM
  depth, G-buffer, transparent-pick, and transparent-lighting routes.
  BIM surface depth/G-buffer render-pass shells now live in
  `renderer/bim/BimSurfaceRasterPassRecorder`, keeping render-pass begin/end,
  viewport/scissor setup, semantic-color push-constant adjustment, and
  delegation to the shared BIM surface recorder out of `FrameRecorder`.
  Deferred BIM depth/G-buffer frame-pass delegation now lives in
  `DeferredRasterBimSurfacePassRecorder` behind narrow record inputs, with
  `DeferredRasterTechnique` translating the current frame contract into the BIM
  frame binding.
  BIM transparent surface plan construction for transparent pick and deferred
  OIT now routes through `buildBimSurfaceFramePassPlan`, so those frame sites no
  longer build `BimSurfacePassInputs` directly.
  BIM frame draw routing now lives in
  `renderer/bim/BimFrameDrawRoutingPlanner`, including GPU-visibility
  eligibility, CPU-filtered draw-list handoff, native point/curve source
  priority, and primitive pass enablement.
  Shadow cascade CPU draw-list planning now lives in
  `renderer/shadow/ShadowCascadeDrawPlanner`, including cascade activity,
  visible-run splitting, GPU-shadow-cull CPU bypasses, BIM CPU fallback
  suppression, and draw-plan cache signatures.
  Shadow pass draw routing now lives in
  `renderer/shadow/ShadowPassDrawPlanner`, including scene GPU-cull handoff,
  scene CPU route ordering, BIM GPU-filtered slot ordering, and BIM CPU route
  ordering.
  Shadow pass command recording now lives in
  `renderer/shadow/ShadowPassRecorder`, including shadow viewport/depth-bias
  setup, scene/BIM descriptor and geometry binding, route pipeline selection,
  GPU indirect scene submission, BIM compacted draw submission, and CPU shadow
  draw emission.
  Shadow cascade preparation policy now lives in
  `renderer/shadow/ShadowCascadePreparationPlanner`, keeping shadow-atlas
  visibility, active cascade, GPU-cull fallback, and BIM-shadow geometry gates
  out of `FrameRecorder`.
  Shadow cascade GPU-cull readiness now lives in
  `renderer/shadow/ShadowCascadeGpuCullPlanner`, keeping shadow-cull pass
  activity, manager readiness, indirect buffer availability, and draw-capacity
  policy out of `FrameRecorder`.
  Shadow GPU-cull source upload policy also lives in
  `renderer/shadow/ShadowCascadeGpuCullPlanner`, keeping shadow-atlas
  visibility, source draw-list presence, manager readiness, and required
  upload capacity out of `FrameRecorder`.
  Shadow-cull graph pass policy now lives in
  `renderer/shadow/ShadowCullPassPlanner`, and the thin dispatch adapter lives
  in `renderer/shadow/ShadowCullPassRecorder`, keeping cascade cull readiness
  and dispatch gating out of `DeferredRasterTechnique`.
  Deferred lighting frame policy now lives in `DeferredRasterLighting`,
  including wireframe/object-normal primary path selection, tiled-vs-stencil
  point-light routing, transparent OIT gating, and debug overlay enablement.
  Deferred directional-light command recording now lives in
  `DeferredDirectionalLightingRecorder`, keeping fullscreen pipeline binding,
  lighting/light/scene descriptor binding, and fullscreen triangle submission
  out of `FrameRecorder`.
  Deferred lighting descriptor routing now lives in
  `DeferredLightingDescriptorPlanner`, keeping directional, point, tiled, and
  light-gizmo descriptor-set ordering out of `FrameRecorder`.
  Deferred transparent OIT command recording now lives in
  `DeferredTransparentOitRecorder`, keeping scene/BIM transparent descriptor
  replacement, geometry binding, semantic-color push-constant selection, and
  transparent scene/BIM recorder delegation out of `FrameRecorder`.
  OIT clear and resolve-preparation command adapters also live in
  `DeferredTransparentOitRecorder`, so `FrameRecorder` no longer calls
  `OitManager::clearResources` or `OitManager::prepareResolve`.
  OIT frame-level enable/readiness policy and clear/resolve delegation now live
  behind `DeferredTransparentOitFramePassRecorder`, so `DeferredRasterTechnique`
  no longer asks `FrameRecorder` for raw OIT activation or `OitManager` service
  access.
  Deferred GUI command dispatch now lives in `DeferredRasterGuiPassRecorder`,
  keeping the direct `GuiManager::render` call out of `FrameRecorder`.
  Deferred frame-level service compatibility now lives in
  `DeferredRasterFrameGraphContext`, keeping GUI dispatch, display-mode reads,
  shadow cascade orchestration, BIM GPU-visibility setup/recording, OIT
  frame-policy access, debug-overlay ownership, and screenshot-copy scheduling
  out of the generic recorder.
  Transparent pick draw recording now lives in `TransparentPickPassRecorder`,
  keeping scene/BIM transparent-pick descriptor binding, geometry binding,
  transparent scene draw delegation, and BIM surface pass delegation out of
  `FrameRecorder`.
  Transparent pick frame-pass recording now lives in
  `TransparentPickRasterPassRecorder`, keeping pick render-pass begin/end,
  viewport/scissor setup, depth-copy sequencing, scene transparent planning, and
  BIM transparent-pick planning out of `FrameRecorder`.
  Transparent pick depth-copy planning and command recording now lives in
  `TransparentPickDepthCopyRecorder`, keeping depth/stencil barriers and
  depth-image copy sequencing out of `FrameRecorder`.
  Deferred point-light draw planning now lives in
  `DeferredPointLightingDrawPlanner`, including tiled push-constant assembly,
  stencil pipeline selection, per-light route copying, and light-count
  clamping.
  Deferred point-light command recording now lives in
  `DeferredPointLightingRecorder`, including tiled accumulation dispatch,
  clustered-light timing, stencil-volume clears, light push constants, and
  fullscreen point-light accumulation.
  Deferred light-gizmo command recording now lives in
  `DeferredLightGizmoRecorder`, with the shared `LightPushConstants` shader
  contract split out of `LightingManager` so gizmo placement policy can stay in
  the manager while Vulkan bind/push/draw sequencing lives in a recorder.
  Deferred light-gizmo push-constant planning now lives in
  `DeferredLightGizmoPlanner`, keeping directional/point gizmo placement,
  color normalization, extent clamping, and point-count limits out of
  `LightingManager`.
  BIM lighting overlay planning now lives in
  `renderer/bim/BimLightingOverlayPlanner`, including point/curve style route
  selection, floor-plan overlay gating, hover/selection styling, native
  point/curve highlight sizing, selection outline readiness, and overlay
  opacity/line-width sanitization.
  BIM lighting overlay command recording now lives in
  `renderer/bim/BimLightingOverlayRecorder`, including overlay pipeline
  fallback selection, scene/BIM geometry binding, wireframe draw submission,
  and selection mask/outline stencil recording.
  Scene opaque draw planning now lives in `SceneOpaqueDrawPlanner`, including
  depth/G-buffer GPU-indirect handoff and stable CPU route ordering for
  single-sided, winding-flipped, and double-sided opaque scene draws.
  Scene opaque draw command recording now lives in `SceneOpaqueDrawRecorder`,
  keeping depth/G-buffer descriptor and geometry binding, route pipeline
  selection, GPU-indirect submission, and CPU scene draw emission out of
  `FrameRecorder`.
  Scene diagnostic cube command recording now lives in
  `SceneDiagnosticCubeRecorder`, keeping diagnostic cube pipeline/descriptor
  binding, geometry binding, object-index push constants, and indexed draw
  emission out of `FrameRecorder`.
  Scene depth/G-buffer pass planning now lives in `SceneRasterPassPlanner`,
  keeping pass-kind clear-value selection, opaque draw-plan assembly, and
  primary/front-cull/no-cull pipeline fallback policy out of `FrameRecorder`.
  Scene depth/G-buffer render-pass shells now live in
  `SceneRasterPassRecorder`, keeping scene clear-value contracts,
  render-pass begin/end, viewport/scissor setup, scene opaque draw delegation,
  and diagnostic cube delegation out of `FrameRecorder`.
  Deferred scene depth/G-buffer frame-pass delegation now lives in
  `DeferredRasterScenePassRecorder` behind narrow record inputs, with
  `DeferredRasterTechnique` translating the current frame contract into scene
  draw, geometry, pipeline, and diagnostic-cube bindings.
  Scene transparent draw planning now lives in
  `SceneTransparentDrawPlanner`, including aggregate-vs-split transparent
  draw-list selection and stable primary/front-cull/no-cull route ordering.
  Scene transparent draw command recording now lives in
  `SceneTransparentDrawRecorder`, keeping transparent-pick and transparent
  lighting descriptor binding, geometry binding, route pipeline selection, and
  draw submission out of `FrameRecorder`.
  Deferred debug overlay route planning now lives in
  `DeferredRasterDebugOverlayPlanner`, including full/overlay wireframe source
  routing, object-normal cull-family routing, aggregate geometry/surface-normal
  routes, and normal-validation face-classification routes.
  Deferred debug overlay command recording now lives in
  `DeferredRasterDebugOverlayRecorder`, including debug source geometry binding,
  debug pipeline selection, diagnostic cube emission, wireframe/scene overlay
  draws, normal validation, and surface-normal recording.
  BIM section clip-cap pass planning now lives in
  `renderer/bim/BimSectionClipCapPassPlanner`, including fill/hatch route
  gating, style transfer, and hatch line-width fallback policy.
  BIM section clip-cap command recording now lives in
  `renderer/bim/BimSectionClipCapPassRecorder`, including descriptor and cap
  geometry binding, section-plane push-constant override, fill/hatch pipeline
  selection, and hatch line-width set/reset.
  Shared Vulkan render-pass scope recording now lives in
  `RenderPassScopeRecorder`, keeping render-pass begin/end and secondary
  command-buffer execution out of `FrameRecorder` for the remaining shadow pass
  shell and the deferred lighting pass recorder.
  Shared command-buffer lifecycle recording now lives in
  `CommandBufferScopeRecorder`, keeping primary command-buffer begin/end and
  shadow secondary reset/begin/end inheritance setup out of `FrameRecorder`.
  Deferred lighting pass attachment policy now lives in
  `DeferredLightingPassPlanner`, keeping lighting render-area, clear-value, and
  selection-stencil clear contracts out of `FrameRecorder`.
  Deferred lighting pass command entry now lives in
  `DeferredRasterLightingPassRecorder`, keeping deferred lighting state
  assembly, debug overlay routing, transparent OIT, BIM primitive/section/overlay
  commands, light gizmos, and lighting render-pass scope out of
  `FrameRecorder`.
  BIM frame GPU-visibility preparation and update recording now lives in
  `renderer/bim/BimFrameGpuVisibilityRecorder`, keeping draw-compaction
  preparation and meshlet/visibility/compaction update sequencing out of
  `FrameRecorder`.
  Shadow pass scope policy now lives in
  `renderer/shadow/ShadowPassScopePlanner`, keeping the fixed shadow render
  area, reverse-Z depth clear, and inline-vs-secondary subpass contents out of
  `FrameRecorder`.
  Shadow pass raster shell planning and command recording now live in
  `renderer/shadow/ShadowPassRasterPlanner` and
  `renderer/shadow/ShadowPassRasterRecorder`, keeping shadow render-pass
  begin/secondary execution/end sequencing out of `FrameRecorder`.
  Shadow secondary command-buffer eligibility now lives in
  `renderer/shadow/ShadowSecondaryCommandBufferPlanner`, keeping the
  GPU-filtered BIM suppression and CPU-command threshold policy out of
  `FrameRecorder`.
  Shadow secondary cascade scheduling and command-buffer lifecycle recording
  now live in `renderer/shadow/ShadowCascadeSecondaryCommandBufferRecorder`,
  keeping parallel worker dispatch and secondary reset/begin/end error handling
  out of `FrameRecorder`.
  Shadow cascade frame-pass orchestration now lives in
  `renderer/shadow/ShadowCascadeFramePassRecorder`, keeping GPU-cull source
  upload, draw-plan cache ownership, cascade pass input assembly, secondary
  command-buffer preparation, and cascade pass recording out of `FrameRecorder`.
  `FrameRecorder` no longer owns the remaining shadow delegate,
  scene/display-mode access, debug-overlay instance, screenshot copy, BIM
  GPU-visibility, or deferred manager-service compatibility adapters. It now
  owns render-graph execution, command-buffer begin/end, telemetry/profiling
  hooks, and technique-provided lifecycle hooks.
- Milestone 4 complete: `FrameResourceRegistry` and `PipelineRegistry` provide
  technique-scoped contracts and runtime binding views, with guardrails keeping
  future algorithm resources out of deferred compatibility structs.
  `RendererFrontend` owns the contract and runtime registries, exposes them
  through `RenderSystemContext`, publishes production `FrameResourceManager`
  image, buffer, framebuffer, descriptor, and sampler bindings into
  `FrameRecordParams`, and carries production Vulkan pipeline handles/layouts
  through registry views on `GraphicsPipelines` and `PipelineLayouts` instead
  of copying those structs into the per-frame contract. Deferred raster
  registers its resource and pipeline contracts before graph setup. Deferred
  raster and shadow consumers now use registry-only bridge helpers for
  framebuffers, images/views, buffers, descriptor sets, samplers, pipelines, and
  layouts with no fixed `GraphicsPipelines`/`PipelineLayouts` fallback. Scene,
  BIM-scene, light, tiled-lighting, shadow, frame-owned lighting, post-process,
  and OIT descriptors are published as runtime descriptor bindings. The
  g-buffer sampler is published as a runtime sampler binding. Frontend
  depth/pick readback plus OIT telemetry consume the published registry
  bindings instead of `FrameResources`. `FrameRecordParams` no longer carries
  fixed `FrameDescriptorSets`, `FrameRenderPassHandles`, or direct
  `gBufferSampler` handles; shadow and post-process render passes live on their
  service state. Shadow cascade framebuffer arrays and swapchain presentation
  framebuffers remain explicit shadow and swapchain service contracts for this
  milestone. The fixed `FrameResources` struct remains internal
  `FrameResourceManager` storage; `PipelineBuildResult` remains a production
  pipeline build output and should shrink or be renamed when a second
  production technique is added. `RenderGraphBuilder` gives techniques a graph
  registration facade instead of requiring direct `FrameRecorder::graph_`
  access, and `RenderGraph` publishes a UI-neutral `RenderGraphDebugModel` for
  pass state.
- Milestone 5 complete for the provider/extraction integration slice:
  backend-neutral `SceneProvider` contracts now
  define mesh, BIM, gaussian-splatting, and radiance-field provider slots, with
  concrete provider adapters for all four representation kinds.
  Mesh and BIM snapshots carry neutral `SceneProviderTriangleBatch` metadata for
  index ranges, material IDs, sidedness, transparency, and instance counts.
  Renderer-side extraction interfaces and provider-backed implementations define
  raster batches, ray-tracing build inputs, splatting dispatch inputs, and
  radiance-field dispatch inputs without depending on deferred draw lists.
  Technique-facing extraction outputs preserve provider bounds and relevant
  revision metadata so future techniques can track geometry, material,
  instance, culling, and volume updates without reaching back into frontend
  managers.
  `MeshSceneProviderBuilder` builds mesh provider assets from CPU primitive and
  material facts without Vulkan, draw-command, or frame-recording state.
  `RendererFrontend` owns a `SceneProviderRegistry`, carries it through
  `RenderSystemContext`, registers/synchronizes production mesh and BIM provider
  adapters through `SceneProviderSynchronizer` on initialization, reload, and
  frame parameter assembly, populates mesh provider assets through
  `buildMeshSceneAsset`, populates BIM triangle batches from `BimManager`, and
  publishes
  provider-derived extraction snapshots
  into `FrameRecordParams`. Deferred BIM pass readiness consumes the provider
  raster view as a sidecar while keeping current-frame draw routing
  authoritative. `SceneProviderSynchronizer` also exposes neutral
  gaussian-splatting and radiance-field sync inputs, so matching runtime scene
  state can be registered without changing the registry/extraction contract.
  BIM mesh-surface raster counts remain separate from native point/curve range
  counts; native-only BIM providers do not produce raster surface batches.
  Concrete splat and radiance-field providers are CPU-only and
  non-triangle-backed; their runtime inputs remain empty until matching loaders
  or scene owners exist.
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
- `FrameRecorder` is no longer the deferred graph owner or deferred-frame
  compatibility facade. It executes the prepared graph and common
  command-buffer/profiling lifecycle; `DeferredRasterFrameGraphContext` owns the
  current deferred compatibility services needed by `DeferredRasterTechnique`.
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
- `FrameRecorder`: prepared graph execution, command-buffer lifecycle, lifecycle
  hooks, telemetry, and GPU profiling.
- `RenderGraph`: scheduling, pass/resource dependency validation, execution
  status, telemetry-facing pass data.
- `FrameResourceManager`: deferred attachments, framebuffers, descriptor layouts
  and descriptor updates for lighting, post-process, and OIT.
- `RenderPassManager`: fixed Vulkan render-pass set for the current frame flow.
- `GraphicsPipelineBuilder` and `PipelineTypes`: fixed graphics pipeline set.
- `RendererTelemetry` and `RenderPassGpuProfiler`: frame and pass timing.

Primary concerns:

- `FrameRecorder` now has a narrow execution role; remaining renderer-core
  coupling risk is the static pass/resource identity model and explicit service
  contracts, not fixed per-frame resource ownership.
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

Implemented guardrails:

- Technique registry and fallback behavior are covered by
  `render_technique_registry_tests.cpp`.
- Source-scan tests prevent ray/path/splat/radiance-field graph registration
  from being added directly to `FrameRecorder.cpp`.
- Source-scan tests prevent new broad deferred/BIM/shadow command hub methods
  from being added to `FrameRecorder`.
- `resource_pipeline_registry_tests.cpp` covers technique-scoped resource and
  pipeline registries, deferred raster contract registration, and
  `RendererFrontend` wiring through `RenderSystemContext`.
- `scene_provider_extraction_tests.cpp` covers backend-neutral scene provider
  contracts, mesh/BIM provider adapters, renderer registry wiring, and planned
  non-triangle provider slots.

Missing guardrails:

- No dependency-direction test prevents renderer-core from gaining UI/window
  coupling.
- No CMake/CI target currently enforces clang-tidy, include budget,
  dependency direction, or coverage thresholds.
- GPU visual regression is opt-in and should remain opt-in by default, but it
  needs a named required local profile before major renderer changes.
- The full CPU build/test profile must be green before marking the
  multi-technique migration complete; current focused object builds verify the
  latest slices, with the known Windows `rendering_convention_tests.exe`
  linker-file lock handled through a manual validation executable when needed.

## SOLID Findings

### Single Responsibility Principle

High-risk classes:

- `RendererFrontend`: orchestration, lifecycle, frame loop, descriptor update,
  render parameter assembly, GUI sync, telemetry, screenshots.
- `FrameRecorder`: graph execution, command-buffer begin/end, lifecycle hooks,
  and common telemetry/profiling dispatch. Residual risk is the fixed
  `FrameRecordParams` contract, not deferred command ownership.
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

Current status:

- Deferred graph construction has moved to `DeferredRasterTechnique`.
- Many pass planners and command recorders now live in grouped deferred, BIM,
  shadow, scene, and shared recorder helpers.
- `FrameRecorder` no longer owns deferred lighting, scene depth/G-buffer, BIM
  depth/G-buffer, transparent-pick frame pass, transform-gizmo pass/overlay, or
  shadow cascade cache/prep/input assembly. It also no longer owns OIT
  clear/resolve command adapters, OIT frame-level activation/readiness
  delegation, direct GUI render dispatch, shadow cascade recording, BIM
  GPU-visibility recording, screenshot copy, debug-overlay ownership, or
  high-level manager/helper headers. Deferred raster compatibility services are
  isolated in `DeferredRasterFrameGraphContext`.
- Milestone 3 is complete for the current migration slice: `FrameRecorder`
  keeps only graph execution, command-buffer lifecycle, telemetry/profiling, and
  lifecycle hook dispatch. The fixed deferred resource/frame contract cleanup is
  covered by the completed Milestone 4 registry migration.
- Guardrail tests now block reintroducing broad deferred/BIM/shadow command
  hubs after the split.

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

Current status:

- `FrameResourceRegistry` and `PipelineRegistry` exist and are carried through
  `RenderSystemContext`.
- `RendererFrontend` creates the production registries and the selected
  technique publishes its contracts through the registry context.
- `DeferredRasterTechnique` registers its current deferred resource and
  pipeline recipes before graph construction.
- `FrameResourceManager` now owns a runtime `FrameResourceRegistry` view and
  publishes actual per-frame attachments, OIT images, OIT buffers, and
  framebuffer handles after creation and clears those bindings on destruction.
  `FrameResources` remains internal manager storage rather than a
  `FrameRecordParams` dependency.
- `GraphicsPipelines` now carries a `PipelineRegistry` handle view,
  `PipelineLayouts` carries a layout view, and `GraphicsPipelineBuilder`
  publishes actual Vulkan pipeline handles and layouts into those registries
  after production pipeline creation.
- `FrameRecordParams` now exposes registry-backed `resourceBinding(...)`,
  `imageBinding(...)`, `bufferBinding(...)`, `framebufferBinding(...)`,
  `framebuffer(...)`, `descriptorBinding(...)`, `descriptorSet(...)`,
  `samplerBinding(...)`, `sampler(...)`, `pipelineHandle(...)`, and
  `pipelineLayout(...)` helpers plus separate
  contract/binding/recipe/handle/layout registry pointers. New techniques can
  resolve runtime resources and handles without adding fields to
  `FrameResources` or `GraphicsPipelines`; `FrameRecordParams` no longer
  exposes a `FrameResources` pointer.
- Tests cover technique-scoped forward-raster pipeline recipes and
  path-tracing accumulation resources without adding fields to the fixed
  deferred compatibility structs.
- Consumption migration is complete for this milestone: the registry-backed
  production view is active, deferred raster and shadow pipeline consumers use
  registry-only bridge helpers for handles and layouts, and `FrameRecordParams`
  no longer carries fixed `GraphicsPipelines` or `PipelineLayouts` copies.
  Published deferred framebuffers are declared in the resource contract
  registry, and published deferred framebuffers, image/view inputs, OIT
  resources, frame-owned descriptor sets, manager-owned scene/BIM/light/tiled
  lighting/shadow descriptors, and the g-buffer sampler now resolve through
  registry-only resource bridge helpers. Frontend depth/pick readback and OIT
  capacity telemetry use the same published runtime bindings instead of the
  fixed frame storage. `FrameRecordParams` no longer carries fixed
  `FrameDescriptorSets`, `FrameRenderPassHandles`, or direct `gBufferSampler`
  fields; shadow and post-process render passes live on their service state.
  Shadow cascade framebuffer arrays and swapchain presentation framebuffers
  remain explicit shadow and swapchain service contracts for this milestone
  rather than registry bindings. The remaining fixed structs are compatibility
  storage at manager/builder boundaries and should shrink further when a second
  production technique is added.

### Milestone 5: Scene Provider And Extraction Split

Tasks:

- Introduce backend-neutral provider outputs:
  - `MeshSceneAsset`
  - `BimSceneAsset`
  - `GaussianSplatSceneAsset`
  - `RadianceFieldSceneAsset`
- Add renderer-side extractors:
  - mesh/BIM raster extraction
  - ray-tracing build input extraction
- Move GPU upload/descriptors out of scene import classes incrementally.

Acceptance:

- Asset import can be tested without Vulkan.
- Ray tracing can consume mesh/BIM geometry without depending on deferred draw
  lists.
- Splats/radiance fields have a planned provider slot that is not triangle-only.

Current status:

- `SceneProviderSnapshot` carries backend-neutral representation counts,
  revisions, bounds, and neutral triangle-batch metadata so renderer
  extraction does not need Vulkan, draw commands, frame params, or primitive
  ranges.
- Concrete mesh, BIM, gaussian-splatting, and radiance-field providers expose
  snapshots through the same `IRenderSceneProvider` interface. Splat and
  radiance-field snapshots intentionally keep `triangleBatches` empty and use
  splat/field metadata consumed by their extractors.
- Production mesh and BIM providers are owned by `RendererFrontend`,
  synchronized from `SceneManager`, `SceneController`, and `BimManager` through
  a pure `SceneProviderSynchronizer`, and registered in the production
  `SceneProviderRegistry`. Mesh snapshots preserve material counts, and BIM
  snapshots preserve opaque/transparent mesh-surface counts separately from
  native point/curve range splits instead of folding all counts into one
  deferred-shaped draw count.
  Mesh provider assets are built by the CPU-only `MeshSceneProviderBuilder`
  from neutral primitive/material facts translated from `SceneManager`; BIM
  triangle batches are translated by `BimManager::sceneProviderTriangleBatches()`.
- The same synchronizer now has neutral gaussian-splatting and radiance-field
  inputs. They publish non-triangle providers into the same registry and clear
  them when unavailable, so future loaders can connect runtime state without
  touching renderer graph code or forcing volumetric/splat data through mesh
  draw-list contracts.
- Provider-backed mesh/BIM raster extractors, ray-tracing build-input
  extraction, splatting dispatch extraction, and radiance-field dispatch
  extraction now consume provider snapshots. Mesh and BIM ray-tracing inputs use
  provider triangle batches when available, fall back to primitive/instance
  counts when older providers omit batches, carry geometry/material/instance
  revisions plus bounds, and never depend on deferred draw lists. Splatting and
  radiance-field dispatch inputs carry provider bounds and representation
  metadata such as SH coefficient count or occupancy presence.
- `FrameRecordParams` now carries `ProviderSceneExtraction`, populated each
  frame by `RendererFrontend::buildFrameRecordParams()` from the current
  registry. This gives techniques a per-frame provider-derived extraction view
  without capturing frontend managers.
- `DeferredRasterTechnique` consumes `ProviderSceneExtraction::rasterBatches`
  for BIM surface readiness as a provider/extraction bridge. The pass still
  requires current-frame opaque BIM draw commands, so filters, layer routing,
  and draw-budget decisions remain authoritative.
- Splat and radiance-field provider slots remain planned-only because the app
  does not yet own production splat/radiance-field scene state; the concrete
  provider adapters and extraction paths are ready for that state when loaders
  are introduced.

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
- neutral triangle batches
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
  neutral triangle batches, metadata, element IDs, bounds, georeferencing,
  materials.

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
- Do not add new deferred, BIM, or shadow command hubs to `FrameRecorder`; keep
  grouped pass policy and command recording in renderer helper files.
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
