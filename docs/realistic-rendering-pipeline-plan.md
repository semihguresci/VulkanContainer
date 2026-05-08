# Realistic Rendering Pipeline Plan

Status: renderer implementation complete for the planned lighting, exposure,
shadow, material, and validation-harness code paths. Visual regression fixture
assets and platform goldens remain data-population work.
Date: 2026-05-01
Scope: lighting, shadows, HDR environment, exposure, post-processing, and the
validation needed to make the renderer behave like a physically grounded
real-time pipeline.

## Implementation Status

The first implementation pass addresses the original review findings and closes
the highest-impact calibration gaps:

- HDR environment loading preserves source radiance by default.
- Exposure is a runtime post-process control.
- Environment intensity is explicit runtime lighting state.
- Point-light attenuation uses inverse-square falloff with finite range fade.
- glTF `KHR_lights_punctual` point lights are imported as authored scene lights
  and merged ahead of generated/debug lights.
- Directional CSM projections use stable square extents, texel snapping, and
  uploaded cascade metadata.
- Receiver-side shadow bias, filtering, cascade blending, and raster caster bias
  are runtime settings.
- Transparent/OIT lighting samples the same directional CSM path as opaque
  lighting.
- Deferred directional, point, and tiled lighting can recover `GpuMaterial`
  through material metadata preserved in the G-buffer.
- Deferred specular/F0 color is texture-sourced through a dedicated G-buffer
  attachment.
- glTF tangent fallback generation uses MikkTSpace.
- glTF spot lights are imported into the shared local-light buffer and rendered
  with cone attenuation in forward, tiled, and per-light deferred paths.
- Auto exposure uses a GPU scene-luminance histogram and GPU-resident
  percentile adaptation. The post-process tone mapper reads the GPU exposure
  state directly; CPU readback is only debug/fallback state.
- Rectangular and disk area lights have a shared CPU/shader data model, glTF
  custom-extension import plumbing, GPU SSBO upload, and direct lighting in
  deferred opaque plus forward transparent passes. The direct shader path uses
  deterministic four-sample emitter integration rather than a single closest
  point, improving near-field shape response without adding LUT descriptors.
- Deferred direct lighting resolves texture-only layered material terms and
  applies transmission-weighted thin-surface direct lighting where the compact
  G-buffer path can recover the source material.
- Shader/host layouts have static assertions and convention tests for the
  shared buffers and push constants touched by this plan.

Remaining validation/data work:

- Visual regression capture and screenshot comparison are implemented as an
  opt-in GPU runtime harness. Calibrated validation assets and committed
  platform goldens still need to be populated.
- Higher-cost area-light refinements, such as LTC lookup textures or more
  adaptive importance sampling, are now optional quality/performance tradeoffs
  instead of required plan work.

## Objective

Move the renderer from a feature-rich but tuned lighting stack to a calibrated
scene-linear HDR pipeline where:

- HDR environment maps keep their source radiance.
- Light intensities have explicit units and predictable falloff.
- Exposure and tone mapping are camera/post-process controls, not hidden scale
  factors.
- Shadow quality is stable across camera movement and tunable per scene.
- Opaque, masked, transparent, and transmissive materials receive consistent
  direct and indirect lighting.
- Visual quality is guarded by repeatable scenes and tests.

## Current Frame Shape

The active renderer already has the main passes needed for a realistic pipeline:

```text
Frustum cull
  -> depth prepass
  -> Hi-Z / occlusion cull
  -> G-buffer
  -> OIT clear
  -> shadow cascade cull
  -> shadow cascades
  -> depth read-only transition
  -> tile light cull
  -> GTAO
  -> deferred lighting
  -> transparent OIT
  -> exposure histogram/adaptation
  -> OIT resolve
  -> bloom
  -> post-process / tone map
```

Important files:

- `src/renderer/lighting/EnvironmentManager.cpp`
- `src/renderer/lighting/LightingManager.cpp`
- `src/renderer/shadow/ShadowManager.cpp`
- `src/renderer/core/FrameRecorder.cpp`
- `include/Container/utility/SceneData.h`
- `shaders/brdf_common.slang`
- `shaders/deferred_directional.slang`
- `shaders/forward_transparent.slang`
- `shaders/post_process.slang`

