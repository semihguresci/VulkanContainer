# Coordinate Audit: Winding, Normals, and Shader Wiring

This is the deeper reasoning and audit companion to
[`coordinate-conventions.md`](coordinate-conventions.md).

It answers three questions:

1. What Vulkan actually defines
2. What the engine chooses on top of that
3. Which local passes and shaders currently match that choice

## 1. Vulkan Facts That Matter Here

### 1.1 Clip and NDC depth

Without `VK_EXT_depth_clip_control`, Vulkan clips against `0 <= Zc <= Wc`.
After perspective divide, `z_ndc` is in `[0, 1]`.

That is why a reverse-Z projection in Vulkan is naturally:

- near -> `z_ndc = 1`
- far -> `z_ndc = 0`

### 1.2 Front-face classification

The Vulkan rasterizer decides front-facing vs back-facing from the sign of the
polygon area in framebuffer coordinates, not from world space and not from raw
clip-space coordinates.

Consequences:
- Winding and viewport setup are coupled.
- Reverse-Z does not change front-facing classification.
- If you change viewport sign, you must re-check the culling convention.

### 1.3 Framebuffer coordinates

Framebuffer coordinates use an upper-left origin with +Y downward.

A positive-height viewport preserves that default mapping.
A negative-height viewport flips the mapping between NDC Y and framebuffer Y.

## 2. Engine Choice

VulkanSceneRenderer chooses the following:

- Right-handed world and view space
- Camera forward is -Z
- Projection matrices do not flip Y
- Scene passes use a negative-height viewport
- Shadow passes use a positive-height viewport
- Scene front faces are clockwise
- Shadow front faces are counter-clockwise
- Depth is reverse-Z
- G-buffer normals are stored in world space

This gives a consistent rule set:

| Domain | Convention |
| --- | --- |
| World/view | RH, +Y up, forward -Z |
| Scene NDC | +Y up |
| Scene framebuffer UV | origin top-left, +V down |
| Shadow framebuffer UV | origin top-left, +V down |

The important part is that scene NDC and framebuffer UV disagree on Y, while
shadow NDC and shadow UV do not because the shadow pass keeps a positive
viewport.

## 3. The Two Valid Y Conversions

There should only be two families of conversion in this codebase.

### 3.1 Scene framebuffer UV <-> scene NDC

Scene passes render with a negative-height viewport, so framebuffer UV and NDC
have opposite Y directions:

```text
sceneUV -> sceneNDC:
  x = 2u - 1
  y = 1 - 2v

sceneNDC -> sceneUV:
  u = 0.5x + 0.5
  v = 0.5 - 0.5y
```

### 3.2 Shadow NDC <-> shadow UV

Shadow cascades render with a positive-height viewport, so shadow UV uses the
default Vulkan mapping:

```text
shadowNDC -> shadowUV:
  u = 0.5x + 0.5
  v = 0.5y + 0.5
```

Using the scene Y flip for shadow UV is incorrect.

## 4. Winding, Normals, and Double-Sided Materials

### 4.1 Geometric face normal

For a triangle `(p0, p1, p2)` in a right-handed basis:

```text
faceNormal = normalize(cross(p1 - p0, p2 - p0))
```

If the vertices are wound CCW when viewed from the front, that cross product
points out of the front face.

### 4.2 Reverse-Z does not touch normals

Reverse-Z changes depth encoding only. It does not change:

- geometric face orientation
- normal direction
- front-face classification
- cull mode choice

### 4.3 glTF double-sided rule

glTF requires two things when `material.doubleSided == true`:

1. Disable back-face culling
2. Reverse the back-face normal before lighting

Flipping the normal without disabling culling is incomplete.
Disabling culling without flipping the normal is also incomplete.

## 5. Per-File Audit

This section records the current status after the coordinate audit.

### 5.1 Core math and camera

| File | Status | Notes |
| --- | --- | --- |
| `include/Container/common/CommonMath.h` | Corrected | Projection helpers are RH reverse-Z with no Y flip |
| `include/Container/utility/Camera.h` | OK | View matrix stays RH; view-projection is `proj * view` |
| `src/renderer/CameraController.cpp` | OK | Uploads `viewProj` and `inverseViewProj` directly |

### 5.2 Viewports and render passes

| File | Status | Notes |
| --- | --- | --- |
| `src/renderer/FrameRecorder.cpp` scene viewport | OK | Negative height for scene-facing passes |
| `src/renderer/FrameRecorder.cpp` shadow viewport | OK | Positive height for shadow cascades |
| `src/renderer/RenderPassManager.cpp` | OK | Reverse-Z clear values and read/write transitions are coherent |

### 5.3 Raster state

