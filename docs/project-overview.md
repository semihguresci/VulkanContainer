# Project Overview

VulkanSceneRenderer is focused on modern real-time Vulkan rendering for glTF
scenes, BIM sidecar models, and renderer experimentation.

## Features

- Vulkan renderer using GLFW, Vulkan Memory Allocator, and Slang shaders
  compiled to SPIR-V.
- glTF scene loading with generated local test assets and optional Khronos
  glTF sample models.
- Sidecar loading for `.bim`, STEP IFC tessellation and swept-solid samples,
  IFC5 IFCX JSON meshes, USD/USDA/USDC/USDZ mesh content, and glTF/GLB
  fallback content through a dedicated BIM render path.
- Deferred opaque rendering plus forward transparent rendering with OIT.
- PBR material support, normal maps, alpha masks, emissive textures, and
  MaterialX integration.
- Reverse-Z depth, negative-height scene viewports, cascaded shadows, tiled
  point lighting, GTAO, bloom, and HDR environment loading.
- GPU frustum and occlusion culling with indirect draw paths.
- Render graph scheduling for frame passes and an EnTT-backed ECS bridge for
  renderables, active camera data, and point-light queries.
- Separate BIM depth and G-buffer passes that write into the existing scene
  depth and deferred lighting inputs without merging BIM geometry into the main
  glTF scene buffers.
- Debug views for wireframe, normals, geometry, light volumes, depth, cascades,
  tiled lighting, and post-process output.

## Repository Layout

- `include/Container/` - public headers for app, renderer, scene, ECS,
  geometry, and utility systems.
- `src/` - implementation split into renderer, geometry, utility, ECS, and app
  libraries.
- `shaders/` - Slang shader sources and shared shader includes.
- `materials/`, `models/`, `hdr/` - runtime assets and source assets copied,
  generated, or downloaded by CMake.
- `cmake/` - dependency grouping, shader compilation, model generation, and
  asset-copy targets.
- `docs/` - renderer conventions, architecture notes, lighting plans, the
  realistic rendering pipeline roadmap, and feature specifications.
- `tests/` - CTest/GTest test targets.

## Naming

The repository identity and CMake targets use `VulkanSceneRenderer`. Existing
include paths and namespaces still use `Container` / `container::`; treat those
as stable code API names unless a deliberate API-wide rename is planned.