## Review Findings

### HDR Environment Energy

`EnvironmentManager::loadHdrEnvironment()` clamps HDR texels and rescales the
loaded map to a fixed mean luminance before IBL generation. That makes the
renderer easier to tune, but it discards real sun/sky intensity and prevents
calibrated exposure.

Target behavior:

- Preserve source HDR radiance by default.
- Add explicit `environmentIntensity` or `environmentExposureOffsetEv` controls.
- Keep optional firefly filtering as an import/debug option, not a mandatory
  energy rewrite.

### Hardcoded Exposure

`post_process.slang` multiplies final HDR by a fixed exposure value before ACES
tone mapping. This is the main blocker for physical light units because every
light and HDRI is currently judged against a hidden post-process scale.

Target behavior:

- Add a `PostProcessSettings`/camera exposure field uploaded to the shader.
- Start with manual EV100 or linear exposure.
- Auto exposure is now implemented with a GPU luminance histogram, percentile
  metering, and GPU-resident exposure adaptation.

### Point-Light Falloff

`SmoothRangeAttenuation()` only smooths lights to zero at a chosen radius. It
does not model inverse-square falloff, so light intensity is scene-tuned rather
than photometric.

Target behavior:

- Use inverse-square attenuation for physical lights.
- Keep radius as a finite cutoff/fade range, not the primary energy model.
- Store light intensity in explicit units, such as candela for point/spot
  lights and lux or radiance-like scene units for directional light.

### Cascade Stability

`ShadowManager::computeCascadeViewProj()` computes a cascade radius, but the
projection still fits changing light-space slice bounds and only snaps the
center. As the camera rotates or the frustum slice shape changes, cascade extents
can change, which changes shadow texel scale and can shimmer.

Target behavior:

- Use a square, sphere-derived cascade extent.
- Snap the projection origin to shadow texel units.
- Upload per-cascade texel size and depth range for bias/filter calculations.

### Transparent Shadows

The transparent/OIT path evaluates directional lighting without CSM shadowing.
Transparent, alpha-blended, and transmission materials can look detached from
the rest of the scene.

Target behavior:

- Share CSM shadow sampling with `forward_transparent.slang`.
- Apply direct-light shadowing to transparent lighting.
- Later, add alpha-tested caster behavior and optional transmission-aware shadow
  tinting.

## Target Pipeline Contract

### Color And Units

- All lighting accumulates in scene-linear HDR.
- Texture color spaces stay explicit at import and sampling.
- HDR environment maps are not normalized destructively.
- Tone mapping and display conversion happen only in post-process.
- Bloom consumes scene-linear HDR before tone mapping.

### Light Data

The lighting data model should separate physical controls from debug/generator
controls:

- Directional light: direction, color temperature or RGB color, illuminance or
  intensity scale.
- Point light: position, color, luminous intensity, finite fade range.
- Spot light: point-light fields plus direction and cone angles.
- Area light: position, color/intensity, finite fade range, emitting normal,
  shape type, and tangent/bitangent half-size fields for rectangular and disk
  emitters.

### Shadows

- Directional CSM remains the primary shadow solution.
- Cascade projection must be stable and texel-snapped.
- Bias and filter settings must be data-driven, not hardcoded in shaders.
- Shadow debug views should show cascade index, shadow factor, bias, texel
  density, and split distances.

### Materials

- Opaque deferred lighting should either store enough G-buffer data for layered
  PBR, or store object/material IDs so lighting can fetch full material data.
- Transparent forward/OIT lighting should share the same BRDF helpers and direct
  shadow path as opaque lighting.
- Clearcoat, sheen, iridescence, transmission, and volume should not be folded
  into emissive or roughness except as an explicitly documented fallback.

## Implementation Phases

### Phase 0: Baseline And Controls

Goal: make current behavior measurable before changing energy.

Tasks:

- Add a small runtime settings struct for exposure, environment intensity,
  shadow quality, and physical-light mode.
- Expose these values in ImGui.
- Add debug output modes for scene-linear luminance, shadow factor, and
  environment-only lighting.
