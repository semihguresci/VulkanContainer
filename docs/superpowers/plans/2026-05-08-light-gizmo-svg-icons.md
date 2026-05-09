# Light Gizmo SVG Icons Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render light gizmos as distinct SVG-derived scene icons for directional, point, spot, and area lights.

**Architecture:** Store four SVG files in `materials/gizmos/lights`, rasterize them into a four-layer texture array at startup, and bind that atlas through a dedicated light-gizmo descriptor set. The existing planner keeps one push constant per gizmo, with `coneOuterCosType.x` carrying the icon layer while the shader draws a camera-facing textured quad.

**Tech Stack:** C++23, Vulkan descriptor sets and 2D texture arrays, Slang shaders, GTest source and unit tests.

---

### Task 1: Tests And Asset Contract

**Files:**
- Create: `tests/renderer/deferred/light_gizmo_icon_atlas_tests.cpp`
- Modify: `tests/CMakeLists.tests.cmake`
- Modify: `tests/renderer/deferred/deferred_light_gizmo_planner_tests.cpp`
- Modify: `tests/validation/rendering_convention_tests.cpp`

- [x] **Step 1: Write failing tests**

Add tests for stable icon layer mapping, expected SVG asset filenames, SVG rasterization alpha output, atlas layer ordering, planner push-constant icon layers, and source contracts for shader/pipeline/descriptor wiring.

- [x] **Step 2: Run the focused red build**

Run:

```powershell
& cmd.exe /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out/build/windows-debug --target light_gizmo_icon_atlas_tests deferred_light_gizmo_planner_tests rendering_convention_tests'
```

Expected: fail because `LightGizmoIconAtlas.h` and the new SVG/descriptor contracts do not exist yet.

### Task 2: SVG Assets And CPU Atlas

**Files:**
- Create: `materials/gizmos/lights/directional.svg`
- Create: `materials/gizmos/lights/point.svg`
- Create: `materials/gizmos/lights/spot.svg`
- Create: `materials/gizmos/lights/area.svg`
- Create: `include/Container/renderer/lighting/LightGizmoIconAtlas.h`
- Create: `src/renderer/lighting/LightGizmoIconAtlas.cpp`
- Modify: `src/CMakeLists.txt`

- [x] **Step 1: Add the four SVG files**

Use simple SVG shapes (`circle`, `rect`, `line`, `polygon`) so the in-repo rasterizer can load the files without pulling a new dependency.

- [x] **Step 2: Implement the atlas helper**

Expose:

```cpp
uint32_t lightGizmoIconLayerForType(EditableLightType type);
std::array<std::filesystem::path, kLightGizmoIconLayerCount>
lightGizmoIconAssetPaths(const std::filesystem::path &assetRoot);
RasterizedLightGizmoIcon rasterizeLightGizmoSvg(std::string_view svg,
                                                uint32_t size);
std::vector<std::byte> buildLightGizmoIconAtlasRgba(
    std::span<const RasterizedLightGizmoIcon, kLightGizmoIconLayerCount> icons);
std::vector<std::byte> loadLightGizmoIconAtlasRgba(
    const std::filesystem::path &assetRoot, uint32_t size);
```

- [x] **Step 3: Run icon atlas tests**

Run:

```powershell
& cmd.exe /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out/build/windows-debug --target light_gizmo_icon_atlas_tests && ctest --test-dir out/build/windows-debug -R light_gizmo_icon_atlas_tests --output-on-failure'
```

Expected: pass.

### Task 3: Vulkan Texture Array And Descriptor Wiring

**Files:**
- Modify: `include/Container/utility/TextureResource.h`
- Modify: `include/Container/utility/AllocationManager.h`
- Modify: `src/utility/AllocationManager.cpp`
- Modify: `include/Container/renderer/lighting/LightingManager.h`
- Modify: `src/renderer/lighting/LightingManager.cpp`
- Modify: `include/Container/renderer/pipeline/PipelineTypes.h`
- Modify: `src/renderer/pipeline/GraphicsPipelineBuilder.cpp`
- Modify: `src/renderer/pipeline/PipelineRegistry.cpp`
- Modify: `src/renderer/core/RendererFrontend.cpp`
- Modify: `include/Container/renderer/deferred/DeferredRasterPipelineBridge.h`
- Modify: `src/renderer/deferred/DeferredRasterLightingPassRecorder.cpp`

- [x] **Step 1: Add texture-array upload support**

Add a `TextureArrayResource` and `AllocationManager::createTexture2DArrayFromRgbaPixels` that uploads all layers into a `VK_IMAGE_VIEW_TYPE_2D_ARRAY` sampled image.

- [x] **Step 2: Add light gizmo icon descriptors**

Create a dedicated descriptor set layout with binding `0` as `VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE` for the texture array and binding `1` as `VK_DESCRIPTOR_TYPE_SAMPLER`. Load `materials/gizmos/lights` in `LightingManager::createTiledResources`, write both descriptors, and expose `lightGizmoIconDescriptorSetLayout()` for pipeline layout creation.

- [x] **Step 3: Add a dedicated pipeline layout**

Register layout name `light-gizmo`, use descriptor sets `{frame lighting, light gizmo icons}`, and route light-gizmo recording through `DeferredRasterPipelineLayoutId::LightGizmo`.

### Task 4: Textured Billboard Shader

**Files:**
- Modify: `shaders/light_gizmo.slang`
- Modify: `src/renderer/deferred/DeferredLightGizmoPlanner.cpp`
- Modify: `src/renderer/deferred/DeferredLightGizmoRecorder.cpp`
- Modify: `tests/renderer/deferred/deferred_light_gizmo_recorder_tests.cpp`

- [x] **Step 1: Encode the icon layer**

Set `coneOuterCosType.x` from `lightGizmoIconLayerForType` for every planned gizmo.

- [x] **Step 2: Draw camera-facing quads**

Replace the six line vertices with two triangles, sample `uLightGizmoIcons`, tint by `pc.colorIntensity`, and preserve alpha blending.

- [x] **Step 3: Keep recorder validation focused**

Keep two descriptor sets required: frame lighting plus icon atlas.

### Task 5: Verification

**Files:**
- All changed files.

- [ ] **Step 1: Build focused targets**

Run:

```powershell
& cmd.exe /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build out/build/windows-debug --target light_gizmo_icon_atlas_tests deferred_light_gizmo_planner_tests deferred_light_gizmo_recorder_tests rendering_convention_tests'
```

- [ ] **Step 2: Run focused tests**

Run:

```powershell
& cmd.exe /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && ctest --test-dir out/build/windows-debug -R "light_gizmo_icon_atlas_tests|deferred_light_gizmo_planner_tests|deferred_light_gizmo_recorder_tests|rendering_convention_tests" --output-on-failure'
```

- [x] **Step 3: Check whitespace**

Run:

```powershell
git diff --check
```
