# Renderer PBR Implementation

This document records the renderer's glTF PBR material pipeline, the supported
features, and the implementation rules for extending it. It is the current
source of truth for material import, GPU layout, shader evaluation, known
limitations, and validation.

## Current Status

The renderer supports the requested PBR stack:

- Metallic-roughness, normal, emissive, and occlusion maps.
- `KHR_materials_specular`.
- `KHR_materials_clearcoat`.
- `KHR_materials_sheen`.
- `KHR_materials_transmission` plus `KHR_materials_volume`.
- `KHR_materials_iridescence`.
- `KHR_materials_dispersion`.
- Multiple-scattering compensation for specular IBL.

The renderer is deferred-first. Opaque and alpha-mask materials write a compact
G-buffer; transparent and transmission materials use the forward/OIT path where
the full object material data is available. This is the most important design
constraint when adding new material features.

## Plan Review

The proposed plan is directionally correct:

- Use an uber-shader style material path instead of one shader per glTF
  extension.
- Keep textures bindless so glTF materials can reference different textures
  without rebinding descriptors per draw.
- Split reusable Slang code into material evaluation, BRDF, normal mapping,
  IBL, and shared data-layout includes.
- Keep opaque, masked, and blended materials as pipeline buckets rather than
  creating a pipeline for each material feature.
- Treat glTF PBR as a layered material model: base metallic-roughness plus
  optional specular, clearcoat, sheen, transmission, volume, iridescence, IOR,
  emissive strength, and unlit behavior.

The parts that need adjustment for this renderer:

- The renderer is currently deferred-first, not Forward+. Opaque and alpha-mask
  objects write a G-buffer, then lighting is applied in deferred passes.
  Transparent materials use a forward path with OIT.
- Material data is currently baked into per-object `ObjectData`, not a separate
  `GpuMaterial` SSBO plus material index. A material SSBO is a good future
  refactor, but not required for the current renderer.
- Descriptor sets already group scene data and bindless textures together in
  the scene descriptor set. Do not redesign descriptor ownership without also
  updating `SceneManager`, `FrameResourceManager`, and pipeline layouts.
- Transmission must not be named or treated as generic refraction. glTF
  `KHR_materials_transmission` is a thin-surface transmission factor. True
  volume behavior comes from `KHR_materials_volume`, where thickness and
  attenuation are applied.
- Opaque deferred shading cannot evaluate every layered extension perfectly
  unless the G-buffer stores enough data or the lighting pass can fetch material
  data by material/object index. The current implementation uses compact
  approximations for opaque objects and richer evaluation in the transparent
  forward path.

## Current Renderer Shape

Frame order:

```text
Depth prepass
  -> Hi-Z / occlusion cull
  -> G-buffer
  -> shadow cascades
  -> tile light cull
  -> GTAO
  -> deferred lighting
  -> transparent OIT
  -> bloom
  -> post-process and debug UI
```

Important files:

- CPU material model: `include/Container/utility/Material.h`
- GPU object/material upload layout: `include/Container/utility/SceneData.h`
- glTF and MaterialX import bridge: `src/utility/MaterialXIntegration.cpp`
- Material resolution and texture descriptors: `src/utility/SceneManager.cpp`
- Object data upload: `src/renderer/SceneController.cpp`
- Shared shader object layout: `shaders/object_data_common.slang`
- Shared PBR material helpers: `shaders/pbr_material_common.slang`
- Shared BRDF helpers: `shaders/brdf_common.slang`
- Opaque material evaluation: `shaders/gbuffer.slang`
- Transparent material evaluation: `shaders/forward_transparent.slang`

## Implementation Ownership

| Responsibility | File |
|---|---|
| CPU material factors and texture indices | `include/Container/utility/Material.h` |
| Host-to-GPU layout and object flags | `include/Container/utility/SceneData.h` |
| glTF extension parsing and texture color-space classification | `src/utility/MaterialXIntegration.cpp` |
| Material resolution into per-object GPU data | `src/utility/SceneManager.cpp` and `src/renderer/SceneController.cpp` |
| Shader-side `ObjectData` mirror | `shaders/object_data_common.slang` |
| Texture channel sampling and material parameter helpers | `shaders/pbr_material_common.slang` |
| GGX BRDF, Fresnel, F0, IBL compensation | `shaders/brdf_common.slang` |
| Opaque and alpha-mask material packing | `shaders/gbuffer.slang` |
| Deferred directional and IBL lighting | `shaders/deferred_directional.slang` |
| Deferred point-light accumulation | `shaders/point_light.slang` and `shaders/tiled_lighting.slang` |
| Transparent layered material evaluation | `shaders/forward_transparent.slang` |

When adding a material field, update the CPU `Material`, GPU `ObjectData`,
shader `ObjectBuffer`, static layout assertions, object upload code, importer,
and at least one shader path in the same change.