| File | Status | Notes |
| --- | --- | --- |
| `src/renderer/GraphicsPipelineBuilder.cpp` front face | Fixed | Scene passes now use `VK_FRONT_FACE_CLOCKWISE` for the negative-height viewport, while shadow passes keep `VK_FRONT_FACE_COUNTER_CLOCKWISE` for the positive-height viewport |
| `src/geometry/GltfModelLoader.cpp` triangle winding | Fixed | Imported normals repair triangles whose geometric face normal is opposite to the authored vertex normals before renderer buffers are built |
| `src/renderer/GraphicsPipelineBuilder.cpp` scene cull | Fixed | Repaired single-sided meshes use back-face culling; mirrored transforms route through front-cull variants |
| `src/renderer/GraphicsPipelineBuilder.cpp` shadow cull | Fixed | Shadow casters use the same repaired-winding cull policy as scene passes, with reverse-Z bias retained |

### 5.4 Deferred path shader wiring

| File | Status | Notes |
| --- | --- | --- |
| `src/renderer/GraphicsPipelineBuilder.cpp` G-buffer vertex layout | Fixed | G-buffer now uses the full vertex layout so normal and tangent attributes reach the shader |
| `shaders/gbuffer.slang` normal/tangent usage | Fixed | Deferred shading now receives valid normal/tangent data |
| `shaders/gbuffer.slang` double-sided normal flip | Fixed | Back-face normals are reversed before storing the lighting normal, and `SV_IsFrontFace` now matches the negative-height scene viewport |
| `src/renderer/FrameRecorder.cpp` material routing | Fixed | Single-sided opaque draws stay on the indirect GPU-cull path; double-sided draws use explicit no-cull passes |
| `shaders/brdf_common.slang` | OK | Scene UV to NDC reconstruction uses the required Y flip |
| `shaders/deferred_directional.slang` | OK | Uses scene UV from `SV_Position`, reconstructs world position consistently |
| `shaders/point_light.slang` | OK | Same reconstruction path as directional |
| `shaders/forward_transparent.slang` | Fixed | Matches the G-buffer normal path, uses the corrected scene `SV_IsFrontFace` convention, and guards view/light normalization to avoid NaNs when the camera sits on geometry or the light direction degenerates |
| `shaders/gbuffer.slang` glTF material scalars | Fixed | Runtime now preserves and applies `normalTexture.scale` and `occlusionTexture.strength` instead of silently dropping them |
| `shaders/forward_transparent.slang` glTF material scalars | Fixed | Transparent shading now applies the same normal-scale and occlusion-strength rules as the G-buffer path |
| `shaders/post_process.slang` depth debug | Fixed | Reverse-Z depth views now linearize perspective depth instead of treating it as `1 - depth` |
| `shaders/tiled_lighting.slang` | OK | Same reconstruction path as directional |

### 5.5 Shadow path

| File | Status | Notes |
| --- | --- | --- |
| `src/renderer/ShadowManager.cpp` | Fixed earlier | Light-space signed Z bounds are converted to positive reverse-Z ortho distances |
| `shaders/shadow_common.slang` | OK | Positive-height shadow viewport means `shadowUV = shadowNdc * 0.5 + 0.5` |
| `shaders/shadow_depth.slang` | Fixed | Shadow depth now mirrors the depth prepass alpha-mask discard so cutout materials cast cutout shadows |

### 5.6 Compute passes

| File | Status | Notes |
| --- | --- | --- |
| `shaders/gtao.slang` | OK | Reprojection back to scene buffers flips Y |
| `shaders/occlusion_cull.slang` | Fixed | Uses the scene Y flip and now computes conservative sphere bounds/depth in reverse-Z instead of mixing screen radius into depth |
| `shaders/tile_light_cull.slang` | OK | Tile corners reconstruct through scene NDC with the scene Y flip |
| `shaders/hiz_generate.slang` | OK | Reverse-Z min reduction is correct for conservative occlusion |
| `shaders/frustum_cull.slang` | Fixed | Frustum planes must be extracted from Slang row indexing (`m[row][column]`), not GLSL-style column access |

### 5.7 Debug passes

| File | Status | Notes |
| --- | --- | --- |
| `shaders/normal_validation.slang` | Fixed | Face normal uses RH cross product in world space and now guards the camera-to-centroid direction against zero-length/NaN cases |
| `shaders/surface_normals.slang` | OK | Surface normal visualization uses world-space face normals |
| `shaders/object_normals.slang` | OK | Object-space debug output is isolated from the world-space lighting path |

## 6. Practical Rules For New Code

When adding a new pass:

1. Decide whether it renders to a scene target or a shadow-like offscreen target.
2. Pick the viewport sign first.
3. Write the UV <-> NDC conversion explicitly for that viewport.
4. Keep reverse-Z logic separate from Y-flip logic.
5. If the pass shades double-sided materials, verify both cull mode and
   back-face normal handling.

When debugging:

- If world-position reconstruction is upside down, check scene UV <-> NDC math.
- If shadow lookups are upside down, check whether the pass used the scene Y
  flip by mistake.
- If normals look random, verify the pipeline is providing the shader's full
  vertex layout before changing any math.
