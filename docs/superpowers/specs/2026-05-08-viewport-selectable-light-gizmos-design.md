# Viewport Selectable Light Gizmos Design

Date: 2026-05-08
Status: Approved for implementation

## Goal

Make every visible editable light selectable from the viewport. Phase B made
generated, imported, and manual lights editable from the inspector; Phase C
connects the rendered light gizmos to the same editable-light identity so a
click on a visible light marker selects that light, synchronizes the inspector,
and enables existing move, rotate, and scale transforms.

## Scope

In scope:

- Directional, point, spot, and area light gizmos built from
  `EditableLightEntity` data.
- Stable editable-light identity attached to each visible gizmo.
- CPU viewport picking for light gizmos using camera projection and framebuffer
  cursor coordinates.
- Selection routing through `LightingManager::selectEditableLight`.
- Selected-light visual emphasis in the existing light-gizmo overlay.
- Focused planner, picker, and source-contract tests.

Out of scope:

- GPU pick-ID rendering for light gizmos.
- Undo and redo for light edits.
- Persistent scene serialization of manual lights.
- Qt editor shell work.
- Multiple simultaneous directional lights beyond the current renderer model.

## Architecture

`LightingManager` remains the owner of editable light state. The deferred light
gizmo planner becomes the boundary that converts editable lights into two
outputs: draw push constants for the existing overlay recorder and lightweight
viewport hit targets for CPU picking. `RendererFrontend` asks the planner for a
fresh plan when selecting in the viewport and routes a successful hit through
the same selection cleanup path used by the inspector.

The picker is intentionally CPU-side in this phase because the existing overlay
is not part of the transparent GPU pick pass. It projects each gizmo center
through the current camera, applies a bounded pixel hit radius, ignores
off-screen or behind-camera markers, and returns the closest cursor hit. This is
small, testable, and avoids changing render-pass resources.

## Data Flow

`LightingManager::updateLightingData()` rebuilds `editableLights_` as it does in
Phase B. `drawLightGizmos()` passes those entities to
`buildDeferredLightGizmoPlan()`. The plan stores one draw push constant and one
selection target per visible gizmo. Selected lights receive a larger marker and
selection tint.

For viewport clicks, `RendererFrontend::selectMeshNodeAtCursor()` checks light
gizmos before mesh/BIM pick routing. The hit radius is small, so clicking a
visible light marker selects the light even when it is drawn over model
geometry, while clicks outside the marker still follow existing GPU and CPU
scene/BIM selection. If a light is hit, the frontend clears mesh and BIM
selection, selects the editable light in `LightingManager`, clears hover state,
and updates the status message.

## Error Handling

Invalid or missing light IDs are not selectable. Empty editable-light spans fall
back to the legacy directional-plus-point gizmo path used by existing tests.
Invalid camera projections, zero-size framebuffers, non-finite positions, and
behind-camera gizmos are ignored by the picker. If no hit is found, selection
continues through the existing GPU pick and CPU scene/BIM pick flow.

## Testing

Tests must cover:

- Editable light gizmo plans preserve `EditableLightId` metadata and selected
  state.
- Area and spot lights produce selectable gizmos, not only point lights.
- Selected lights draw with a larger or tinted marker.
- Picker ignores invalid targets and returns the nearest projected hit under
  the cursor.
- `RendererFrontend` source contract checks prove viewport selection calls the
  light picker before clearing selection on an empty GPU pick.