## Data Model

The current material data path is:

```text
glTF material
  -> container::material::Material
  -> container::gpu::ObjectData
  -> shaders/object_data_common.slang::ObjectBuffer
```

`ObjectData` carries both transform data and material data. This keeps draw
submission simple because each draw already references an object index.

Current feature data includes:

- Base color, opacity, alpha cutoff, double-sided state.
- Metallic, roughness, normal scale, occlusion strength.
- Emissive color and `KHR_materials_emissive_strength`.
- Separate roughness, metalness, specular intensity, specular color, height,
  opacity, transmission, clearcoat, sheen, volume, and iridescence textures.
- IOR, dispersion, attenuation color, attenuation distance, thickness, and
  iridescence thickness range.
- Workflow flags for alpha mask, alpha blend, double-sided, specular-glossiness,
  and unlit.

Future refactor:

```text
ObjectData
  transform
  normal matrix
  materialIndex
  bounding sphere

GpuMaterial SSBO
  factors
  texture indices
  feature flags
```

That refactor is useful when many objects share a material, because it reduces
object-buffer bandwidth and lets deferred lighting fetch full material data.
It should be done deliberately because all cull, depth, debug, G-buffer, and
transparent shaders depend on the object layout.

## Descriptor Strategy

Current scene descriptors are already close to the proposed bindless design:

- Camera data.
- Object data buffer.
- Shared material sampler.
- Bindless sampled image array for material textures.

The important rule is that texture indices use `0xffffffff` as invalid. The
shader can infer most feature presence from a valid texture index or non-default
factor. A separate feature bit is still useful when interpretation changes, such
as specular-glossiness where alpha is glossiness rather than roughness.

Recommended next descriptor evolution:

- Keep the current scene descriptor set stable until a material SSBO is added.
- When adding `GpuMaterial`, keep bindless textures in the same descriptor
  model and pass `materialIndex` through object/draw data.
- Add include-only Slang files to shader build dependencies so editing shared
  material includes triggers shader rebuilds.

## Shader Organization

The current organization should evolve toward this layout without requiring a
large directory move:

```text
shaders/
  object_data_common.slang
  lighting_structs.slang
  brdf_common.slang
  pbr_material_common.slang
  surface_normal_common.slang
  alpha_mask_common.slang
  gbuffer.slang
  forward_transparent.slang
  deferred_directional.slang
  point_light.slang
  tiled_lighting.slang
```

`object_data_common.slang` is the single source for shader-side `ObjectBuffer`.
Any change to `container::gpu::ObjectData` must be reflected there and in the
static layout asserts in `SceneData.h`.

`pbr_material_common.slang` owns texture sampling and material parameter
evaluation. Pass shaders should assemble the inputs and call helpers rather
than reimplementing glTF texture channel rules.

`brdf_common.slang` owns BRDF math, Fresnel, GGX distribution, geometry terms,
F0 from IOR/specular, and world-position reconstruction.

## PBR Evaluation

Base material evaluation should follow glTF channel conventions:

- Base color texture is sRGB. Alpha contributes to material alpha.
- Metallic-roughness texture is data/UNORM. Roughness is G, metallic is B.
- Normal, occlusion, clearcoat, thickness, sheen roughness, iridescence, and
  transmission textures are data/UNORM.
- Emissive, specular color, sheen color, and diffuse/base-color textures are
  color/sRGB.
- Occlusion uses R and blends with `occlusionStrength`.
- Clearcoat roughness uses G.
- Sheen roughness uses A.
- Volume thickness uses G.
- Iridescence thickness uses G.

Direct lighting uses Cook-Torrance GGX:

- GGX/Trowbridge-Reitz normal distribution.
- Smith masking-shadowing.
- Schlick Fresnel.
- Metallic workflow diffuse suppression.
- F0 derived from IOR and `KHR_materials_specular` for dielectrics, then mixed
  with base color for metals.

IBL remains required:

- Irradiance cubemap for diffuse.
- Prefiltered environment cubemap for specular.
- BRDF LUT for split-sum specular.
- Multiple-scattering compensation for rough specular IBL. The renderer uses
  the BRDF LUT to estimate missing GGX single-scatter energy and feeds that
  energy back into the specular environment term.
- GTAO/occlusion applied to indirect diffuse and specular occlusion.

## Supported PBR Features

Current renderer support for the requested PBR stack:

