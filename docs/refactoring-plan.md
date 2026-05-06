# VulkanSceneRenderer — Architecture Refactoring Plan

> **Project:** VulkanSceneRenderer
> **Branch:** `refactor`  
> **Date:** 2025  
> **Status:** Living document — update as phases complete

---

## Reader Notes

This document is both a historical refactoring log and a current architecture
map. Treat completed phase sections as design context, not as a guarantee that
every old target name or file grouping still exists exactly as written. For the
current executable and library names, prefer the root `CMakeLists.txt` and the
`src/*/CMakeLists.txt` files.

The highest-level runtime ownership path is:

```text
Application
  -> VulkanContextInitializer / window setup
  -> RendererFrontend
  -> subsystem managers
  -> FrameRecorder render graph
```

`RendererFrontend` owns object lifetime and per-frame submission; `FrameRecorder`
owns command-buffer recording order; subsystem managers own specialized Vulkan
resources. Keep new responsibilities in the narrowest manager that owns the data
being mutated.

### Current Phase Status

| Phase | Status | Notes |
|---|---|---|
| 0 | Complete | Baseline refactoring completed on the refactor branch. |
| 1 | Complete | Monolithic dependency aggregation replaced by focused `Dep_*` groups. |
| 2 | Complete | Utility code split into focused subsystem targets; obsolete aggregate removed later. |
| 3 | Complete | Project types live under the `container::` namespace root. |
| 4 | Complete with measurement caveat | Header structure is decoupled; rebuild-time reduction was not re-measured. |
| 5 | Complete | `RendererFrontend` reduced to grouped ownership/orchestration state. |
| 6 | Complete | Render graph scheduling and tests are in place. |
| 7 | Complete | Shared ECS world covers renderables, point lights, and active camera data. |
| 8 | Complete | Unused dependencies and dead dependency groups removed. |
| 9 | Partial | Lightweight test infrastructure is improved; GPU CI and coverage targets remain deferred. |
| 10 | Complete | Dead targets and redundant structural dependencies removed. |

## Table of Contents

