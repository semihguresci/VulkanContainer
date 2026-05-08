# VulkanSceneRenderer — Lighting System Improvement Plan

> **Project:** VulkanSceneRenderer
> **Branch:** `main`  
> **Date:** 2025  
> **Status:** Living document — Phase 1 ✅ Phase 2 ✅ Phase 3 ✅ (+ cascade blend & debug view) Phase 4 ✅ Phase 5 ✅ Phase 6 ✅ Bloom ✅
> **Related:** [`docs/refactoring-plan.md`](refactoring-plan.md) (architecture refactoring)

---

## Reader Notes

This document records the renderer feature plan and the rationale behind the
current lighting pipeline. Some early audit rows are intentionally historical;
when behavior differs, trust the implementation and the phase-completion notes
later in the file.

The active frame-lighting path is:

```text
Depth prepass
  -> Hi-Z / occlusion cull
  -> G-buffer
  -> shadow cascades
  -> tile light cull
  -> GTAO
  -> deferred lighting + transparent OIT
  -> bloom
  -> post-process
```

Coordinate, reverse-Z, viewport, and winding rules are defined in
[`coordinate-conventions.md`](coordinate-conventions.md). Shader comments should
point back to those rules instead of restating a conflicting local convention.

## Table of Contents

1. [Current State Audit](#1-current-state-audit)
2. [Gap Analysis](#2-gap-analysis)
3. [Phase 1 — Extract Shared BRDF Include](#3-phase-1--extract-shared-brdf-include)
4. [Phase 2 — G-Buffer Optimization](#4-phase-2--g-buffer-optimization)
5. [Phase 3 — Cascaded Shadow Maps](#5-phase-3--cascaded-shadow-maps)
6. [Phase 4 — Clustered / Tiled Light Culling](#6-phase-4--clustered--tiled-light-culling)
7. [Phase 5 — Image-Based Lighting & SSAO](#7-phase-5--image-based-lighting--ssao)
8. [Phase 6 — GPU-Driven Rendering](#8-phase-6--gpu-driven-rendering)
9. [Bloom Post-Processing](#9-bloom-post-processing)
10. [Appendix A — File Inventory](#appendix-a--file-inventory)
11. [Appendix B — Reference Material](#appendix-b--reference-material)

---

## 1. Current State Audit

### Pipeline Overview

```
Depth Prepass → G-Buffer Fill (4 MRTs) → OIT Clear
  → Shadow Cascade 0..3 (depth-only, directional light CSM)
  → Tile Cull (compute, bins lights into 16×16 tiles)
  → GTAO (compute, half-res AO) → GTAO Blur (compute, bilateral denoise)
  → Directional Light (fullscreen quad, CSM shadows + IBL ambient + SSAO)
  → Point Lights (tiled deferred, stencil fallback) → OIT Transparent
  → Bloom (compute, dual-filter downsample/upsample mip chain)
  → Post-Process (OIT Resolve + Bloom Composite + Tone Map) → Debug Overlays → ImGui
```

### G-Buffer Layout

| Attachment | Format | Content |
|---|---|---|
| RT0 — Albedo | `R8G8B8A8_UNORM` | Base color + alpha |
| RT1 — Normal | `R16G16B16A16_SFLOAT` | World-space normal (encoded `xyz * 0.5 + 0.5`) |
| RT2 — Material | `R16G16B16A16_SFLOAT` | Metallic (R), Roughness (G), Occlusion (B) |
| RT3 — Emissive | `R16G16B16A16_SFLOAT` | Emissive RGB |
| Depth/Stencil | D32_S8 | Reverse-Z depth + stencil for light volumes (sampled in lighting pass via `depthSamplingView`) |

**Total G-Buffer bandwidth per pixel:** ~40 bytes (1 × RGBA8 + 3 × RGBA16F + D32_S8).  
**Note:** Position is reconstructed from depth via `ReconstructWorldPosition()` in `brdf_common.slang` (Phase 2).

### Lighting Architecture

| Component | Current Implementation |
|---|---|
| **Directional light** | 1 hardcoded sun; fullscreen triangle; Cook-Torrance GGX in `deferred_directional.slang`; shadow sampling via CSM (Phase 3, in progress) |
| **Point lights** | `kMaxDeferredPointLights = 4`; hardcoded positions in `LightingManager::updateLightingData()`; per-light stencil-mark cube + fullscreen-triangle shading |
| **Light data transport** | Single `LightingData` uniform buffer (~144 bytes); CPU-written via `SceneController::writeToBuffer()` each frame |
| **Shadows** | CSM — 4-cascade, 2048², D32_S8 array texture, 3×3 PCF, comparison sampler (Phase 3 ✅) |
| **Ambient** | IBL split-sum (irradiance cubemap + pre-filtered specular + BRDF LUT) × GTAO in `deferred_directional.slang` (Phase 5 ✅) |
| **IBL / Probes** | Placeholder white cubemaps (1×1); BRDF LUT generated at startup; framework ready for HDR environment loading (Phase 5 ✅) |
| **SSAO** | GTAO compute pass (half-res R8) + bilateral blur; dispatched per-frame between G-Buffer and Lighting (Phase 5 ✅) |
| **Frustum culling** | **None** (all objects drawn unconditionally) |
| **Occlusion culling** | **None** |
| **PBR BRDF** | Cook-Torrance GGX (D), Smith (G), Schlick (F) — shared via `brdf_common.slang` include (Phase 1) |

### Point Light Rendering Cost (Per Light)

The point light loop now reached from
`DeferredRasterLightingPassRecorder::record()` performs per light:

1. `vkCmdClearAttachments` — stencil clear
2. `vkCmdBindPipeline` — stencil volume pipeline
3. `vkCmdBindDescriptorSets` — lighting descriptors
4. `vkCmdPushConstants` — light push constants
5. `vkCmdDraw(36)` — stencil cube (36 vertices)
6. `vkCmdBindPipeline` — point light shading pipeline
7. `vkCmdBindDescriptorSets` — lighting descriptors (redundant rebind)
8. `vkCmdPushConstants` — light push constants (duplicate)
9. `vkCmdDraw(3)` — fullscreen triangle

**At 4 lights:** 8 draw calls + 4 stencil clears = 12 GPU commands.  
**At 1000 lights:** 2000 draw calls + 1000 stencil clears = **3000 GPU commands** — completely command-buffer-bound.

---

## 2. Gap Analysis

### Scalability Gaps

| Gap | Severity | Impact |
|---|---|---|
| Hard cap at 4 point lights (`kMaxDeferredPointLights`) | **Critical** | Cannot exceed 4 lights without changing UBO layout, shader, and C++ struct |
| Per-light draw call loop (stencil mark + shade) | **Critical** | O(N) pipeline binds and draw calls; unusable beyond ~32 lights |
| No light culling | **High** | Every light is evaluated for every pixel inside its stencil volume |
| No frustum culling for geometry | **High** | All meshes drawn regardless of camera visibility |
| No indirect/multi-draw batching | **Medium** | One draw call per mesh per pass |

### Quality Gaps

| Gap | Severity | Impact | Status |
|---|---|---|---|
| No shadows at all | **Critical** | Scene has no grounding, objects float, no depth cues | ✅ Phase 3 complete |
| ~~Flat ambient (`0.03`)~~ | ~~**High**~~ | ~~Materials look flat and unlit in indirect light; no environment reflections~~ | ✅ Phase 5 complete |
| ~~No SSAO~~ | ~~**Medium**~~ | ~~Contact shadows and crevice darkening missing~~ | ✅ Phase 5 complete |
| ~~World-position G-Buffer target~~ | ~~**Medium**~~ | ~~Wastes ~16 bytes/pixel bandwidth~~ | ✅ Resolved (Phase 2) |

### Maintenance Gaps

| Gap | Severity | Impact | Status |
|---|---|---|---|
| ~~BRDF code duplicated in 4 shaders~~ | ~~**Medium**~~ | ~~Bug fixes / improvements must be applied 4 times~~ | ✅ Resolved (Phase 1, extended to `forward_transparent.slang`) |
| ~~`LightingData` struct layout shared between C++ and Slang via convention only~~ | ~~**Low**~~ | ~~No compile-time layout verification~~ | ✅ Resolved (static_asserts in `SceneData.h` for `CameraData`, `LightingData`, `PointLightData`, `ShadowCascadeData`, `ShadowData`, `ObjectData`) |

---

## 3. Phase 1 — Extract Shared BRDF Include ✅

**Goal:** Eliminate shader code duplication and establish the pattern for shared shader includes.  
**Risk:** Low — shader-only change, no C++ or pipeline modifications.  
**Status:** ✅ **Complete**

### Implementation Summary

1. **Created `shaders/brdf_common.slang`** — shared BRDF functions (`PI`, `SafeNormalize`, `DistributionGGX`, `GeometrySchlickGGX`, `GeometrySmith`, `FresnelSchlick`) plus `ReconstructWorldPosition()` (moved from `gbuffer_composite.slang`).

2. **Created `shaders/lighting_structs.slang`** — shared struct definitions (`CameraBuffer`, `PointLightData`, `LightingBuffer`, `ShadowCascadeData`, `ShadowBuffer`).

3. **Updated 4 consumer shaders** to `#include` instead of duplicating:
   - `deferred_directional.slang`
   - `point_light.slang`
   - `point_light_stencil_debug.slang`
   - `gbuffer_composite.slang`

4. **Updated `cmake/Shaders.cmake`** — added exclude filters for `brdf_common.slang` and `lighting_structs.slang` (include-only files, not compiled directly).

### Validation Criteria

- [x] No duplicated BRDF functions remain in individual shader files
- [x] All shaders compile without errors
- [x] Rendered output is pixel-identical before and after
- [x] Validation checks pass

---

## 4. Phase 2 — G-Buffer Optimization ✅

**Goal:** Remove the world-position render target and reconstruct position from depth.  
**Risk:** Medium — changes G-Buffer attachment layout, framebuffer creation, and multiple shaders.  
**Status:** ✅ **Complete**  
**Prerequisite:** Phase 1 (shared includes) ✅

### Implementation Summary

Removed RT4 (world-position) from the G-Buffer. All shaders now reconstruct position from depth via `ReconstructWorldPosition()` in `brdf_common.slang`.

**Files modified (7 shaders):**
- `gbuffer.slang` — removed `PSOutput.position` output (5 → 4 MRTs)
- `deferred_directional.slang` — replaced `gPositionTexture` with `Texture2D<float> gDepthTexture` (binding 6), uses `depth == 0.0` as sky test
- `point_light.slang` — same depth reconstruction change
- `point_light_stencil_debug.slang` — same depth reconstruction change
- `forward_transparent.slang` — removed position read
- `gbuffer_composite.slang` — removed position sampling
- `post_process.slang` — removed position sampling

**Files modified (C++ infrastructure):**
- `FrameResources.h` — removed `AttachmentImage position`, added `VkImageView depthSamplingView`
- `FrameResourceManager.h/.cpp` — removed position format, added `VK_IMAGE_USAGE_SAMPLED_BIT` to depth, created depth-only image view for shader sampling, updated framebuffers from 6 → 5 attachments, descriptor writes use depth view
- `RenderPassManager.h/.cpp` — removed `positionFormat` parameter, 6 → 5 attachments, 5 → 4 color refs, lighting depth layout changed to `DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL`
- `RendererFrontend.cpp` — updated to match new render pass/framebuffer sizes
- `FrameRecorder.cpp` — clear values 6 → 5
- `GraphicsPipelineBuilder.cpp` — blend state 5 → 4 color attachments

### Validation Criteria

- [x] G-Buffer uses 4 color attachments + depth instead of 5 + depth
- [x] Position reconstruction matches previous world-position output
- [x] No visual regression in lit scenes
- [x] Validation checks pass

### Achieved G-Buffer Layout

| Attachment | Format | Content |
|---|---|---|
| RT0 — Albedo | `R8G8B8A8_UNORM` | Base color + alpha |
| RT1 — Normal | `R16G16B16A16_SFLOAT` | World-space normal (encoded) |
| RT2 — Material | `R16G16B16A16_SFLOAT` | Metallic (R), Roughness (G), Occlusion (B) |
| RT3 — Emissive | `R16G16B16A16_SFLOAT` | Emissive RGB |
| Depth/Stencil | D32_S8 | Reverse-Z depth (sampled via `depthSamplingView`) + stencil |

---

## 5. Phase 3 — Cascaded Shadow Maps ✅

**Goal:** Add directional light shadows via Cascaded Shadow Maps (CSM).  
**Risk:** High — new render passes, new shaders, new descriptor bindings, changes to lighting shaders.  
**Status:** ✅ **Complete** — all core implementation done, build verified, all 82 tests pass  
**Prerequisite:** Phase 2 (G-Buffer optimization) ✅

### Overview

Cascaded Shadow Maps split the camera frustum into 4 depth slices, each rendered from the directional light's perspective into a shadow atlas (D32_S8 2D array texture, 2048×2048, 4 layers). The lighting pass samples the shadow atlas with a comparison sampler and 3×3 PCF kernel.

### Data Structures ✅

```cpp
// SceneData.h — implemented
inline constexpr uint32_t kShadowCascadeCount = 4;
inline constexpr uint32_t kShadowMapResolution = 2048;

struct ShadowCascadeData {
    alignas(16) glm::mat4 viewProj{1.0f};
    alignas(4)  float     splitDepth{0.0f};
    alignas(4)  float     padding[3]{};
};

struct ShadowData {
    ShadowCascadeData cascades[kShadowCascadeCount];
};

struct ShadowPushConstants {
    uint32_t objectIndex{0};
    uint32_t cascadeIndex{0};
};
```

Matching Slang structs added to `lighting_structs.slang` (`SHADOW_CASCADE_COUNT`, `ShadowCascadeData`, `ShadowBuffer`).

### Implementation Progress

| Step | Status | Details |
|---|---|---|
| `SceneData.h` data structures | ✅ | `kShadowCascadeCount`, `kShadowMapResolution`, `ShadowCascadeData`, `ShadowData`, `ShadowPushConstants` |
| `lighting_structs.slang` structs | ✅ | `SHADOW_CASCADE_COUNT`, `ShadowCascadeData`, `ShadowBuffer` |
| `shadow_common.slang` | ✅ | `SelectCascade()`, `SampleCascadeShadow()` helper, `ComputeShadowFactor()` with 3×3 PCF, normal offset bias, slope-scaled bias, cascade blend overlap zone (10% blend fraction) |
| `shadow_depth.slang` | ✅ | Vertex-only: `ObjectBuffer` SSBO + `ShadowBuffer` UBO + push constants (`objectIndex`, `cascadeIndex`) |
| `deferred_directional.slang` shadow integration | ✅ | Bindings 7–9 (shadow UBO, atlas Texture2DArray, comparison sampler), computes viewDepth, calls `ComputeShadowFactor()`, multiplies `directLighting` |
| `RenderPassManager` shadow render pass | ✅ | D32_S8 depth-only, CLEAR→STORE, UNDEFINED→SHADER_READ_ONLY_OPTIMAL, dependencies for shader read |
| `ShadowManager` class | ✅ | Owns shadow atlas image (2D array, 4 layers), per-cascade views, comparison sampler (`VK_COMPARE_OP_GREATER`), shadow UBO, descriptor set (shadow UBO binding 0), framebuffers; `update()` computes cascade splits (practical scheme λ=0.75) and cascade viewProj matrices |
| `PipelineTypes.h` | ✅ | Added `VkPipeline shadowDepth`, `VkPipelineLayout shadow`, `VkDescriptorSetLayout shadow`, `VkRenderPass shadow` |
| `GraphicsPipelineBuilder.cpp` | ✅ | Shadow depth pipeline: vertex-only, front-face culling, depth bias enabled (constant=4.0, slope=1.5), uses shadow render pass; shadow pipeline layout: `{scene, shadow}` descriptor sets + `ShadowPushConstants` |
| `FrameResourceManager` descriptor expansion | ✅ | Lighting layout expanded from 7 → 10 bindings (7=shadow UBO, 8=shadow atlas SAMPLED_IMAGE, 9=shadow comparison SAMPLER); pool sizes updated; `updateDescriptorSets()` accepts shadow params |
| `FrameRecorder` render graph integration | ✅ | 4 `ShadowCascade0..3` passes added between `OitClear` and `Lighting`; `recordShadowPass()` method renders all opaque geometry per cascade |
| `cmake/Shaders.cmake` exclude filter | ✅ | `shadow_common.slang` excluded from compilation (include-only) |
| `src/CMakeLists.txt` | ✅ | `renderer/shadow/ShadowManager.cpp` added to `VulkanSceneRenderer_renderer` library |
| `RendererFrontend` wiring | ✅ | `ShadowManager` in `OwnedSubsystems`; `createResources()` + `createFramebuffers()` in `initialize()`; per-frame `update()` with camera/aspect/lightDirection; shadow descriptor set + framebuffers passed to `FrameRecordParams`; shadow UBO/atlas/sampler passed to `updateDescriptorSets()` |
| `shadow_depth.slang` binding fix | ✅ | Corrected descriptor bindings to `(1,0)` for ObjectBuffer SSBO (scene set 0, binding 1) and `(0,1)` for ShadowBuffer UBO (shadow set 1, binding 0) |
| Build verification | ✅ | C++ build successful |
| Test verification | ✅ | All 82 existing tests pass |
| Cascade debug view | ✅ | `GBufferViewMode::ShadowCascades = 11` — color-coded cascade visualization (green/blue/yellow/red) with blend zone indicator in `post_process.slang` |
| GUI controls | ⬜ | Shadow on/off toggle, bias controls, PCF kernel size (deferred) |

### Shadow Depth Pipeline

```
Descriptor Set 0 (scene): binding 0 = camera UBO, binding 1 = objects SSBO, ...
Descriptor Set 1 (shadow): binding 0 = ShadowBuffer UBO
Push Constants: { objectIndex, cascadeIndex }
Rasterization: front-face culling, depth bias (constant=4.0, slope=1.5)
Depth/Stencil: depth test + write enabled, GREATER_OR_EQUAL (reverse-Z)
Color: no color attachments
```

### Shadow Sampling (deferred_directional.slang)

```
Binding 7 (set 0): ConstantBuffer<ShadowBuffer> — cascade viewProj + split depths
Binding 8 (set 0): Texture2DArray<float> — shadow atlas (4 layers)
Binding 9 (set 0): SamplerComparisonState — VK_COMPARE_OP_GREATER, linear PCF filtering
```

### Render Graph Order

```
DepthPrepass → GBuffer → OitClear → ShadowCascade0 → ShadowCascade1
  → ShadowCascade2 → ShadowCascade3 → Lighting → OitResolve → PostProcess
```

### Cascade Split Strategy

```
Practical split: splitDepth[i] = lerp(near * pow(far/near, i/N), near + (far-near) * i/N, lambda)
lambda = 0.75
```

### Shadow Bias Strategy

- **Normal offset:** `texelSize * 2.0` along surface normal (in `shadow_common.slang`)
- **Slope-scaled bias:** `max(0.005 * (1.0 - NdotL), 0.001)` (in shader)
- **Pipeline depth bias:** `depthBiasConstantFactor = 4.0`, `depthBiasSlopeFactor = 1.5` (in `GraphicsPipelineBuilder`)

### Completed Integration

All core CSM infrastructure is fully wired:

1. ✅ `shadow_common.slang` excluded from Shaders.cmake (include-only file)
2. ✅ `renderer/shadow/ShadowManager.cpp` added to `src/CMakeLists.txt`
3. ✅ `shadow_depth.slang` descriptor bindings corrected (`binding(1,0)` for ObjectBuffer, `binding(0,1)` for ShadowBuffer)
4. ✅ `ShadowManager` wired into `RendererFrontend`:
   - `std::unique_ptr<ShadowManager> shadowManager` in `OwnedSubsystems`
   - Created in `initialize()` after `LightingManager`, resources + framebuffers created
   - `descriptorSetLayout()` passed to `PipelineDescriptorLayouts.shadow`
   - `resources_.renderPasses.shadow` passed to `PipelineRenderPasses.shadow`
   - Per-frame `update(camera, aspect, lightDirection)` called before command recording
   - `shadowDescriptorSet` + `shadowFramebuffers` set in `buildFrameRecordParams()`
   - Shadow UBO, atlas view, and sampler passed to `updateDescriptorSets()`
   - Destroyed in `shutdown()` alongside other subsystems
5. ✅ Tests updated for new `RenderPassHandles.shadow` and `shadowDescriptorSet` fields
6. ✅ Build verified, all 82 tests pass

### Cascade Blend Overlap Zone

To eliminate hard cuts between cascade levels, `ComputeShadowFactor()` in `shadow_common.slang` now detects when a fragment is near a cascade split boundary and blends between the current and next cascade:

- **Blend fraction:** `CASCADE_BLEND_FRACTION = 0.1` (10% of each cascade's depth range)
- **Blend zone detection:** `blendStart = splitDepths[cascadeIndex] - blendZone` where `blendZone = cascadeRange * CASCADE_BLEND_FRACTION`
- **Smooth interpolation:** `blendFactor = saturate((viewDepth - blendStart) / blendZone)` — linearly interpolates between `SampleCascadeShadow()` results from the current and next cascade
- **Helper function:** `SampleCascadeShadow()` extracts single-cascade PCF sampling (normal offset bias + slope-scaled bias + 3×3 PCF) for reuse by the blend logic

### Shadow Cascade Debug View

A new G-Buffer debug view mode (`GBufferViewMode::ShadowCascades = 11`) provides visual verification of cascade coverage and blend zones:

- **Depth linearization:** Converts reverse-Z depth to linear view depth via `viewZ = (near * far) / (far - depth * (far - near))`
- **Cascade color coding:** Green (cascade 0), Blue (cascade 1), Yellow (cascade 2), Red (cascade 3)
- **Blend zone indicator:** Pixels in the blend overlap zone are brightened to show transition regions
- **Scene overlay:** Color-coded cascade index composited over a dimmed scene (0.3× albedo)
- **Push constants:** `PostProcessPushConstants` extended with `cameraNear`, `cameraFar`, and `cascadeSplits[4]` to pass cascade split depths to the post-process shader

### Future Enhancements

- GUI controls: shadow on/off toggle, bias parameter tuning, PCF kernel size

### Validation Criteria

- [x] 4 cascade shadow maps render correctly (verified with shadow cascade debug view)
- [x] Shadow acne is eliminated with bias settings (triple bias: hardware depth bias + normal offset + slope-scaled)
- [x] Peter panning is minimal (front-face culling + moderate bias values)
- [x] Cascade transitions are smooth (10% blend overlap zone with linear interpolation)
- [ ] Performance: shadow pass adds < 2ms at 2048² per cascade on mid-range GPU (requires runtime profiling)
- [x] All existing tests pass (84/84)

---

## 6. Phase 4 — Clustered / Tiled Light Culling ✅

**Goal:** Replace the per-light stencil draw loop with a GPU-driven tiled/clustered culling system, enabling hundreds to thousands of dynamic point lights.  
**Risk:** High — new compute pipeline, major changes to lighting data layout and shaders.  
**Estimated effort:** Large (3–5 days)  
**Prerequisites:** Phase 1 (shared includes) ✅, Phase 2 (depth reconstruction) ✅

### Problem

The current per-light stencil loop generates `2N + N` Vulkan commands for `N` lights (2 draw calls + 1 stencil clear each). This is command-processor-bound and fundamentally cannot scale.

### Approach: Tiled Deferred (recommended first step)

Divide the screen into 16×16 pixel tiles. A compute shader tests each light sphere against each tile frustum. Each tile builds a light index list. The lighting fragment shader reads only its tile's list.

### Approach: Clustered (future upgrade from tiled)

Extend tiles into the Z dimension (depth slices), creating 3D clusters. Better for scenes with wide depth range and many overlapping lights. More complex but similar shader structure.

### Data Structures

```cpp
// SceneData.h — updated
inline constexpr uint32_t kMaxClusteredLights    = 4096;
inline constexpr uint32_t kTileSize              = 16;   // pixels
inline constexpr uint32_t kMaxLightsPerTile      = 256;

struct PointLightData {
    alignas(16) glm::vec4 positionRadius{0.0f, 0.0f, 0.0f, 1.0f};
    alignas(16) glm::vec4 colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
};
// No change to struct, but stored in SSBO instead of UBO.

struct TileLightGrid {
    uint32_t offset;  // into global light index list
    uint32_t count;   // number of lights affecting this tile
};
```

### New Resources

| Resource | Type | Description |
|---|---|---|
| Light SSBO | `VkBuffer` (storage) | `PointLightData[kMaxClusteredLights]` — all scene lights |
| Tile light grid | `VkBuffer` (storage) | `TileLightGrid[tileCountX * tileCountY]` |
| Light index list | `VkBuffer` (storage) | `uint32_t[]` — packed light indices per tile |
| Tile culling compute pipeline | `VkPipeline` (compute) | Bins lights into tiles |

### New Shaders

| Shader | Description |
|---|---|
| `shaders/tile_light_cull.slang` | Compute shader: one workgroup per tile; loads tile frustum planes; tests all lights; writes light indices to tile grid |
| `shaders/tiled_lighting.slang` | Fragment shader: replaces `point_light.slang`; reads tile grid for pixel's tile; loops over tile's lights; accumulates Cook-Torrance |

### Steps

1. ✅ **Added `kMaxClusteredLights`, `kTileSize`, `kMaxLightsPerTile`** constants in `SceneData.h`. Added `TileLightGrid`, `TileCullPushConstants`, `TiledLightingPushConstants` structs. Mirrored in `lighting_structs.slang`.

2. ✅ **Updated `LightingManager`**:
   - `createTiledResources()` allocates light SSBO, tile grid SSBO, light index list SSBO
   - `updateLightingData()` mirrors point lights to `pointLightsSsbo_` vector
   - `dispatchTileCull()` uploads SSBO, updates compute set0 descriptors, dispatches compute, inserts barrier
   - Compute pipeline + 3 descriptor set layouts (camera+depth, light SSBO, tile grid + index list)
   - Fragment-side tiled descriptor set (light SSBO + tile grid + index list)

3. ✅ **Created `shaders/tile_light_cull.slang`** compute shader:
   - One workgroup (16×16) per tile, shared memory for min/max depth + light indices
   - Sphere-vs-AABB culling in world space using inverse view-projection
   - Fixed-stride tile allocation (tileIndex × MAX_LIGHTS_PER_TILE)

4. ✅ **Created `shaders/tiled_lighting.slang`** fragment shader:
   - Fullscreen triangle, reads G-Buffer + tile grid, loops tile lights
   - Cook-Torrance BRDF from `brdf_common.slang`

5. ✅ **Updated `FrameRecorder::buildGraph()`**:
   - Added `TileCull` pass (compute dispatch) between shadow cascades and Lighting
   - Lighting pass auto-selects tiled path when ready, falls back to stencil loop
   - Pipeline barrier compute→fragment on tile SSBOs

6. Stencil volume infrastructure **kept as fallback** — tiled path is primary, stencil is auto-selected when tiled resources are not available.

7. **Light spawning for testing** — deferred to future work (GUI controls).

### Infrastructure Changes

- ✅ `PipelineManager`: added `createComputePipeline()` method
- ✅ `cmake/Shaders.cmake`: added compute shader entry point (`computeMain`) compilation
- ✅ `PipelineTypes.h`: added `tiledLighting` layout, `tiledPointLight` pipeline, `tiled` descriptor layout
- ✅ `GraphicsPipelineBuilder`: loads tiled_lighting vert/frag modules, creates tiled pipeline
- ✅ `FrameRecordParams`: added `tiledDescriptorSet`, `cameraBuffer`, `cameraBufferSize`, `gBufferSampler`
- ✅ `RendererFrontend`: wires `createTiledResources()`, tiled descriptor set, camera buffer, sampler

### Performance Targets

| Metric | Target |
|---|---|
| 64 lights | < 0.5 ms total (cull + shade) |
| 256 lights | < 1.5 ms total |
| 1024 lights | < 4 ms total |
| 4096 lights | < 10 ms total (at 1080p on mid-range GPU) |

### Validation Criteria

- [x] Tile culling compute shader correctly bins lights (verified with tile light heat map debug view: `GBufferViewMode::TileLightHeatMap = 12`, tile light count → heat map color)
w- [ ] Visual output matches per-light stencil approach for ≤ 4 lights
- [ ] Stable 60 FPS with 256+ lights at 1080p
- [x] No light popping at tile boundaries (conservative 5% radius padding on sphere-vs-AABB test)
- [x] All existing tests pass (84/84)
- [x] C++ build successful
- [x] All shaders compile (Slang → SPIR-V)

---

## 7. Phase 5 — Image-Based Lighting & SSAO ✅

**Goal:** Replace the flat ambient term with physically-based environment lighting and screen-space ambient occlusion.  
**Risk:** Medium — new textures and shader modifications, but no fundamental pipeline changes.  
**Status:** ✅ **Complete** — all core implementation done and build verified  
**Prerequisites:** Phase 2 (G-Buffer optimization) ✅, Phase 3 (shadows) ✅

### Overview

Phase 5 adds two major lighting quality improvements:
- **Part A — IBL:** Split-sum ambient lighting using irradiance cubemap, pre-filtered specular cubemap, and BRDF integration LUT.
- **Part B — GTAO:** Screen-space ambient occlusion via compute shader (half-resolution) + bilateral blur.

### Part A — Image-Based Lighting (IBL)

#### Implementation Summary

1. ✅ **Created `EnvironmentManager` class** (`include/Container/renderer/lighting/EnvironmentManager.h`, `src/renderer/lighting/EnvironmentManager.cpp`):
   - Owns BRDF LUT (512×512 RG16F), placeholder irradiance/prefiltered cubemaps (1×1 white), all samplers
   - `createResources()` generates BRDF LUT via one-time compute dispatch at startup
   - `createPlaceholderCubemaps()` creates white 1×1 RGBA16F cubemaps (6 layers) — ready for HDR environment loading
   - Compute pipeline for BRDF LUT generation with descriptor pool/set/layout

2. ✅ **Created preprocessing compute shaders** (compiled to SPIR-V, ready for HDR environment pipeline):
   - `shaders/brdf_lut.slang` — Hammersley sampling, importance-sampled GGX, IBL geometry term
   - `shaders/equirect_to_cubemap.slang` — equirectangular→cubemap face conversion
   - `shaders/irradiance_convolution.slang` — hemisphere integral for diffuse irradiance
   - `shaders/prefilter_specular.slang` — importance-sampled pre-filtered specular per mip level

3. ✅ **Updated `deferred_directional.slang`** — full split-sum IBL ambient:
   - Bindings 10-14: irradiance cubemap, prefiltered cubemap, BRDF LUT, env sampler, BRDF sampler
   - `FresnelSchlickRoughness()` for energy-conserving kS/kD split
   - Diffuse IBL from irradiance cubemap, specular IBL from prefiltered cubemap + BRDF LUT
   - `ambientLighting = (kD * diffuseIBL + specularIBL) * occlusion * ssao`

4. ✅ **Updated `FrameResourceManager`** — lighting descriptor layout expanded to 17 bindings:
   - Bindings 10-14: IBL textures and samplers
   - Bindings 15-16: AO texture and sampler
   - Descriptor pool sizes updated, fallback descriptors for uninitialized resources

5. ✅ **Updated `RendererFrontend`** — `EnvironmentManager` fully wired:
   - Created in `initialize()` after `ShadowManager`
   - `createResources()` called with executable directory (shader path)
   - `createGtaoResources()` called in `initializeScene()`
   - `recreateGtaoTextures()` called on resize
   - IBL + AO resources passed to `updateDescriptorSets()`

### Part B — Screen-Space Ambient Occlusion (GTAO)

#### Implementation Summary

1. ✅ **Created `shaders/gtao.slang`** compute shader:
   - Half-resolution R8 output, 8×8 workgroups
   - Fibonacci spiral sampling in normal-oriented hemisphere
   - Interleaved gradient noise for spatial jitter
   - World-space depth reconstruction + range falloff
   - Push constants: aoRadius, aoIntensity, sampleCount, fullWidth, fullHeight

2. ✅ **Created `shaders/gtao_blur.slang`** compute shader:
   - 5×5 bilateral blur (Gaussian spatial × depth-aware edge preservation)
   - Push constants: width, height, depthThreshold

3. ✅ **GTAO resources in `EnvironmentManager`**:
   - Two half-res R8 textures (raw AO + blurred result)
   - Compute pipelines + descriptor sets for GTAO and blur
   - `dispatchGtao()` updates descriptors per-frame, dispatches compute, inserts barrier
   - `dispatchGtaoBlur()` dispatches blur, inserts compute→fragment barrier
   - Configurable settings: `aoRadius` (0.5), `aoIntensity` (1.5), `aoSampleCount` (16), `aoEnabled` (true)

4. ✅ **GTAO pass in render graph** (`FrameRecorder::buildGraph()`):
   - `GTAO` pass added between `TileCull` and `Lighting`
   - Dispatches GTAO compute + blur when `isGtaoReady()` is true
   - Reads depth (via `depthSamplingView`) and normals from G-Buffer

5. ✅ **AO bound in lighting pass** — binding 15 (SAMPLED_IMAGE) + binding 16 (SAMPLER) in lighting descriptor set, sampled in `deferred_directional.slang` as `gAOTexture`

6. ✅ **`cmake/Shaders.cmake`** — all Phase 5 shaders excluded from vert/frag glob, added as explicit compute entries

7. ✅ **`src/CMakeLists.txt`** — `renderer/lighting/EnvironmentManager.cpp` added to `VulkanSceneRenderer_renderer`

### Render Graph Order (Updated)

```
DepthPrepass → GBuffer → OitClear → ShadowCascade0..3
  → TileCull → GTAO (+ Blur) → Lighting → OitResolve → PostProcess
```

### Descriptor Bindings (Lighting Set, 17 total)

| Binding | Type | Content | Phase |
|---|---|---|---|
| 0 | UBO | CameraBuffer | 2 |
| 1 | Sampler | G-Buffer sampler | 2 |
| 2-5 | SAMPLED_IMAGE | Albedo, Normal, Material, Emissive | 2 |
| 6 | SAMPLED_IMAGE | Depth (via depthSamplingView) | 2 |
| 7 | UBO | ShadowBuffer | 3 |
| 8 | SAMPLED_IMAGE | Shadow atlas (Texture2DArray) | 3 |
| 9 | Sampler | Shadow comparison sampler | 3 |
| 10 | SAMPLED_IMAGE | Irradiance cubemap | 5 |
| 11 | SAMPLED_IMAGE | Pre-filtered specular cubemap | 5 |
| 12 | SAMPLED_IMAGE | BRDF LUT | 5 |
| 13 | Sampler | Environment cubemap sampler | 5 |
| 14 | Sampler | BRDF LUT sampler | 5 |
| 15 | SAMPLED_IMAGE | AO texture (blurred GTAO) | 5 |
| 16 | Sampler | AO sampler | 5 |

### Future Enhancements

- HDR environment map loading (`.hdr`/`.exr` equirectangular → cubemap → irradiance/prefiltered via compute) ✅ **Implemented** — `EnvironmentManager::loadHdrEnvironment()` loads `.exr` via tinyexr, runs `equirect_to_cubemap`, `irradiance_convolution`, and `prefilter_specular` compute shaders. Default HDR loaded at startup: `hdr/citrus_orchard_road_puresky_4k.exr` (see `AppConfig::kDefaultEnvironmentHdrRelativePath`).
- GUI controls: AO radius, intensity, sample count, on/off toggle
- GUI controls: IBL intensity, environment map selection
- Temporal GTAO for reduced noise
- Screen-space reflections (SSR) complement to IBL

### Validation Criteria

- [ ] IBL produces visible environment reflections on metallic surfaces (requires HDR environment map)
- [ ] Rough dielectrics receive soft diffuse irradiance (requires HDR environment map)
- [ ] SSAO darkens crevices, contact edges, and concavities
- [ ] AO does not produce halo artifacts at object edges
- [x] All existing tests pass (84/84)
- [x] C++ build successful
- [x] All shaders compile (Slang → SPIR-V)

---

## 8. Phase 6 — GPU-Driven Rendering

**Goal:** Reduce CPU draw call overhead via frustum culling, Hi-Z occlusion culling, and multi-draw indirect.  
**Risk:** High — requires compute pipelines, indirect draw buffers, and changes to scene data upload.  
**Estimated effort:** Large (3–5 days)  
**Prerequisites:** All prior phases  
**Status:** ✅ Complete

### Problem

Currently every mesh is drawn unconditionally in every pass (depth prepass, G-Buffer, shadow passes). For scenes with 1000+ objects, CPU-side loop and draw call overhead becomes the bottleneck.

### Implementation

#### Data Layout Changes

- **`ObjectData`** — replaced `alignas(8) glm::vec2 padding` with `alignas(4) float padding0` + `alignas(16) glm::vec4 boundingSphere` (xyz = world-space center, w = radius)
- All 10 vertex shaders updated to match: `float padding0; float4 boundingSphere;` in `ObjectBuffer`
- Added `GpuDrawIndexedIndirectCommand` (matches `VkDrawIndexedIndirectCommand`), `CullPushConstants`, `HiZPushConstants` structs

#### Bounding Sphere Computation

- `SceneController::syncObjectDataFromSceneGraph()` — computes world-space bounding spheres from primitive vertex AABB → local center + radius → transform by worldTransform with max scale factor

#### SV_InstanceID Migration

- All 10 vertex shaders migrated from `pc.objectIndex` push constant to `SV_InstanceID` (maps to `gl_InstanceIndex`)
- All `vkCmdDrawIndexed` call sites updated to pass `dc.objectIndex` as the `firstInstance` parameter
- This enables both direct draws (CPU loop) and indirect draws (`VkDrawIndexedIndirectCommand.firstInstance`) without shader branching
- Shadow shader: `cascadeIndex` remains in push constants; `objectIndex` via `firstInstance`

#### GpuCullManager (New Class)

- **Header:** `include/Container/renderer/culling/GpuCullManager.h`
- **Implementation:** `src/renderer/culling/GpuCullManager.cpp`
- **Frustum cull pipeline:** 5-binding descriptor set (camera UBO, object SSBO, input draws, output draws, draw count) + push constants for object count
- **Occlusion cull pipeline:** 8-binding descriptor set (camera UBO, object SSBO, input draws, output draws, occlusion count, Hi-Z sampled image, frustum draw count, Hi-Z sampler) + push constants
- **Hi-Z generation pipeline:** 3-binding descriptor set (source sampled image, sampler, destination storage image) + push constants for mip dimensions
- **Buffer management:** input (HOST_SEQUENTIAL_WRITE), output (DEVICE_LOCAL + INDIRECT), count (DEVICE_LOCAL + INDIRECT + TRANSFER_DST), occlusion output/count (same layout)
- **Hi-Z image:** R32_SFLOAT with full mip chain, per-mip storage views, nearest-clamp sampler
- **Dispatch flow:** FrustumCull → DepthPrepass → HiZGenerate (per-mip with barriers) → OcclusionCull → GBuffer
- **`drawIndirect()`:** issues `vkCmdDrawIndexedIndirectCount` with frustum-culled results
- **`drawIndirectOccluded()`:** issues `vkCmdDrawIndexedIndirectCount` with occlusion-culled results

#### Compute Shaders (3 New Files)

| Shader | Purpose |
|---|---|
| `shaders/frustum_cull.slang` | Gribb-Hartmann frustum plane extraction, sphere-plane test, atomic draw count, 64 threads/group |
| `shaders/hiz_generate.slang` | Min-reduction depth mip pyramid (8×8, reverse-Z aware, per-mip compute dispatch) |
| `shaders/occlusion_cull.slang` | Full Hi-Z occlusion culling: sphere-to-screen projection, mip selection, conservative 4-corner sampling, reverse-Z comparison |

#### Render Graph Integration

- **Pass order:** FrustumCull → DepthPrepass → HiZGenerate → OcclusionCull → CullStatsReadback → GBuffer → OitClear → ShadowCascades → ...
- FrustumCull uploads draw commands, dispatches compute, produces indirect buffer
- DepthPrepass uses `gpuCullManager_->drawIndirect(cmd)` (frustum-culled)
- HiZGenerate transitions depth to readable, dispatches per-mip Hi-Z generation, transitions depth back
- OcclusionCull dispatches occlusion cull against Hi-Z pyramid using frustum-culled input
- CullStatsReadback copies frustum + occlusion draw counts to HOST_VISIBLE staging buffer (1-frame latency readback, no GPU stall)
- GBuffer uses `gpuCullManager_->drawIndirectOccluded(cmd)` (occlusion-culled)
- ShadowCascades use `gpuCullManager_->drawIndirect(cmd)` (frustum-culled, single indirect call per cascade)
- Diagnostic cube always drawn via direct draw (separate geometry buffer)

#### Stats Readback

- `CullStats` struct: `totalInputCount`, `frustumPassedCount`, `occlusionPassedCount`
- `scheduleStatsReadback(cmd)`: barrier (compute→transfer), 2× `vkCmdCopyBuffer` from count buffers to HOST_VISIBLE staging, barrier (transfer→host)
- `collectStats()`: reads mapped staging pointer at frame start (after fence), populates `lastStats_`
- Stats exposed in ImGui via `GuiManager::setCullStats()` — shows input count, per-stage passed/culled counts

#### Freeze-Culling Debug Mode

- **Purpose:** Verify culling correctness by freezing the culling camera and freely moving the rendering camera to inspect what was culled.
- **F8 keybinding** or ImGui checkbox toggles the freeze state.
- When frozen: `GpuCullManager::freezeCulling(cmd, ...)` snapshots the live camera UBO into a `frozenCameraBuffer_` via `vkCmdCopyBuffer`. Subsequent `dispatchFrustumCull` / `dispatchOcclusionCull` use the frozen buffer for descriptor binding 0 instead of the live camera.
- When unfrozen: `unfreezeCulling()` clears the flag; cull dispatches resume using the live camera.
- ImGui shows orange "CULLING FROZEN" indicator when active.
- Bidirectional sync between F8 key toggle and ImGui checkbox via `presentSceneControls()`.

#### Build System

- `cmake/Shaders.cmake` — exclude filters + explicit compute entries for 3 new shaders
- `src/CMakeLists.txt` — added `renderer/culling/GpuCullManager.cpp`

### Files Modified

| File | Change |
|---|---|
| `include/Container/utility/SceneData.h` | `ObjectData.boundingSphere`, new GPU-driven structs |
| `src/renderer/scene/SceneController.cpp` | Bounding sphere computation |
| `include/Container/renderer/culling/GpuCullManager.h` | **New** — GPU cull manager class |
| `src/renderer/culling/GpuCullManager.cpp` | **New** — full implementation |
| `shaders/frustum_cull.slang` | **New** — frustum cull compute |
| `shaders/hiz_generate.slang` | **New** — Hi-Z mip generation |
| `shaders/occlusion_cull.slang` | **New** — full Hi-Z occlusion cull (sphere projection, mip selection, 4-corner sampling) |
| `shaders/depth_prepass.slang` | ObjectBuffer + SV_InstanceID |
| `shaders/gbuffer.slang` | ObjectBuffer + SV_InstanceID |
| `shaders/shadow_depth.slang` | ObjectBuffer + SV_InstanceID |
| `shaders/forward_transparent.slang` | ObjectBuffer + SV_InstanceID |
| `shaders/geometry_debug.slang` | ObjectBuffer + SV_InstanceID |
| `shaders/normal_validation.slang` | ObjectBuffer + SV_InstanceID |
| `shaders/object_normals.slang` | ObjectBuffer + SV_InstanceID |
| `shaders/surface_normals.slang` | ObjectBuffer + SV_InstanceID |
| `shaders/wireframe_debug.slang` | ObjectBuffer + SV_InstanceID |
| `shaders/wireframe_fallback.slang` | ObjectBuffer + SV_InstanceID |
| `src/renderer/debug/DebugOverlayRenderer.cpp` | firstInstance = dc.objectIndex in all draw calls |
| `src/renderer/core/FrameRecorder.cpp` | GpuCullManager wiring, FrustumCull pass, indirect draw paths |
| `include/Container/renderer/core/FrameRecorder.h` | GpuCullManager params |
| `include/Container/renderer/core/RendererFrontend.h` | GpuCullManager ownership |
| `src/renderer/core/RendererFrontend.cpp` | GpuCullManager creation + wiring, `collectStats()` at frame start, F8 freeze toggle |
| `include/Container/utility/GuiManager.h` | `setCullStats()`, `setFreezeCulling()`, `freezeCullingRequested()` methods |
| `src/utility/GuiManager.cpp` | Stats display, freeze checkbox + orange indicator in Scene Controls |
| `include/Container/renderer/debug/DebugRenderState.h` | `freezeCulling` + `freezeCullingKeyDown` fields |
| `include/Container/renderer/core/FrameRecorder.h` | `debugFreezeCulling` in FrameRecordParams |
| `cmake/Shaders.cmake` | 3 new compute shader entries |
| `src/CMakeLists.txt` | GpuCullManager.cpp source |

### Performance Targets

| Scene | Current | Target |
|---|---|---|
| 100 objects | ~100 draw calls/pass | 1 indirect call/pass |
| 1000 objects | ~1000 draw calls/pass | 1 indirect call/pass, ~200 drawn after culling |
| 10,000 objects | Not viable | 1 indirect call/pass, ~500 drawn after culling |

### Validation Criteria

- [x] Frustum culling correctly hides off-screen objects (freeze-camera debug mode implemented: F8 toggle)
- [x] Hi-Z occlusion culling correctly hides fully occluded objects
- [x] Draw call count drops to 1–2 per pass regardless of object count
- [x] No visual popping or missing objects
- [x] All existing tests pass
- [x] C++ build successful
- [x] All shaders compile (Slang → SPIR-V)

---

## Appendix A — File Inventory

### C++ Files (Lighting-Related)

| File | Role | Phase |
|---|---|---|
| `include/Container/utility/SceneData.h` | `LightingData`, `PointLightData`, `ShadowData`, `ShadowCascadeData`, `ShadowPushConstants`, `TileLightGrid`, `TileCullPushConstants`, constants | 1, 3, 4 |
| `include/Container/renderer/lighting/LightingManager.h` | `LightingManager` class, `SceneLightingAnchor`, `LightPushConstants`, tiled culling | 4 |
| `src/renderer/lighting/LightingManager.cpp` | Light data computation, descriptor setup, gizmo drawing, tiled culling dispatch | 4 |
| `include/Container/renderer/shadow/ShadowManager.h` | `ShadowManager` class — owns shadow atlas, UBO, sampler, framebuffers, cascade computation | 3 |
| `src/renderer/shadow/ShadowManager.cpp` | Shadow manager implementation — cascade splits, viewProj, VMA image/buffer allocation | 3 |
| `include/Container/renderer/lighting/EnvironmentManager.h` | `EnvironmentManager` class — BRDF LUT, placeholder cubemaps, GTAO compute dispatch, IBL accessors | 5 |
| `src/renderer/lighting/EnvironmentManager.cpp` | IBL resource creation (BRDF LUT compute, cubemaps, samplers), GTAO pipelines/textures, per-frame dispatch | 5 |
| `include/Container/renderer/core/FrameRecorder.h` | `FrameRecordParams` (shadow/tiled/camera/gpuCullManager params), `recordShadowPass()` | 3, 4, 6 |
| `src/renderer/core/FrameRecorder.cpp` | Render pass recording; shadow cascade + tile cull + GTAO + frustum cull + indirect draw + lighting passes in render graph | 3, 4, 5, 6 |
| `include/Container/renderer/resources/FrameResources.h` | Per-frame GPU resources (framebuffers, descriptor sets, `depthSamplingView`) | 2 |
| `include/Container/renderer/resources/FrameResourceManager.h` | Descriptor layout creation (17-binding lighting layout with shadow 7–9, IBL 10–14, AO 15–16) | 2, 3, 5 |
| `src/renderer/resources/FrameResourceManager.cpp` | Attachment/framebuffer/descriptor creation, `updateDescriptorSets()` with shadow/IBL/AO params | 2, 3, 5 |
| `include/Container/renderer/core/RenderPassManager.h` | `RenderPasses` struct (includes `shadow` render pass) | 3 |
| `src/renderer/core/RenderPassManager.cpp` | Render pass creation (depth prepass, G-Buffer, shadow, lighting, post-process) | 2, 3 |
| `include/Container/renderer/pipeline/PipelineTypes.h` | `PipelineLayouts` (shadow, tiledLighting), `GraphicsPipelines` (shadowDepth, tiledPointLight), descriptor layouts | 3, 4 |
| `include/Container/renderer/pipeline/GraphicsPipelineBuilder.h` | Pipeline builder interface | 3 |
| `src/renderer/pipeline/GraphicsPipelineBuilder.cpp` | Shadow depth + tiled lighting pipeline creation | 3, 4 |
| `include/Container/renderer/core/RendererFrontend.h` | `OwnedSubsystems` (shadowManager, environmentManager, gpuCullManager), `buildFrameRecordParams()` | 3, 5, 6 |
| `src/renderer/core/RendererFrontend.cpp` | ShadowManager + EnvironmentManager + GpuCullManager wiring, IBL/AO descriptor updates | 3, 5, 6 |

### Shader Files (Lighting-Related)

| File | Role | Phase |
|---|---|---|
| `shaders/brdf_common.slang` | Shared BRDF functions + `FresnelSchlickRoughness()` + `ReconstructWorldPosition()` (include-only) | 1, 5 |
| `shaders/lighting_structs.slang` | Shared struct definitions: `CameraBuffer`, `LightingBuffer`, `ShadowBuffer`, `TileLightGrid` (include-only) | 1, 4 |
| `shaders/shadow_common.slang` | `SelectCascade()`, `SampleCascadeShadow()`, `ComputeShadowFactor()` with 3×3 PCF + cascade blend (include-only) | 3 |
| `shaders/shadow_depth.slang` | Vertex-only shadow depth shader (ObjectBuffer SSBO + ShadowBuffer UBO + push constants) | 3 |
| `shaders/tile_light_cull.slang` | Compute shader: tiled light culling (16×16 tiles, sphere-vs-AABB) | 4 |
| `shaders/tiled_lighting.slang` | Fragment shader: tiled deferred point light shading | 4 |
| `shaders/brdf_lut.slang` | Compute shader: BRDF integration LUT (512×512, RG16F, split-sum) | 5 |
| `shaders/equirect_to_cubemap.slang` | Compute shader: equirectangular→cubemap face conversion | 5 |
| `shaders/irradiance_convolution.slang` | Compute shader: hemisphere integral for diffuse irradiance cubemap | 5 |
| `shaders/prefilter_specular.slang` | Compute shader: importance-sampled pre-filtered specular per mip | 5 |
| `shaders/gtao.slang` | Compute shader: GTAO half-res ambient occlusion | 5 |
| `shaders/gtao_blur.slang` | Compute shader: bilateral blur for GTAO denoising | 5 |
| `shaders/gbuffer.slang` | G-Buffer fill (4 MRT output) | 2 |
| `shaders/deferred_directional.slang` | Directional light fullscreen pass (Cook-Torrance + CSM shadows + IBL + SSAO, bindings 0–16) | 1, 2, 3, 5 |
| `shaders/point_light.slang` | Point light fullscreen pass (depth reconstruction, shared BRDF) | 1, 2 |
| `shaders/point_light_stencil_debug.slang` | Debug visualization of point light stencil volumes | 1 |
| `shaders/light_stencil.slang` | Stencil volume cube vertex shader | — |
| `shaders/light_gizmo.slang` | Light source gizmo visualization | — |
| `shaders/gbuffer_composite.slang` | Post-process composite (depth reconstruction, shared BRDF) | 1, 2 |
| `shaders/post_process.slang` | Tone mapping, final output, bloom composite, shadow cascade debug view (mode 11), tile light heat map (mode 12) | 2, Bloom, 3, 4 |
| `shaders/surface_normal_common.slang` | Shared normal utilities (include-only) | — |
| `shaders/frustum_cull.slang` | Compute shader: frustum culling (Gribb-Hartmann, sphere test, indirect buffer output) | 6 |
| `shaders/hiz_generate.slang` | Compute shader: Hi-Z depth mip pyramid (min reduction, reverse-Z) | 6 |
| `shaders/occlusion_cull.slang` | Compute shader: Hi-Z occlusion culling (sphere projection, mip selection, conservative sampling) | 6 |
| `shaders/bloom_downsample.slang` | Compute shader: bloom brightness extraction + 13-tap progressive downsample (Jimenez 2014) | Bloom |
| `shaders/bloom_upsample.slang` | Compute shader: bloom 9-tap tent filter upsample with additive blend | Bloom |

### Files Created (Phase 6)

| Phase | File | Type | Status |
|---|---|---|---|
| 6 | `shaders/frustum_cull.slang` | Compute shader | ✅ Created |
| 6 | `shaders/hiz_generate.slang` | Compute shader | ✅ Created |
| 6 | `shaders/occlusion_cull.slang` | Compute shader | ✅ Created (full Hi-Z implementation) |
| 6 | `include/Container/renderer/culling/GpuCullManager.h` | C++ header | ✅ Created |
| 6 | `src/renderer/culling/GpuCullManager.cpp` | C++ source | ✅ Created |

### Files Created (Bloom)

| Feature | File | Type | Status |
|---|---|---|---|
| Bloom | `shaders/bloom_downsample.slang` | Compute shader | ✅ Created (13-tap downsample, Jimenez 2014, soft knee threshold, Karis 2014) |
| Bloom | `shaders/bloom_upsample.slang` | Compute shader | ✅ Created (9-tap tent filter upsample, additive blend) |
| Bloom | `include/Container/renderer/effects/BloomManager.h` | C++ header | ✅ Created |
| Bloom | `src/renderer/effects/BloomManager.cpp` | C++ source | ✅ Created |

---

## 9. Bloom Post-Processing

**Status:** ✅ Complete

### Overview

Physically-motivated bloom using a dual-filter downsample/upsample mip chain
(Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014).
Bright regions of the HDR scene color are extracted with a soft knee threshold
(Karis 2014) and progressively blurred through a 6-level mip pyramid, then
composited back into the scene before tone mapping.

### Algorithm

1. **Brightness extraction + Downsample chain** (compute shader, `bloom_downsample.slang`):
   - Mip 0 applies soft-knee threshold: `max(0, brightness - threshold + knee)² / (4 * knee)`
   - Each subsequent mip uses a 13-tap filter (Jimenez 2014) for high-quality progressive downsample
   - 6 mip levels at successive half-resolutions (`R16G16B16A16_SFLOAT`)

2. **Upsample chain** (compute shader, `bloom_upsample.slang`):
   - 9-tap tent filter (3×3 bilinear) for smooth upsampling
   - Additive blend with destination mip at each level
   - Final pass writes result to mip 0

3. **Compositing** (in `post_process.slang`):
   - Bloom result (mip 0) sampled and added to HDR scene color before ACES tone mapping
   - Controlled by `bloomEnabled` and `bloomIntensity` push constant fields

### Render Graph Integration

```
... → OIT Resolve → Bloom → Post-Process → ...
```

The Bloom pass sits between OIT Resolve and Post-Process. It transitions the
scene color attachment from `COLOR_ATTACHMENT_OPTIMAL` to `SHADER_READ_ONLY_OPTIMAL`,
runs the downsample/upsample compute dispatches, then the post-process shader
samples the bloom result via descriptor set bindings 7–8.

### GUI Controls

| Control | Range | Default | Description |
|---|---|---|---|
| Bloom Enabled | checkbox | ✅ on | Enable/disable the entire bloom pass |
| Bloom Threshold | 0.0 – 5.0 | 1.0 | HDR brightness threshold for extraction |
| Bloom Knee | 0.0 – 1.0 | 0.1 | Soft knee width (smooth threshold transition) |
| Bloom Intensity | 0.0 – 2.0 | 0.3 | Strength of bloom contribution to final image |
| Bloom Radius | 0.1 – 3.0 | 1.0 | Filter radius for upsample tent filter |

### Files Modified

| File | Changes |
|---|---|
| `shaders/post_process.slang` | Added bloom texture/sampler bindings (7–8), bloom compositing before tone mapping, shadow cascade debug view (outputMode 11) |
| `include/Container/utility/SceneData.h` | Extended `PostProcessPushConstants` with `bloomEnabled`, `bloomIntensity`, `cameraNear`, `cameraFar`, `cascadeSplits[4]`, `tileCountX`, `totalLights` |
| `src/renderer/core/FrameRecorder.cpp` | Added Bloom render graph pass, bloom push constant fields |
| `include/Container/renderer/core/FrameRecorder.h` | Added `BloomManager*` to `FrameRecordParams` and constructor |
| `src/renderer/core/RendererFrontend.cpp` | BloomManager lifecycle, descriptor updates, GUI bidirectional sync |
| `include/Container/renderer/core/RendererFrontend.h` | Added `unique_ptr<BloomManager>` to `OwnedSubsystems` |
| `src/renderer/resources/FrameResourceManager.cpp` | Extended post-process descriptor layout (7→9 bindings), pool sizes, descriptor writes |
| `include/Container/renderer/resources/FrameResourceManager.h` | Extended `updateDescriptorSets()` signature |
| `src/utility/GuiManager.cpp` | Added bloom GUI section (checkbox + sliders), `setBloomSettings()` |
| `include/Container/utility/GuiManager.h` | Added bloom settings members and accessors |
| `cmake/Shaders.cmake` | Added bloom shader compilation entries |
| `src/CMakeLists.txt` | Added `renderer/effects/BloomManager.cpp` |

---

## Appendix B — Reference Material

### Tiled / Clustered Shading

- Olsson & Assarsson, "Tiled Shading" (2011) — foundational paper on tiled deferred
- Olsson, Billeter & Assarsson, "Clustered Deferred and Forward Shading" (2012)
- Epic Games / Unreal Engine 5 — Lumen uses screen-space probes + clustered forward for local lights
- id Software / DOOM (2016) — textbook clustered forward implementation

### Cascaded Shadow Maps

- Dimitrov & Wimmer, "Cascaded Shadow Maps" (ShaderX5, 2006)
- Microsoft DirectX 11 CSM sample — practical split scheme and stabilization
- Reverse-Z CSM — use `VK_COMPARE_OP_GREATER` for better depth precision

### IBL

- Karis, "Real Shading in Unreal Engine 4" (SIGGRAPH 2013) — split-sum approximation
- Lagarde & de Rousiers, "Moving Frostbite to PBR" (SIGGRAPH 2014)

### SSAO / GTAO

- Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect Occlusion" (SIGGRAPH 2016) — GTAO
- Bavoil & Sainz, "Screen Space Ambient Occlusion" (NVIDIA, 2008) — original SSAO/HBAO

### GPU-Driven Rendering

- Wihlidal, "Optimizing the Graphics Pipeline with Compute" (GDC 2016)
- Ubisoft, "GPU-Driven Rendering Pipelines" (SIGGRAPH 2015)

### Bloom

- Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014) — dual-filter downsample/upsample
- Karis, "Real Shading in Unreal Engine 4" (SIGGRAPH 2013) — soft knee threshold for bloom extraction