| Feature | Current support |
|---|---|
| Metallic-roughness | Core glTF factors and packed G/B metallic-roughness texture in deferred and forward paths |
| Normal | Tangent-space normal map with scale, double-sided face correction, and clearcoat normal in forward transparent path |
| Emissive | Emissive texture/factor plus `KHR_materials_emissive_strength` |
| Occlusion | R-channel occlusion texture with `occlusionStrength`, combined with GTAO in deferred lighting |
| Specular | `KHR_materials_specular` intensity/color import; deferred packs scalar F0 magnitude, forward keeps color F0 |
| Clearcoat | Factor, roughness, textures, and clearcoat normal; compact opaque approximation and forward second specular lobe |
| Sheen | Sheen color/roughness factors and textures with grazing-angle cloth approximation |
| Transmission + volume | Thin-surface transmission, thickness texture/factor, attenuation color/distance, and Beer-Lambert absorption in forward transparent path |
| Iridescence | Factor, IOR, thickness range/texture with angle/thickness tint approximation |
| Dispersion | Optional per-channel IOR spread for transmitted forward transparent sampling |
| Multiple scattering compensation | GGX specular IBL compensation in deferred and forward paths, including clearcoat IBL |

## Feature Details

Metallic-roughness:

- Factors come from core glTF `pbrMetallicRoughness`.
- The packed texture follows glTF channels: G is roughness, B is metallic.
- Separate legacy roughness and metalness textures are still accepted for
  compatibility.

Normal:

- Normal maps are sampled as data textures.
- `normalTexture.scale` is applied in shader.
- Double-sided materials flip tangent-space evaluation on back faces.
- Clearcoat can use its own normal map in the forward transparent path.

Emissive:

- Emissive factor and texture are multiplied together.
- `KHR_materials_emissive_strength` allows values beyond `[0, 1]`.
- Unlit materials route base color through the emissive/no-light path.

Occlusion:

- Occlusion uses the R channel and blends with `occlusionStrength`.
- Deferred lighting combines material AO with GTAO.

Specular:

- `KHR_materials_specular` intensity and color are imported.
- Forward shading keeps colored F0.
- Deferred shading currently packs scalar dielectric F0 magnitude into the
  emissive alpha G-buffer channel. This preserves IOR and specular intensity,
  but not per-channel specular color.

Clearcoat:

- Factor, roughness, factor textures, roughness texture, and clearcoat normal
  texture are imported.
- Forward transparent shading evaluates a second specular lobe.
- Opaque deferred shading uses a compact roughness/F0 approximation until a
  material ID or additional G-buffer channel is added.

Sheen:

- Sheen color and roughness factors/textures are imported.
- The current shader uses a grazing-angle cloth reflection approximation.

Transmission and volume:

- Transmission is treated as glTF thin-surface transmission, not generic
  refraction.
- Volume adds thickness and Beer-Lambert attenuation.
- The high-quality path is currently forward transparent/OIT.

Iridescence:

- Factor, IOR, thickness range, and thickness texture are imported.
- The shader uses an angle/thickness tint approximation.

Dispersion:

- Dispersion is optional and only affects transmitted forward transparent
  sampling.
- It offsets per-channel IOR and samples the environment with separated RGB
  directions.

Multiple scattering compensation:

- Specular IBL uses `EvaluateSpecularIblMultipleScattering()` in
  `brdf_common.slang`.
- Deferred lighting, forward transparent lighting, and clearcoat IBL all use
  the compensated split-sum result.
- Direct lights still use the standard single-scattering Cook-Torrance BRDF.

## glTF Extension Semantics

Supported importer target:

| Extension | Import data | Shader behavior |
|---|---|---|
| Core metallic-roughness | base color, metallic, roughness, normal, AO, emissive, alpha | Deferred and forward |
| `KHR_materials_pbrSpecularGlossiness` | diffuse, specular, glossiness, textures | Converted to metallic-roughness approximation; glossiness alpha converted to roughness |
| `KHR_materials_specular` | specular intensity and specular color | Modifies dielectric F0 |
| `KHR_materials_clearcoat` | clearcoat, roughness, normal | Adds second specular lobe in forward; compact approximation in G-buffer |
| `KHR_materials_transmission` | transmission factor/texture | Thin-surface transmission term |
| `KHR_materials_volume` | thickness, attenuation color/distance | Beer-Lambert absorption for transmitted color |
| `KHR_materials_sheen` | sheen color and roughness | Grazing-angle cloth reflection approximation |
| `KHR_materials_iridescence` | factor, IOR, thickness range/texture | Angle/thickness color-shift approximation |
| `KHR_materials_emissive_strength` | emissive multiplier | Multiplies emissive output beyond `[0, 1]` |
| `KHR_materials_unlit` | unlit flag | Emits base color through emissive/no-light path |
| `KHR_materials_ior` | IOR | Controls dielectric F0 and transmission eta |
| `KHR_materials_dispersion` | dispersion | Offsets per-channel eta for transmitted color |

Do not conflate these:

- Transmission is thin-surface light passage.
- Volume adds interior absorption.
- IOR changes Fresnel and volume refraction direction.
- Dispersion is wavelength-dependent refraction and should remain optional.

