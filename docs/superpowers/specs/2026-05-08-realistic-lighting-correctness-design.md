# Realistic Lighting Correctness Design

Date: 2026-05-08
Status: Approved for implementation planning

## Goal

Improve realistic lighting quality by fixing correctness first. The first
implementation phase focuses on shadow behavior, winding and culling
consistency, surface-normal handling, camera-perspective stability, and
physically consistent PBR evaluation. Editor light controls and final image
polish are intentionally later phases.

The target behavior is precise: blocker geometry casts shadows consistently,
receiver surfaces shade consistently, camera movement does not change shadow
truth, and normals affect shading and receiver bias without making geometry
transparent to light.

## Scope

In scope:

- Directional cascaded shadow correctness.
- Local shadow correctness for point, spot, and area lights.
- Consistent draw routing for normal, winding-flipped, mirrored, double-sided,
  alpha-mask, and no-cull geometry.
- Camera-depth reconstruction and shadow-space convention checks.
- Open-world and closed-room validation scenes.
- PBR sanity checks after shadow correctness: finite values, energy-plausible
  direct lighting, attenuation, IBL, AO, and tone mapping interactions.

Out of scope for this phase:

- Selectable, moveable, and rotateable light editor features.
- Multiple directional lights beyond the current renderer architecture.
- New global illumination systems.
- Broad material-feature expansion unrelated to the shadow and lighting
  correctness issues.

## Architecture

The first phase is a lighting correctness foundation. It establishes renderer
contracts that every lighting pass follows:

- One shared convention for world, view, clip, shadow, framebuffer UV, and
  reverse-Z depth.
- One surface visibility rule used consistently by depth prepass, G-buffer,
  directional shadow, local shadow, and transparent or alpha-mask paths.
- Shadow casters selected by geometry and material behavior, not by surface
  normals.
- Normals used for shading and receiver bias only.
- Separate validation for directional cascaded shadows and local light shadows,
  because they fail through different projection and sampling paths.

This phase should make surfaces block light correctly regardless of winding
repair, mirrored transforms, camera angle, open or closed scene layout, and
material culling mode.

## Components

### Convention Audit

Verify reverse-Z compare direction, negative viewport Y handling, framebuffer
UV reconstruction, cascade split depth, and shadow matrix use. This component
addresses camera-perspective-dependent errors and should reference the existing
coordinate convention tests and shader reconstruction helpers.

### Caster Routing

Make depth, G-buffer, directional shadow, and local shadow passes agree on each
draw route: back-cull, front-cull, or no-cull. Winding-flipped, mirrored,
double-sided, alpha-mask, and no-cull cases must route consistently across all
passes.

### Shadow Sampling

Fix directional and local shadow sampling so receiver bias uses normals only to
offset the receiver safely. Normals must not decide whether a surface blocks
light. Blocker truth comes from shadow depth rendered from the light view.

### Validation Scenes

Add targeted tests and fixtures for:

- A single wall blocker in an open scene.
- Closed room shadow containment.
- Flipped-winding blocker.
- Mirrored-transform blocker.
- Double-sided thin-surface blocker.
- Camera angle sweep for shadow stability.
- Point, spot, and area local-light blockers.

### PBR Consistency Check

After shadow correctness, verify direct-light BRDF behavior, attenuation, IBL
and AO application, exposure, and tone mapping against calibrated reference
scenes. This check should not mask shadow bugs with exposure or bloom changes.

## Data Flow

Scene and import data produce mesh draw commands, material flags,
winding-flipped routing, object transforms, normal matrices, and bounds.
Visibility and culling decide draw eligibility, but they do not decide whether
a surface blocks light based on normals.

Depth prepass, G-buffer, directional shadow, and local shadow passes all consume
the same draw routing classification. Shadow maps store blocker depth from
light-space rendering only. Lighting reconstructs the receiver world position
from camera depth, selects the relevant shadow projection, applies receiver
bias, samples the shadow map, and then evaluates direct and indirect PBR
lighting.

Debug views should expose cascade selection, shadow factor, normals,
winding/cull route, and light-space depth so failures can be traced to a
specific stage.

## Error Handling And Debugging

The renderer should fail to stable, inspectable behavior when lighting data is
invalid:

- Non-finite reconstructed world positions, normals, light directions, or
  shadow matrix values use stable fallbacks.
- Shadow debug output distinguishes outside-shadow-map, no-caster-depth,
  receiver-bias, cascade-blend, and actually-shadowed cases where practical.
- CPU planner tests expose cull-route decisions so pass drift is caught before
  visual testing.
- Visual regression fixtures include numeric shadow and luminance probes, not
  screenshots only.
- Deterministic fixture lighting is used for correctness tests, while the
  runtime renderer keeps safe defaults for missing environment maps and lights.

## Testing Strategy

Implementation must use failing tests before production changes.

Required test coverage:

- Unit and planner tests for draw routing across normal, winding-flipped,
  mirrored, double-sided, alpha-mask, and local-shadow caster cases.
- Shader convention tests for world-position reconstruction, reverse-Z depth
  comparison, viewport Y inversion, and cascade selection.
- Visual regression fixtures for open-world blockers, closed rooms,
  flipped-winding blockers, mirrored blockers, camera-perspective stability,
  and local-light blockers.
- Numeric probes for lit-to-shadow luminance ratio, penumbra gradient, and
  stability when the camera moves.
- Existing renderer, shadow, deferred, and validation tests remain part of the
  verification set.

The pass condition is not that the image looks nicer. The pass condition is
that shadow truth is stable and physically plausible before later image-quality
work begins.

## Implementation Sequence

1. Add or update failing tests that isolate the suspected correctness failures.
2. Audit and document current pass routing and shader convention behavior.
3. Fix shared draw routing so depth, G-buffer, directional shadow, and local
   shadow pass decisions cannot drift.
4. Fix shadow-space reconstruction and sampling conventions for directional
   cascades.
5. Fix local shadow projection and sampling behavior for point, spot, and area
   lights.
6. Add deterministic open and closed scene visual fixtures.
7. Run the PBR consistency check and tune only correctness-related lighting
   behavior.

## Future Phases

After this foundation is verified:

- Add selectable, moveable, and rotateable editor lights for all visible lights,
  including live generated lights.
- Add UI and viewport controls for directional light direction.
- Improve final image quality through calibrated exposure, tone mapping, IBL
  intensity, AO balance, and bloom after shadows are trustworthy.