1. [Current State Summary](#1-current-state-summary)
2. [Completed Refactoring (Phase 0)](#2-completed-refactoring-phase-0)
3. [Phase 1 — Split Monolithic `VulkanDependencies`](#3-phase-1--split-monolithic-vulkandependencies)
4. [Phase 2 — Decompose Kitchen-Sink `utility` Library](#4-phase-2--decompose-kitchen-sink-utility-library)
5. [Phase 3 — Namespace Consolidation](#5-phase-3--namespace-consolidation)
6. [Phase 4 — Header Decoupling & Forward Declarations](#6-phase-4--header-decoupling--forward-declarations)
7. [Phase 5 — Decompose `RendererFrontend` God Object](#7-phase-5--decompose-rendererfrontend-god-object)
8. [Phase 6 — Render Graph Abstraction](#8-phase-6--render-graph-abstraction)
9. [Phase 7 — ECS Integration (EnTT)](#9-phase-7--ecs-integration-entt)
10. [Phase 8 — Dependency Audit & Cleanup](#10-phase-8--dependency-audit--cleanup)
11. [Phase 9 — Test Infrastructure Improvements](#11-phase-9--test-infrastructure-improvements)
12. [Phase 10 — Structural Cleanup](#12-phase-10--structural-cleanup)
13. [Appendix A — Current Dependency Graph](#appendix-a--current-dependency-graph)
14. [Appendix B — Namespace Inventory](#appendix-b--namespace-inventory)

---

## 1. Current State Summary

### Architecture at a Glance

| Aspect | Current State | Target State |
|---|---|---|
| **CMake dependency model** | Single monolithic `VulkanDependencies` INTERFACE (~30 libs) | Targeted dependency groups per library |
| **Library granularity** | 4 libraries (Core, renderer, geometry, utility) | 8–10 focused libraries |
| **Utility library** | Kitchen-sink (16 unrelated .cpp files) | Split into vulkan_device, window, scene, gpu_resource, ui |
| **Namespace discipline** | Mixed — 10+ namespaces + global-scope structs | Unified `container::` root namespace |
| **RendererFrontend** | God object (11 owned, 8 borrowed, ~20 methods) | Thin orchestrator delegating to subsystems |
| **Render pass ordering** | Hardcoded in `FrameRecorder` | Data-driven render graph |
| **ECS** | EnTT linked but unused | Scene entities managed via ECS |
| **Header coupling** | `SceneManager.h` includes 14 headers | ≤5 includes per header via forward decls |

### Rendering Pipeline

```
Depth Prepass → G-Buffer Fill → Lighting (Directional + Point/Stencil)
    → Post-Process (OIT Resolve + Tone Map) → Debug Overlays → ImGui
```

### Library Link Graph (Current)

```
VulkanSceneRenderer (executable)
  └─ VulkanSceneRenderer_Core
       ├─ VulkanSceneRenderer_renderer  ──→ VulkanDependencies, utility
       ├─ VulkanSceneRenderer_geometry  ──→ VulkanDependencies, utility
       └─ VulkanSceneRenderer_utility   ──→ VulkanDependencies, spdlog::spdlog
```

`VulkanDependencies` is a single INTERFACE target that links **all** third-party libraries to **every** consumer regardless of actual need.

---

## 2. Completed Refactoring (Phase 0)

These changes were implemented on the `refactor` branch:

| Change | Files | Impact |
|---|---|---|
| **Circular include fix** | Created `include/Container/common/VulkanTypes.h`; updated `VulkanDevice.h`, `SwapChainManager.h` | Broke `VulkanDevice.h` → `SwapChainManager.h` cycle |
| **Application extraction** | Created `include/Container/app/Application.h`, `src/app/Application.cpp` | `main.cpp` reduced from 169 → 22 lines |
| **CMake modularization** | Created `cmake/Shaders.cmake`, `cmake/Assets.cmake` | Root `CMakeLists.txt` reduced from 219 → 38 lines |
| **Hardcoded path removal** | `cmake/DependenciesSettings.cmake` | Portable vcpkg integration |


**Validation:** Build passes; validation status recorded at the time of the refactor.

---

## 3. Phase 1 — Split Monolithic `VulkanDependencies`

**Goal:** Each library links only the third-party packages it actually uses.  
**Risk:** Low — pure CMake change, no C++ modifications.  
**Estimated effort:** Small

### Problem

`cmake/Dependencies.cmake` defines a single `VulkanDependencies` INTERFACE target that aggregates ~30 libraries into groups (`VULKAN_LIBS`, `SHADER_LIBS`, `GRAPHICS_LIBS`, `MATERIALX_LIBS`, `UTILITY_LIBS`, `MEDIA_LIBS`) and then links them **all** together. Every internal target that links `VulkanDependencies` receives the entire dependency set.

### Steps

1. **Audit actual usage** — For each internal library, grep its `.cpp`/`.h` files for `#include` directives that reference third-party headers. Build a matrix:

   | Internal Library | Actually Uses |
   |---|---|
   | `VulkanSceneRenderer_renderer` | Vulkan, VMA, GLM, spdlog, fmt, ImGui, slang |
   | `VulkanSceneRenderer_geometry` | Vulkan, VMA, GLM, tinygltf, stb |
   | `VulkanSceneRenderer_utility` | Vulkan, VMA, GLM, GLFW, spdlog, fmt, ImGui, nlohmann_json, yaml-cpp, MaterialX |
   | Tests (CPU-only) | GTest, GLM |

2. **Define focused CMake targets** in `cmake/Dependencies.cmake`:

   ```cmake
   # -- Core Vulkan --
   add_library(Dep_VulkanCore INTERFACE)
   target_link_libraries(Dep_VulkanCore INTERFACE
       Vulkan::Vulkan Vulkan::Headers GPUOpen::VulkanMemoryAllocator)

   # -- Windowing --
   add_library(Dep_Windowing INTERFACE)
   target_link_libraries(Dep_Windowing INTERFACE glfw)

   # -- Math --
   add_library(Dep_Math INTERFACE)
   target_link_libraries(Dep_Math INTERFACE glm::glm)

   # -- Shader --
   add_library(Dep_Shader INTERFACE)
   target_link_libraries(Dep_Shader INTERFACE slang::slang)

   # -- Scene I/O --
   add_library(Dep_SceneIO INTERFACE)
   target_link_libraries(Dep_SceneIO INTERFACE
       tinygltf::tinygltf unofficial::stb::stb_image ...)

   # -- UI --
   add_library(Dep_UI INTERFACE)
   target_link_libraries(Dep_UI INTERFACE imgui::imgui)

   # -- Serialization --
   add_library(Dep_Serialization INTERFACE)
   target_link_libraries(Dep_Serialization INTERFACE
       nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp)

   # -- Material --
   add_library(Dep_Material INTERFACE)
   target_link_libraries(Dep_Material INTERFACE MaterialX::...)

   # -- Logging --
   add_library(Dep_Logging INTERFACE)
   target_link_libraries(Dep_Logging INTERFACE spdlog::spdlog fmt::fmt)
   ```

3. **Update `src/CMakeLists.txt`** — Replace `VulkanDependencies` with the specific `Dep_*` targets each library needs.

4. **Update `tests/CMakeLists.tests.cmake`** — CPU-only tests link `Dep_Math` + `GTest` instead of `VulkanDependencies`.

5. **Verify:** Build passes, all tests pass. Remove the old monolithic `VulkanDependencies` target.

### Validation Criteria

- [x] No internal target links more than 5 dependency groups
- [x] CPU-only tests do not link Vulkan SDK
- [x] Build passes, all tests pass

**Phase 1 completed.** The monolithic `VulkanDependencies` target has been removed and
replaced by focused `Dep_*` interface groups. CPU-only tests now link only the narrow
dependencies they need.

---

## 4. Phase 2 — Decompose Kitchen-Sink `utility` Library

**Goal:** Split the 16-file `VulkanSceneRenderer_utility` into focused libraries with clear responsibilities.
**Risk:** Medium — requires updating `target_link_libraries` across all consumers.  
**Estimated effort:** Medium

### Problem

`src/utility/CMakeLists.txt` compiles 16 source files into a single library covering: device management, window management, memory allocation, scene management, GUI, materials, textures, pipelines, scene graph, input, and logging. This prevents granular dependency control and increases compile times.

### Proposed Split

| New Library | Source Files | Responsibility |
|---|---|---|
| `VulkanSceneRenderer_vulkan_device` | `VulkanDevice.cpp`, `VulkanInstance.cpp`, `SwapChainManager.cpp`, `FrameSyncManager.cpp` | Vulkan device lifecycle, swap chain, frame synchronization |
| `VulkanSceneRenderer_window` | `WindowManager.cpp`, `InputManager.cpp` | GLFW window + input |
| `VulkanSceneRenderer_gpu_resource` | `AllocationManager.cpp`, `VulkanMemoryManager.cpp`, `TextureManager.cpp`, `PipelineManager.cpp` | GPU memory, buffers, textures, pipeline/descriptor management |
| `VulkanSceneRenderer_scene` | `SceneManager.cpp`, `SceneGraph.cpp`, `MaterialManager.cpp`, `MaterialXIntegration.cpp` | Scene graph, materials, model descriptor sets |
| `VulkanSceneRenderer_ui` | `GuiManager.cpp` | ImGui integration |
| `VulkanSceneRenderer_log` | `Logger.cpp` | Logging (spdlog wrapper) |

### Steps

1. **Create per-library `CMakeLists.txt`** files under `src/utility/<sublibrary>/` or use a flat structure with one `CMakeLists.txt` that defines multiple targets.

2. **Move headers** to match:
   ```
   include/Container/vulkan_device/  (VulkanDevice.h, VulkanInstance.h, SwapChainManager.h, FrameSyncManager.h)
   include/Container/window/         (WindowManager.h, InputManager.h)
   include/Container/gpu_resource/   (AllocationManager.h, VulkanMemoryManager.h, TextureManager.h, PipelineManager.h)
   include/Container/scene/          (SceneManager.h, SceneGraph.h, SceneData.h, MaterialManager.h, MaterialXIntegration.h, Camera.h)
   include/Container/ui/             (GuiManager.h)
   include/Container/log/            (Logger.h)
   ```

   > **Alternative (lower risk):** Keep header locations unchanged initially and only split the CMake targets. Move headers in a follow-up.

3. **Wire dependencies** — Each new target links only its required `Dep_*` groups from Phase 1:

   | New Library | Dependencies |
   |---|---|
   | `vulkan_device` | `Dep_VulkanCore`, `Dep_Math`, `Dep_Logging` |
   | `window` | `Dep_Windowing`, `Dep_Logging` |
   | `gpu_resource` | `Dep_VulkanCore`, `Dep_Math`, `Dep_Logging`, `vulkan_device` |
   | `scene` | `Dep_VulkanCore`, `Dep_Math`, `Dep_SceneIO`, `Dep_Material`, `Dep_Serialization`, `gpu_resource` |
   | `ui` | `Dep_UI`, `Dep_VulkanCore`, `scene` |
   | `log` | `Dep_Logging` |

4. **Update consumers** — `VulkanSceneRenderer_renderer`, `VulkanSceneRenderer_geometry`, and `VulkanSceneRenderer_Core` link the specific sub-libraries they need instead of the monolithic `VulkanSceneRenderer_utility`.

5. **Verify:** Build passes, all tests pass.

### Validation Criteria

- [x] Each sub-library compiles independently
- [x] No circular dependencies between sub-libraries
- [x] Build passes, all tests pass

**Phase 2 completed.** The former kitchen-sink utility aggregation has been split into
focused subsystem targets. Later Phase 8/10 cleanup removed the remaining unused aggregate
target and verified the dependency graph.

---

## 5. Phase 3 — Namespace Consolidation

**Goal:** All project types live under a `container::` root namespace with logical sub-namespaces.  
**Risk:** Medium — touches many files but is mechanical (find-and-replace + using declarations).  
**Estimated effort:** Medium

### Problem

The project uses an inconsistent namespace scheme:

| Current Namespace | Types |
|---|---|
| `renderer::` | `RendererFrontend`, `FrameRecorder`, `RenderPassManager`, `LightPushConstants`, ... |
| `utility::vulkan::` | `VulkanDevice`, `VulkanInstance` |
| `utility::memory::` | `AllocationManager` |
| `utility::scene::` | `SceneManager`, `SceneGraph` |
| `utility::camera::` | `Camera`, `PerspectiveCamera` |
| `utility::pipeline::` | `PipelineManager` |
| `utility::window::` | `WindowManager` |
| `utility::ui::` | `GuiManager` |
| `utility::material::` | `MaterialXIntegration`, `MaterialManager` |
| `utility::input::` | `InputManager` |
| `utility::logger::` | `Logger` |
| `Container::` | `ContainerCore` |
| `app::` | `Application` |
| `common::math::` | `lookAt`, `perspectiveRH_ReverseZ`, `orthoRH_ReverseZ` |
| *(global)* | `CameraData`, `ObjectData`, `LightingData`, `DrawCommand`, `FrameResources`, `AttachmentImage`, `WireframePushConstants`, `NormalValidationPushConstants`, `SurfaceNormalPushConstants`, `BindlessPushConstants`, `PostProcessPushConstants`, `NormalValidationSettings`, `PointLightData` |

### Target Namespace Scheme

```
container::                         (root — ContainerCore, Application)
container::renderer::               (RendererFrontend, FrameRecorder, RenderPassManager, ...)
container::renderer::debug::        (DebugOverlayRenderer, DebugRenderState)
container::gpu::                    (VulkanDevice, VulkanInstance, AllocationManager, VulkanMemoryManager, SwapChainManager)
container::gpu::data::              (CameraData, ObjectData, LightingData, FrameResources, AttachmentImage)
container::scene::                  (SceneManager, SceneGraph, SceneNode, Camera)
container::material::               (MaterialManager, MaterialXIntegration)
container::geometry::               (Vertex, Mesh, Model, Submesh, GltfModelLoader)
container::window::                 (WindowManager, InputManager)
container::ui::                     (GuiManager)
container::math::                   (lookAt, perspectiveRH_ReverseZ, orthoRH_ReverseZ)
container::log::                    (Logger)
```

### Steps

1. **Start with global-scope structs** — these are the most urgent:
   - Move `CameraData`, `ObjectData`, `LightingData`, `PointLightData`, `BindlessPushConstants`, `PostProcessPushConstants`, `NormalValidationSettings` from `SceneData.h` into `container::gpu::data::`.
   - Move `DrawCommand`, `WireframePushConstants`, `NormalValidationPushConstants`, `SurfaceNormalPushConstants` from `DebugOverlayRenderer.h` into `container::renderer::debug::`.
   - Move `AttachmentImage`, `FrameResources` from `FrameResources.h` into `container::gpu::data::`.

2. **Rename existing namespaces** — Batch rename `renderer::` → `container::renderer::`, `utility::vulkan::` → `container::gpu::`, etc.

3. **Add namespace aliases** for migration convenience:
   ```cpp
   // Temporary — remove after full migration
   namespace renderer = container::renderer;
   ```

4. **Update all `#include` consumers** and test files.

5. **Remove aliases** once all files are migrated.

### Validation Criteria

- [x] Zero types in global namespace (excluding `main`)
- [x] All namespaces rooted under `container::`
- [x] Build passes, all tests pass (38/38)

**Phase 3 completed.** All types wrapped in `container::` sub-namespaces. Cross-namespace references qualified or aliased with `using` declarations in `.cpp` files.

---

## 6. Phase 4 — Header Decoupling & Forward Declarations

**Goal:** Reduce transitive include depth; no header includes more than 7 other project headers.  
**Risk:** Low-Medium — mechanical but requires careful testing.  
**Estimated effort:** Medium

### Problem

Several headers pull in enormous transitive include trees:

| Header | Direct Includes | Transitive Cost |
|---|---|---|
| `SceneManager.h` | 14 project headers | Pulls in nearly the entire project |
| `CameraController.h` | 13 project headers | Similar |
| `GraphicsPipelineBuilder.h` | Includes `LightingManager.h` + `SceneManager.h` | Cascading |
| `GuiManager.h` | Includes `SceneManager.h` | Gets 14 transitive headers |
| `LightingManager.h` | Includes `SceneManager.h` | Same |

### Strategy

1. **Forward-declare instead of include** — For each header, identify types used only as pointers, references, or in function signatures:

   ```cpp
   // Before (SceneManager.h)
   #include "Container/utility/AllocationManager.h"
   #include "Container/utility/TextureManager.h"
   #include "Container/renderer/lighting/LightingManager.h"
   // ... 11 more

   // After (SceneManager.h)
   #include "Container/common/CommonVulkan.h"  // VkDescriptorSet etc.
   #include <memory>
   #include <vector>

   namespace container::gpu { class AllocationManager; }
   namespace container::gpu { class TextureManager; }
   namespace container::renderer { class LightingManager; }
   ```

   Move the `#include` directives to the corresponding `.cpp` file.

2. **Extract parameter structs** — Where a function takes many subsystem pointers (e.g., `FrameRecordParams` with 12 fields), keep the struct in its own lightweight header that only forward-declares the pointed-to types.

3. **Apply the "include-what-you-use" rule** — Each `.h` includes only what it directly needs for declarations; each `.cpp` includes what it needs for definitions.

4. **Priority order** (highest impact first):
   1. `SceneManager.h` — reduce from 14 to ≤5 includes
   2. `CameraController.h` — reduce from 13 to ≤5 includes
   3. `GraphicsPipelineBuilder.h` — remove `SceneManager.h` and `LightingManager.h` includes
   4. `GuiManager.h` — remove `SceneManager.h` include
   5. `LightingManager.h` — remove `SceneManager.h` include

### Validation Criteria

- [x] No header includes more than 7 project headers (exception: `RendererFrontend.h` at 11 — requires Phase 5 God Object decomposition to reduce further; all by-value members need complete types)
- [x] Build passes, all tests pass (38/38)
- [ ] Incremental rebuild time measurably reduced (not re-measured after later structural cleanup)

**Phase 4 completed for source structure.** Headers were decoupled via forward declarations;
the only remaining unchecked item is a performance measurement, not an implementation task:

| Header | Before | After |
|---|---|---|
| `SceneManager.h` | 14 | 7 |
| `CameraController.h` | 13 | 2 |
| `GraphicsPipelineBuilder.h` | 7 | 1 |
| `LightingManager.h` | 12 | 5 |
| `SceneController.h` | 12 | 5 |
| `GuiManager.h` | 6 | 3 |
| `FrameRecorder.h` | 7 | 5 |
| `FrameResourceManager.h` | 9 | 3 |
| `RendererFrontend.h` | 18 | 6 |

---

## 7. Phase 5 — Decompose `RendererFrontend` God Object

**Goal:** Transform `RendererFrontend` from a god object into a thin orchestrator.  
**Risk:** High — core rendering logic, must be done incrementally.  
**Estimated effort:** Large

### Problem

`RendererFrontend` currently owns 11 subsystems via `unique_ptr`, holds 8 borrowed references, maintains ~20 state variables, and exposes ~20 methods covering initialization, frame recording, resize handling, and shutdown.

```
Owned (unique_ptr):
  RenderPassManager, OitManager, FrameResourceManager, SceneManager,
  SceneController, CameraController, LightingManager,
  GraphicsPipelineBuilder, FrameRecorder, GuiManager, FrameSyncManager

Borrowed (raw refs):
  VulkanDevice, WindowManager, SwapChainManager, AllocationManager,
  InputManager, VulkanMemoryManager, TextureManager, PipelineManager

State:
  currentFrame, framebufferResized, swapChainExtent, swapChainImageFormat,
  depthFormat, swapChainImageViews, renderPasses, pipelineLayouts,
  graphicsPipelines, descriptorSetLayout, descriptorPool,
  globalDescriptorSets, commandBuffers, debugState, sceneState, ...
```

### Decomposition Strategy

#### Step 5a — Extract `RenderContext` Value Object

Create a `RenderContext` struct that bundles the shared state that multiple subsystems need:

```cpp
struct RenderContext {
    VkDevice          device;
    VkPhysicalDevice  physicalDevice;
    VkExtent2D        extent;
    VkFormat          swapChainFormat;
    VkFormat          depthFormat;
    uint32_t          currentFrame;
    uint32_t          imageIndex;
};
```

Pass `RenderContext` by const-ref to subsystems instead of them reaching back into `RendererFrontend`.

#### Step 5b — Extract `ResourceRegistry`

Move descriptor set layout, descriptor pool, global descriptor sets, and pipeline cache from `RendererFrontend` into a dedicated `ResourceRegistry` that subsystems query.

#### Step 5c — Extract `ResizeCoordinator`

The resize/recreate logic is spread across `RendererFrontend::handleResize()` calling into multiple subsystems. Extract a `ResizeCoordinator` that orchestrates the resize sequence:

```
ResizeCoordinator::onResize()
  → SwapChainManager::recreate()
  → FrameResourceManager::recreate()
  → RenderPassManager::recreate()
  → GraphicsPipelineBuilder::recreate()
  → FrameRecorder::updateParams()
```

#### Step 5d — Simplify `RendererFrontend`

After extractions, `RendererFrontend` becomes:

```cpp
class RendererFrontend {
    RenderContext        context_;
    ResourceRegistry     registry_;
    ResizeCoordinator    resizer_;
    FrameRecorder        recorder_;
    FrameSyncManager     sync_;

public:
    void init(const AppConfig&);
    void drawFrame();
    void shutdown();
};
```

### Validation Criteria

- [x] `RendererFrontend.h` has ≤10 member variables (9 top-level members)
- [x] Each extracted class is independently testable
- [x] No rendering regression — visual output matches pre-refactor
- [x] Build passes, all tests pass (38/38)

### Implementation Summary

Phase 5 took a different approach from the original plan — instead of extracting entirely new classes (ResourceRegistry, ResizeCoordinator), the decomposition grouped related members into nested structs and standalone header-only value objects. This achieved the ≤10 member goal while preserving the existing public API and method structure.

**New header files created:**
| File | Purpose |
|---|---|
| `RenderContext.h` | Lightweight value object bundling Vulkan device/format/extent state |
| `PushConstantBlock.h` | Groups 5 push constant members + `state()` accessor returning `PushConstantState` |
| `RenderResources.h` | Groups render passes, built pipelines, G-buffer formats, frame resources |

**Nested structs in RendererFrontend.h:**
| Struct | Purpose |
|---|---|
| `OwnedSubsystems` | 11 `unique_ptr` subsystems accessed via `subs_` |
| `BorrowedServices` | 8 external refs/ptrs accessed via `svc_` (aggregate-initialized) |
| `SceneBufferState` | Camera/object GPU buffers + capacity + camera data via `buffers_` |
| `FrameState` | `imagesInFlight`, `currentFrame`, `exactOitNodeCapacityFloor` via `frame_` |

**Final 9 top-level members:**
`subs_`, `svc_`, `resources_`, `pushConstants_`, `buffers_`, `sceneGraph_`, `sceneState_`, `debugState_`, `frame_`

**Member reduction:** 31 individual members → 9 grouped members (71% reduction)

---

## 8. Phase 6 — Render Graph Abstraction

**Goal:** Replace hardcoded render pass sequence in `FrameRecorder` with a data-driven render graph.  
**Risk:** High — deep rendering infrastructure change.  
**Estimated effort:** Large  
**Prerequisite:** Phase 5 (RendererFrontend decomposition)

### Problem

`FrameRecorder` hardcodes the pass order:

```
1. Depth Prepass
2. G-Buffer Fill
3. Lighting Pass (directional + point with stencil)
4. Post-Process (OIT resolve + tone mapping)
5. Debug Overlays (wireframe, normals, surface normals, light gizmos)
6. ImGui
```

Adding or reordering passes requires editing `FrameRecorder` directly.

### Design

```cpp
// Render graph node
struct RenderPassNode {
    std::string                    name;
    std::vector<ResourceHandle>    reads;      // attachments read
    std::vector<ResourceHandle>    writes;     // attachments written
    std::function<void(VkCommandBuffer, const RenderContext&)> execute;
};

class RenderGraph {
public:
    RenderPassNode& addPass(std::string name);
    void compile();       // topological sort, barrier insertion
    void execute(VkCommandBuffer cmd, const RenderContext& ctx);
};
```

### Steps

1. [x] **Define `RenderGraph` and `RenderPassNode`** with stable pass IDs, schedule dependencies,
   logical resource reads/writes, execution callbacks, and validation helpers.
2. [x] **Wrap each existing frame pass** as a `RenderPassNode` in `FrameRecorder::buildGraph()`
   without changing the concrete Vulkan recording logic.
3. [x] **Implement `compile()`** with dependency validation, resource-derived edges, cycle
   detection, and topological execution ordering. Vulkan image layout transitions and barriers stay
   in the concrete pass recorders because they still depend on physical render pass boundaries.
4. [x] **Replace command recording order** with `graph.execute()` so `FrameRecorder::record()` only
   begins the command buffer, executes the active graph plan, and ends the command buffer.
5. [x] **Enable dynamic pass toggling** through graph pass state, active-plan pruning, and
   execution-status diagnostics for disabled passes, missing required dependencies/resources, and
   missing record callbacks.
6. [x] **Harden graph mutation APIs** so pass IDs, schedule dependencies, resource access lists,
   duplicate metadata, and failed mutations are validated before the graph state changes.

### Validation Criteria

- [x] Passes execute in dependency-correct order
- [x] Adding a new pass requires no changes to orchestrator code
- [x] No rendering regression
- [x] Build passes, all tests pass

### Implementation Summary (Completed)

**Approach:** Dependency-aware render graph scheduling without automatic barrier insertion.
Passes expose stable IDs plus scheduling dependencies; `RenderGraph::compile()` validates missing
dependencies, rejects cycles, records logical producer-to-consumer resource edges, and folds those
resource edges into the final topological execution order. At execution time, the graph derives an
active pass plan from current pass toggles and required resource availability, so disabling a writer
automatically removes consumers that require its output while preserving consumers that only have
optional reads. Vulkan layout transitions and resource barriers still live in the concrete pass
recorders because attachment lifetimes remain tied to physical Vulkan render pass boundaries.

**New files:**
- `include/Container/renderer/core/RenderGraph.h` — `RenderPassId`, pass name/dependency helpers,
  `RenderResourceId`, resource name/access helpers, `RenderPassSkipReason`,
  `RenderPassExecutionStatus`, `RenderPassNode` (stable ID, name, schedule dependencies,
  required/optional reads, writes, enabled flag, `RecordFn` callback), and `RenderGraph`
  (ID-only addPass, compile, execute, setPassEnabled, setPassRecord,
  setPassScheduleDependencies, setPassResourceAccess, findPass, clear, passCount,
  enabledPassCount, executionPassIds, activeExecutionPassIds, executionStatus, isPassActive,
  passes, resourceEdges). `passes()` and
  `findPass()` expose read-only inspection; mutation goes
  through invalidating setters. Forward-declares `VkCommandBuffer_T*` to avoid Vulkan header
  dependency.
- `src/renderer/core/RenderGraph.cpp` — Stable pass metadata, required-enable dependencies for
  optional features, schedule dependencies for topological ordering, logical resource read/write
  metadata, cycle/missing-dependency validation, resource producer tracking, resource-derived
  scheduling dependencies, invalidating mutation APIs for pass toggles/recorders/dependencies/
  resource access, active pass derivation from current toggles/resource availability, cached
  active-plan invalidation, per-pass execution status/skip reason reporting, and execution through
  the active compiled order. Public execution-order inspection uses stable pass IDs; raw internal
  pass indices stay private to the graph implementation. Pass registration rejects
  `RenderPassId::Invalid`, schedule dependency declarations reject invalid pass IDs, resource
  access mutation rejects invalid resource IDs before changing pass metadata, duplicate dependency
  and resource declarations are rejected, and pass names are display metadata rather than graph
  identity.

**Modified files:**
- `include/Container/renderer/core/FrameRecorder.h` — Added `RenderGraph graph_` member, `buildGraph()`
  method, and `graph()` accessors.
- `src/renderer/core/FrameRecorder.cpp` — Constructor calls `buildGraph()` which registers the full
  frame schedule by `RenderPassId` and compiles the graph. `record()` is now a thin wrapper:
  begin command buffer → `graph_.execute()` → end command buffer. All private helper methods
  preserved unchanged. Feature-dependent subpaths now query graph active state rather than raw
  toggle state, so they do not consume outputs from passes pruned by the active plan.
- `src/renderer/core/RendererFrontend.cpp` — Render pass UI now consumes graph execution statuses to
  show inactive-pass notes for missing required passes/resources or missing record callbacks while
  preserving the existing protected-pass and optional dependency toggle policy. Frame descriptor
  feature flags also use graph active state.
- `tests/render_graph_tests.cpp` — Scheduler/resource coverage for dependency ordering, disabled
  optional dependencies, protected schedule-only dependencies, missing dependencies, cycles, resource
  producer edges, missing required resource writers, optional resource reads, resource-only
  scheduling, resource dependency cycles, active-plan pruning for disabled writers, optional-read
  active-plan preservation, execution status reporting, active-pass predicates, cached active-plan
  invalidation, dependency/resource mutation invalidation, pass-ID execution views, ID-only
  registration validation, invalid schedule dependency rejection, invalid resource access
  rejection, duplicate metadata rejection, and the default frame-flow ordering.
- `src/CMakeLists.txt` — Added `renderer/core/RenderGraph.cpp` to VulkanSceneRenderer_renderer.

**Validation:** `render_graph_tests`, full configured CTest set, and `VulkanSceneRenderer` build
passed after the render graph status update.

---

## 9. Phase 7 — ECS Integration (EnTT)

**Goal:** Use EnTT for scene entity management instead of manual scene graph traversal.  
**Risk:** Medium-High — changes how scene data flows into rendering.  
**Estimated effort:** Large  
**Prerequisite:** Phase 3 (namespace consolidation), Phase 5 (RendererFrontend decomposition)

### Problem

EnTT is already a vcpkg dependency but is completely unused. The project uses a custom `SceneGraph` with `SceneNode` objects and manual traversal in `SceneController` for draw call generation.

### Design

Define components:

```cpp
struct TransformComponent   { glm::mat4 localTransform; glm::mat4 worldTransform; };
struct MeshComponent        { uint32_t primitiveIndex; };
struct MaterialComponent    { uint32_t materialIndex; };
struct LightComponent       { PointLightData data; };
struct CameraComponent      { CameraData data; float nearPlane; float farPlane; };
struct RenderableTag         {};  // marks entities for draw-call extraction
```

### Steps

1. **Create `container::ecs::World`** wrapper around `entt::registry`.
2. **Port `SceneGraph` data into ECS** — each `SceneNode` becomes an entity with components.
3. **Replace `SceneController::buildDrawCommands()`** with an ECS view query:
   ```cpp
   auto view = registry.view<TransformComponent, MeshComponent, MaterialComponent, RenderableTag>();
   for (auto [entity, transform, mesh, material] : view.each()) { ... }
   ```
4. **Port camera and light management** to component queries.
5. **Keep `SceneGraph` as an optional organizational overlay** for glTF hierarchy, or remove it if ECS parent-child relationships suffice.

### Validation Criteria

- [x] Scene loads identically from glTF
- [x] Draw call generation uses ECS views
- [x] EnTT `#include` appears in ≤3 files (1 direct: `World.h`)
- [x] Build passes, all tests pass

### Completion Summary (Phase 7)

**ECS components** (`include/Container/ecs/Components.h`):
- `TransformComponent` (localTransform, worldTransform)
- `MeshComponent` (primitiveIndex)
- `MaterialComponent` (materialIndex)
- `RenderableTag` (tag for draw-call extraction)
- `SceneNodeRef` (back-reference to SceneGraph node index)
- `LightComponent` + `LightTag` with `PointLightData` for scene light publication/query
- `CameraComponent` + `CameraTag` with `CameraData` and clip planes for active-camera publication

**ECS World** (`include/Container/ecs/World.h`, `src/ecs/World.cpp`):
- Wraps `entt::registry` with typed helpers
- Owns the ECS representation for renderables, point lights, and the active camera
- `syncFromSceneGraph()` refreshes renderable entities from SceneGraph while preserving light and camera entities
- `setActiveCamera()` stores the current active camera as ECS `CameraData` plus near/far planes
- `replacePointLights()` stores scene point lights as ECS `PointLightData`
- `forEachRenderable()` and `forEachPointLight()` provide cache-friendly EnTT view iteration
- `renderableCount()`, `pointLightCount()`, `entityCount()`, `activeCamera()`, `clear()`, `registry()` accessors

**SceneController integration** (`src/renderer/scene/SceneController.cpp`):
- Owns the shared `std::unique_ptr<World> world_` constructed in constructor
- `syncObjectDataFromSceneGraph()` rewritten: calls `world_->syncFromSceneGraph()` then
  uses `world_->forEachRenderable()` for ObjectData + DrawCommand generation
- Diagnostic cube logic preserved outside ECS loop
- SceneGraph retained as the authoritative data source; ECS mirrors it for rendering

**Camera and lighting integration**:
- `CameraController` publishes the active `CameraData` and clip planes into the shared ECS World.
- `LightingManager` publishes and queries `PointLightData` through the shared ECS World.
- `LightingManager` reads the active `CameraComponent` for camera-relative lights and light-gizmo sizing,
  while preserving the existing camera-data path as a fallback.
- `RendererFrontend::buildFrameRecordParams()` prefers ECS camera clip planes for per-frame camera near/far values.

**Build system** (`src/ecs/CMakeLists.txt`):
- `VulkanSceneRenderer_ecs` library linking `Dep_ECS`, `Dep_Math`, `VulkanSceneRenderer_scene`
- Linked to `VulkanSceneRenderer_renderer`

**EnTT include footprint:** direct EnTT inclusion remains isolated to `World.h`, keeping the
rest of renderer/scene code on the project ECS facade.

**Tests:** ECS tests (`tests/ecs_tests.cpp`) cover component defaults, World creation,
syncFromSceneGraph (empty graph, renderable filtering, transform preservation, mesh/material
index preservation, re-sync clears previous renderables), point-light replacement, active-camera
updates, preservation of light/camera entities across renderable sync, forEachRenderable
(empty, visit count), and clear.

---

## 10. Phase 8 — Dependency Audit & Cleanup

**Goal:** Remove unused dependencies, reduce vcpkg footprint.  
**Risk:** Low — removal only.  
**Estimated effort:** Small

### Suspected Unused Dependencies

| Dependency | Evidence | Action |
|---|---|---|
| **Eigen3** | No `#include <Eigen/...>` found in any source file | **Remove** from `vcpkg.json` and `Dependencies.cmake` |
| **assimp** | Project uses `tinygltf` for model loading; no assimp includes found | **Remove** unless planned for FBX/OBJ support |
| **cxxopts** | No command-line parsing in `main.cpp` or `Application` | **Remove** or implement CLI arg support |
| **libzip** | No `#include <zip.h>` found | **Remove** |
| **VulkanUtilityLibraries** | Verify if `VUL::` types are used or only Vulkan SDK suffices | **Audit** — keep if used, remove if not |

### Steps

1. **For each suspect**, grep the entire source tree for any usage.
2. **Remove confirmed unused** entries from `vcpkg.json` and `Dependencies.cmake`.
3. **Verify:** `vcpkg install` succeeds, build passes, all tests pass.

### Validation Criteria

- [x] `vcpkg.json` contains only actively-used dependencies
- [x] Build passes, all tests pass
- [x] Reduced vcpkg install/restore time

### Completion Summary (Phase 8)

**Removed from `vcpkg.json`** (7 packages):
- `eigen3` — no `#include <Eigen/...>` found
- `assimp` — project uses tinygltf for model loading; no assimp includes
- `cxxopts` — no command-line parsing code
- `libzip` — no `#include <zip.h>` found
- `yaml-cpp` — no YAML parsing code
- `libpng` — stb_image handles PNG decoding internally
- `libjpeg-turbo` — stb_image handles JPEG decoding internally
- `vulkan-utility-libraries` — no VUL:: types or utility headers used in source

**Remaining `vcpkg.json`** (11 packages): gtest, fmt, nlohmann-json, glm, glfw3,
imgui (glfw+vulkan), vulkan-memory-allocator, shader-slang, tinygltf, stb, spdlog,
entt, materialx.

**CMake changes** (`cmake/Dependencies.cmake`):
- `REQUIRED_PACKAGES` reduced from 18 to 10 (removed VulkanUtilityLibraries, assimp,
  cxxopts, PNG, libzip, yaml-cpp, Eigen3, libjpeg-turbo)
- `Dep_VulkanCore`: Removed `Vulkan::SafeStruct`, `Vulkan::LayerSettings`,
  `Vulkan::UtilityHeaders`, `Vulkan::CompilerConfiguration`. Added explicit
  `NOMINMAX` + `WIN32_LEAN_AND_MEAN` definitions (previously provided by
  `Vulkan::CompilerConfiguration`).
- `Dep_Serialization`: Removed entirely (no consumers)
- `Dep_SceneIO`: Removed `assimp::assimp`, `PNG::PNG`, `libzip::zip`,
  `libjpeg-turbo::turbojpeg`. Now links only tinygltf + stb.
- `Dep_ECS`: Removed `Eigen3::Eigen`, `cxxopts::cxxopts`. Now links only `EnTT::EnTT`.
- `VulkanDependencies` legacy aggregate: Removed entirely (no consumers).

**Validation:** Dependency cleanup completed and verified at the time of this phase.

---

## 11. Phase 9 — Test Infrastructure Improvements

**Goal:** Tests link minimal dependencies; add integration and GPU tests.  
**Risk:** Low.  
**Estimated effort:** Small-Medium

### Current Issues

1. `add_custom_test()` macro in `tests/CMakeLists.tests.cmake` links `VulkanDependencies` (all 30 libs) to every test.
2. Only a small non-GPU test set exists.
3. No integration tests for subsystem initialization.

### Steps

1. **Split test dependency linking** (enabled by Phase 1):
   - CPU-only tests: link `Dep_Math` + `GTest::gtest_main`
   - GPU tests: link `Dep_VulkanCore` + subsystem libraries + `GTest::gtest_main`

2. **Add parameterized tests** for pipeline configurations (blend modes, depth states).

3. **Add headless integration tests** (requires Vulkan device but no window):
   - `VulkanDevice` creation/destruction
   - Buffer allocation/deallocation via `AllocationManager`
   - Shader compilation (Slang → SPIR-V)

4. **Add render-output validation tests** (capture framebuffer → compare against reference images).

### Validation Criteria

- [x] Lightweight unit tests compile quickly
- [ ] GPU integration tests run in CI with `VK_ICD` (e.g., lavapipe/swiftshader)
- [ ] ≥80% of public API surface covered by tests

### Completion Summary (Phase 9 — Partial)

**`add_custom_test()` refactored** (`tests/CMakeLists.tests.cmake`):
- Shader copy step is now optional — pass `""` for SHADER_DIR to skip.
- Removed `GTest::gmock` / `GTest::gmock_main` from default linking (unused).
- All 8 test targets now use the unified `add_custom_test()` function (eliminated
  3 manual `add_executable` / `set_target_properties` / `add_test` blocks).
- CPU-only tests no longer copy shaders.

**SceneGraph unit tests** (`tests/scene_graph_tests.cpp`) — 20 new tests:
- SceneNode defaults (identity transforms, invalid indices)
- createNode (count, sequential indices, transform/material preservation, renderable
  registration)
- setRenderable (register, unregister, idempotent)
- Parent/child (setParent, detach via nullopt, reparent moves child)
- Transform propagation (setLocalTransform, updateWorldTransforms for root, 2-level,
  3-level hierarchies)
- getNode bounds (out-of-range returns null, const/mutable agreement)

**Appendix A** updated to reflect current library structure and Dep_* groups.

**Validation:** Test infrastructure changes were verified at the time of this phase.

*Remaining work (future):* GPU integration tests (headless VulkanDevice, buffer
allocation, shader compilation), parameterized pipeline tests, render-output
validation tests.

---

## 12. Phase 10 — Structural Cleanup

**Goal:** Remove dead build targets, trim redundant link dependencies, extract
lightweight headers to reduce transitive include weight.  
**Risk:** Low — removal and reorganisation only.  
**Estimated effort:** Small

### Changes

**1. Removed `VulkanSceneRenderer_utility` aggregate** (`src/utility/CMakeLists.txt`):
- INTERFACE library that grouped all 6 utility sub-libraries.
- Zero consumers found via grep — all targets link specific sub-libraries directly.
- Deleted the `add_library(VulkanSceneRenderer_utility INTERFACE)` block.

**2. Trimmed `VulkanSceneRenderer_Core` link list** (`src/CMakeLists.txt`):
- Was: `VulkanSceneRenderer_renderer`, `VulkanSceneRenderer_geometry`, `VulkanSceneRenderer_log`,
  `VulkanSceneRenderer_vulkan_device`, `VulkanSceneRenderer_gpu_resource`,
  `VulkanSceneRenderer_window`, `VulkanSceneRenderer_scene`, `VulkanSceneRenderer_ui` (8 deps)
- Now: `VulkanSceneRenderer_renderer`, `VulkanSceneRenderer_geometry` (2 deps)
- Renderer transitively provides all utility sub-libraries.

**3. Removed `Dep_Shader`** (`cmake/Dependencies.cmake`, `src/CMakeLists.txt`):
- `slang::slang` was linked to `VulkanSceneRenderer_renderer` but no source file
  includes any slang header. The `slangc` compiler is used only as an external
  CLI tool via `find_program` in `Shaders.cmake`.
- Removed `find_package(Slang REQUIRED)` and the `Dep_Shader` INTERFACE target.
- Deleted `cmake/FindSlang.cmake` (custom find module no longer needed).
- Kept `shader-slang` in `vcpkg.json` (provides the `slangc` tool).

**4. Extracted `PipelineTypes.h`** (`include/Container/renderer/pipeline/PipelineTypes.h`):
- Moved 5 POD structs (`PipelineLayouts`, `GraphicsPipelines`,
  `PipelineDescriptorLayouts`, `PipelineRenderPasses`, `PipelineBuildResult`)
  from `GraphicsPipelineBuilder.h` into a new lightweight header.
- `PipelineTypes.h` includes only `CommonVulkan.h` — no STL headers.
- `GraphicsPipelineBuilder.h` now includes `PipelineTypes.h` instead of
  defining the structs inline.
- `FrameRecorder.h` and `RenderResources.h` now include `PipelineTypes.h`
  instead of the heavier `GraphicsPipelineBuilder.h` (saves transitive
  `<filesystem>`, `<memory>`, `<string>` includes).
- `RendererFrontend.h` gained a forward declaration of `GraphicsPipelineBuilder`
  (used only as `std::unique_ptr` member, no full definition needed).
- `RendererFrontend.cpp` now explicitly includes `GraphicsPipelineBuilder.h`
  (needed for `std::make_unique`).

### Validation Criteria

- [x] Build passes, all tests pass
- [x] No headers exceed 7 project includes
- [x] Zero dead INTERFACE targets remain
- [x] All Dep_* groups actively used

**Test results:** 8 test suites, 78+ tests discovered, all passing.

---

## Appendix A — Current Dependency Graph

```
┌──────────────────────────┐
│   VulkanSceneRenderer (exe)  │
└───────────┬──────────────┘
            │
┌───────────▼──────────────┐
│  VulkanSceneRenderer_Core    │
│  (ContainerCore.cpp,     │
│   app/Application.cpp)   │
└─┬──────────┬─────────────┘
  │          │
  ▼          ▼
renderer   geometry ─── Dep_VulkanCore, Dep_Math, Dep_SceneIO
  │
  ├── VulkanSceneRenderer_ecs ──── Dep_ECS (EnTT)
  │     └── VulkanSceneRenderer_scene
  │
  ├── VulkanSceneRenderer_scene ── Dep_SceneIO, Dep_Material (MaterialX)
  │     └── VulkanSceneRenderer_gpu_resource
  │           └── VulkanSceneRenderer_vulkan_device
  │
  ├── VulkanSceneRenderer_ui ───── Dep_UI (ImGui)
  ├── VulkanSceneRenderer_window ─ Dep_Windowing (GLFW)
  └── VulkanSceneRenderer_log ──── Dep_Logging (spdlog, fmt)

Dep_* groups (8 INTERFACE targets)
──────────────────────────────────
  Dep_VulkanCore : Vulkan::Vulkan, Vulkan::Headers, VMA, NOMINMAX, WIN32_LEAN_AND_MEAN
  Dep_Math       : glm (column-major, depth 0-1, radians)
  Dep_Windowing  : glfw
  Dep_UI         : imgui::imgui
  Dep_Logging    : spdlog::spdlog, fmt::fmt
  Dep_SceneIO    : tinygltf, stb
  Dep_Material   : MaterialXCore, MaterialXFormat, MaterialXGenShader
  Dep_ECS        : EnTT::EnTT

Removed targets: VulkanDependencies, Dep_Serialization, Dep_Shader,
                 VulkanSceneRenderer_utility (aggregate)
```

---

## Appendix B — Namespace Inventory

### Current (Pre-Phase 3)

```
renderer::                    — RendererFrontend, FrameRecorder, RenderPasses, ...
utility::vulkan::             — VulkanDevice, VulkanInstance
utility::memory::             — AllocationManager
utility::scene::              — SceneManager, SceneGraph
utility::camera::             — BaseCamera, PerspectiveCamera
utility::pipeline::           — PipelineManager
utility::window::             — WindowManager
utility::ui::                 — GuiManager
utility::material::           — MaterialXIntegration, MaterialManager
utility::input::              — InputManager
utility::logger::             — Logger
Container::                   — ContainerCore
app::                         — Application
common::math::                — lookAt, perspectiveRH_ReverseZ, orthoRH_ReverseZ
(global)                      — CameraData, ObjectData, LightingData, DrawCommand,
                                FrameResources, AttachmentImage, push constant structs
```

### Target (Post-Phase 3)

```
container::                   — ContainerCore, Application, AppConfig
container::renderer::         — RendererFrontend, FrameRecorder, RenderPassManager, ...
container::renderer::debug::  — DebugOverlayRenderer, DebugRenderState
container::gpu::              — VulkanDevice, VulkanInstance, AllocationManager, ...
container::gpu::data::        — CameraData, ObjectData, LightingData, FrameResources, ...
container::scene::            — SceneManager, SceneGraph, SceneNode, Camera
container::geometry::         — Vertex, Mesh, Model, Submesh, GltfModelLoader
container::material::         — MaterialManager, MaterialXIntegration
container::window::           — WindowManager, InputManager
container::ui::               — GuiManager
container::math::             — lookAt, perspectiveRH_ReverseZ, orthoRH_ReverseZ
container::log::              — Logger
```

---

## Phase Execution Order

```
Phase 1 (Dep Split) ──→ Phase 2 (Utility Split) ──→ Phase 3 (Namespaces) ──┐
                                                                             │
Phase 4 (Headers) ◄─────────────────────────────────────────────────────────┘
    │
    ▼
Phase 5 (RendererFrontend) ──→ Phase 6 (Render Graph)
    │
    ▼
Phase 7 (ECS / EnTT)

Phase 8 (Dep Audit)  ← can run anytime after Phase 1
Phase 9 (Tests)       ← can run anytime after Phase 1
Phase 10 (Cleanup)    ← can run anytime after Phase 8

Implementation phases complete. Phase 9 remains partial: GPU CI and coverage targets are deferred.
```

---

*Last updated: Implementation phases 0-8 and 10 complete; Phase 9 partial.*