## Deferred Versus Forward

Opaque deferred path:

- `gbuffer.slang` evaluates base material inputs and writes compact material
  channels.
- Deferred lighting sees albedo, alpha, normal, metallic, roughness, AO,
  emissive, and scalar dielectric F0. The F0 channel is evaluated from IOR,
  `KHR_materials_specular` intensity, and the magnitude of specular color before
  packing, so deferred lights are not locked to the default 0.04 dielectric F0.
- Layered effects are approximated if they cannot fit into current G-buffer
  outputs.
  Full colored specular still requires a material/object ID or an extra packed
  G-buffer channel.

Transparent forward path:

- `forward_transparent.slang` has access to full object/material data.
- It can evaluate clearcoat, sheen, iridescence, transmission, volume, IOR, and
  dispersion more directly.
- OIT stores the final transparent color and alpha.

Long-term option:

- Add a material/object index G-buffer channel or compact material ID.
- Let deferred lighting fetch `GpuMaterial` from an SSBO.
- This enables full layered material evaluation for opaque objects without
  adding many G-buffer attachments.

## Pipeline Strategy

Current buckets should stay:

- Depth prepass.
- Shadow depth.
- Opaque G-buffer.
- Alpha-mask G-buffer.
- Transparent forward/OIT.
- Debug/normal/wireframe passes.

Do not create a pipeline per extension. Use object/material flags and texture
indices inside the shader. Later, when profiling shows branch cost matters, add
coarse permutations:

- Base opaque PBR.
- Normal-mapped opaque PBR.
- Layered opaque PBR.
- Transparent/transmission PBR.
- Unlit.

Specialization constants are useful for coarse variants, but they are not a
replacement for per-material data in glTF scenes.

## Import Rules

The importer should:

- Give `KHR_materials_pbrSpecularGlossiness` precedence over core
  metallic-roughness when present.
- Preserve alpha mode: OPAQUE, MASK, BLEND.
- Promote opaque materials to blend when opacity or transmission requires it.
- Classify texture color spaces by usage before upload.
- Store extension factors even if no texture exists.
- Resolve extension textures directly from glTF texture indices when possible.
- Keep legacy `refraction` names as import compatibility, but store them in
  transmission fields internally.

## Debug Views

Recommended debug output modes:

- Base color.
- Normal.
- Metallic.
- Roughness.
- AO.
- Emissive.
- Specular/F0.
- Clearcoat.
- Sheen.
- Transmission.
- Thickness/volume absorption.
- IOR.
- Alpha.
- Diffuse only.
- Specular only.
- IBL only.

These views are more valuable than trying to visually inspect every extension
through final lit output.

## Implementation Roadmap

Phase 1 - Stabilize current PBR:

- Keep shared `ObjectBuffer` in `object_data_common.slang`.
- Keep shared material helpers in `pbr_material_common.slang`.
- Keep transmission terminology in CPU and shader fields.
- Build sample-model tests for imported extension factors and texture indices.

Phase 2 - Improve correctness:

- Add MikkTSpace tangent generation or validation for glTF assets without
  trustworthy tangents.
- Support `KHR_texture_transform` and alternate texture coordinate sets.
- Improve colored specular propagation into deferred lighting, either with a
  material ID or extra packed G-buffer data. The current G-buffer preserves F0
  magnitude but not per-channel F0 color.
- Add reference scene screenshots for Khronos sample models.

Phase 3 - Material SSBO:

- Split material data out of `ObjectData`.
- Store `materialIndex` per object/draw.
- Let forward and deferred passes fetch `GpuMaterial`.
- Move feature flags from object flags into material flags.

Phase 4 - Better glass:

- Add screen-space transmission/background sampling for thin glass.
- Use volume thickness from mesh scale or thickness texture consistently.
- Make dispersion optional and low-cost by default.

Phase 5 - Optimization:

- Profile branch divergence in the uber-shader.
- Add coarse shader permutations only where profiling proves value.
- Consider compact 16-bit or packed material fields after correctness is stable.

## Validation Checklist

Build and tests:

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --build out\build\windows-debug'
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && ctest --test-dir out\build\windows-debug --output-on-failure'
```

Visual sample set:

- `DamagedHelmet` for base metallic-roughness.
- `SpecularTest` for `KHR_materials_specular`.
- `ClearCoatTest` and `ClearcoatWicker` for clearcoat.
- `SheenCloth` and `GlamVelvetSofa` for sheen.
- `TransmissionTest` and `TransmissionRoughnessTest` for transmission.
- `DragonAttenuation` or `MosquitoInAmber` for volume attenuation.
- `IridescenceDielectricSpheres` for iridescence.
- `EmissiveStrengthTest` for emissive strength.
- `UnlitTest` for unlit.
- `SpecGlossVsMetalRough` for specular-glossiness conversion.