- Capture baseline screenshots for Sponza, material spheres, and a simple
  shadow test scene.

Acceptance:

- Existing defaults can reproduce the current look.
- Debug views reveal luminance and shadow behavior without editing shaders.

### Phase 1: HDR And Exposure Calibration

Goal: remove hidden energy normalization and move brightness control to camera
and environment settings.

Tasks:

- Remove mandatory HDR mean-luminance normalization.
- Keep optional firefly clamp behind a setting.
- Add `exposure` or `exposureEv100` to post-process push constants or a
  dedicated post-process UBO.
- Add `environmentIntensity` or EV offset for IBL sampling.
- Audit bloom threshold/intensity so thresholds are expressed in scene-linear
  units.

Acceptance:

- A known HDRI can be loaded without source radiance destruction.
- Manual exposure can brighten/darken the entire scene predictably.
- Bloom behavior remains stable after exposure changes.

### Phase 2: Physical Local Lights

Goal: make point-light brightness and range predictable.

Tasks:

- Extend `PointLightData` or introduce a versioned light SSBO with physical
  intensity and fade range.
- Replace pure smooth-radius attenuation with inverse-square falloff multiplied
  by a finite-range fade.
- Add glTF `KHR_lights_punctual` import support if missing.
- Keep generated light grids available as a debug preset, but avoid making them
  the realism baseline.

Acceptance:

- Doubling distance reduces physical point-light energy by roughly 4x before
  cutoff fade.
- Imported punctual lights match expected placement and relative brightness.
- Clustered/tiled culling still works with the new light layout.

### Phase 3: Stable, Tunable Directional Shadows

Goal: reduce shimmer, make bias/filtering scene-scaled, and expose quality
controls.

Tasks:

- Change cascade projection to square sphere-derived extents.
- Snap cascade origin to shadow texel units.
- Upload per-cascade texel size, world-space radius, near/far depth, and split
  depth.
- Move normal bias, slope bias, receiver-plane bias scale, filter radius, and
  cascade blend size into shadow settings.
- Add debug views for raw shadow factor and cascade texel density.
- Apply CSM shadows in `forward_transparent.slang`.

Acceptance:

- Slow camera rotation does not visibly resize cascade texels.
- Bias can be tuned without shader edits.
- Transparent objects receive directional shadows consistently with opaque
  objects.

### Phase 4: Deferred Material Fidelity

Goal: reduce differences between opaque deferred and transparent forward
material evaluation.

Tasks:

- Add object ID or material ID to the G-buffer, or provide a compact material
  index attachment.
- Let deferred lighting fetch full `GpuMaterial` data where needed.
- Move clearcoat, sheen, iridescence, and transmission approximations out of
  emissive/roughness packing where possible.
- Keep compact G-buffer fallback paths documented for lower-quality modes.

Acceptance:

- Material spheres for metallic/roughness, clearcoat, sheen, and iridescence
  match between deferred and forward reference paths within expected limits.
- Opaque layered material behavior is explained by stored data, not hidden
  approximations.

### Phase 5: Validation Scenes And Regression Tests

Goal: make lighting changes reviewable without relying only on subjective
screenshots.

Tasks:

- Add a material calibration scene: diffuse gray, chrome, rough metal, dielectric
  plastic, clearcoat, glass/transmission.
- Add a shadow stability scene: floor, vertical blocker, moving camera path.
- Add luminance histogram/adaptation checks for exposure.
- Add screenshot comparison gates where the environment supports Vulkan
  rendering.
- Keep CPU tests for math invariants such as cascade split ordering, texel
  snapping, and light attenuation behavior.

Acceptance:

- CPU tests cover exposure math, attenuation, cascade projection bounds, and
  render graph ordering.
- Visual regression assets can catch large lighting/shadow changes.

## Validation Artifact Contract

The repository now has a CPU-only fixture contract plus an opt-in GPU screenshot
harness:

```text
tests/
  realistic_rendering_validation_tests.cpp
  fixtures/rendering/
    realistic_visual_regression.schema.json
    realistic_visual_regression.fixtures.json
  visual-regression/
    golden/{platform}/{scene-id}.png        # future committed baselines
test_results/
  visual-regression/
    candidate/{platform}/{scene-id}.png     # local or GPU-CI output
    diff/{platform}/{scene-id}.png          # local or GPU-CI output
    manifest/{platform}/latest.json         # capture paths, metrics, status
```

`tests/fixtures/rendering/realistic_visual_regression.fixtures.json` is the
source of truth for active and planned visual scenes. The GPU harness renders
active scenes through `VulkanSceneRenderer`, writes candidates under
`test_results/visual-regression/candidate/{platform}`, emits diff images under
`test_results/visual-regression/diff/{platform}` when a golden exists, and
compares against the configured per-platform thresholds. Each GPU run also
writes `test_results/visual-regression/manifest/{platform}/latest.json` with
the capture command results, candidate/golden/diff paths, metrics, and skipped
scene reasons. Planned validation assets under `models/validation/` remain
metadata-only until those scenes and goldens are authored.

The first scene set should remain small and calibrated:

- `calibration_material_grid`: matte gray card, chrome, rough metal, clearcoat
  plastic, and thin glass/transmission under fixed environment and directional
  light settings.
- `calibration_exposure_steps`: EV-spaced patches rendered through the
  scene-linear luminance/debug path to verify manual exposure and bloom inputs.
- `calibration_area_lights`: rectangular and disk emitters on simple receiver
  geometry to catch area-light import, orientation, range, and falloff changes.
- `shadow_cascade_stability`: floor and vertical blocker with a short camera
  motion path to catch cascade snapping, texel density, bias, filtering, and
  transparent shadow regressions.

Screenshot comparison is opt-in. Run it with
`CONTAINER_RUN_GPU_VISUAL_REGRESSION=1` and
`CONTAINER_VISUAL_REGRESSION_PLATFORM=<bucket>`. Set
`CONTAINER_VISUAL_REGRESSION_ALLOW_MISSING_GOLDENS=1` when bootstrapping a new
platform bucket so candidates can be generated before goldens are committed.
Set `CONTAINER_VISUAL_REGRESSION_PROMOTE_GOLDENS=1` to copy missing goldens
from candidates, or `CONTAINER_VISUAL_REGRESSION_OVERWRITE_GOLDENS=1` to
refresh existing goldens intentionally. Use separate platform buckets because
Vulkan driver differences can affect filtered shadows, tone mapping roundoff,
and image layout precision. Keep the default CI path on the CPU validator plus
skipped GPU harness unless a Vulkan runner is explicitly configured.

## File Ownership Guide

Expected edit areas by phase:

| Phase | Files |
|---|---|
| Exposure/HDR | `EnvironmentManager.*`, `FrameRecorder.*`, `SceneData.h`, `post_process.slang`, `GuiManager.*` |
| Physical lights | `LightingManager.*`, `SceneData.h`, `lighting_structs.slang`, `brdf_common.slang`, `area_light_common.slang`, `tiled_lighting.slang`, `forward_transparent.slang` |
| Shadows | `ShadowManager.*`, `SceneData.h`, `lighting_structs.slang`, `shadow_common.slang`, `deferred_directional.slang`, `forward_transparent.slang`, `GuiManager.*` |
| Material fidelity | `SceneData.h`, `FrameResourceManager.*`, `gbuffer.slang`, `deferred_directional.slang`, `forward_transparent.slang`, material import/upload code |
| Validation | `tests/`, `docs/`, fixture scenes/assets, optional screenshot tooling |

## Non-Goals For The First Pass

- Full path tracing or offline reference rendering in-engine.
- High-sample or LUT-based area-light integration beyond the current
  four-sample real-time path.
- Global illumination beyond IBL and screen-space occlusion.
- Replacing the current render graph or Vulkan resource ownership model.

## Open Questions

- Which unit convention should the renderer expose first: EV100 + cd/lux, or a
  simpler linear exposure and intensity scale with documented migration path?
- Should HDR firefly filtering happen at asset import time, runtime load time, or
  as an optional IBL prefilter input clamp?
- Should the G-buffer store material ID, object ID, or both?
- What visual test harness should own screenshot capture and image comparison?
