# Rendering Space Interaction Plan

This plan defines how mouse and keyboard input should interact with the
rendering space. The goal is to make viewport control predictable while keeping
ImGui panels, model controls, debug toggles, and future object editing from
fighting over the same input events.

## Current Baseline

- `WindowInputBridge` forwards GLFW cursor, mouse button, key, resize, and
  focus callbacks to `InputManager`.
- `InputManager` supports RMB-look mode, raw mouse motion, and continuous
  WASD/Space/E/Ctrl/Q camera movement.
- `RendererFrontend::processInput()` owns debug key toggles for F6, F7, and F8.
- `GuiManager::isCapturingInput()` uses ImGui `WantCaptureMouse` and
  `WantCaptureKeyboard` to prevent camera input while editing UI controls,
  except while look mode is already active.

This is enough for a basic fly camera, but it does not clearly separate render
surface input from UI input, lacks scroll input, does not define selection or
orbit behavior, and leaves shortcut ownership implicit.

## Implementation Snapshot - 2026-05-02

The current implementation has moved beyond the original baseline:

- `RenderSurfaceInteractionController` owns viewport gesture routing for
  selection, hover, fly-look, orbit, pan, dolly, transform drag, debug
  shortcuts, and Escape cancellation.
- `CameraController` supports pivot-aware orbit, drag pan, view-basis movement,
  dolly, frame/reset,
  perspective/orthographic projection, animated top/bottom/left/right/front/back
  view presets, and orthographic zoom.
- GPU picking is the primary selection path. Opaque selection reads the pick and
  depth outputs so occluded meshes are not selected through visible geometry.
  Transparent geometry is handled by a dedicated transparent-pick pass.
- Selection visualization uses a renderer-native orange outline/stroke around
  the selected object instead of drawing every vertex or triangle edge.
- Transform tooling uses renderer-native gizmo passes for selected objects, with
  move and scale handles sized from the selected bounds and camera distance.
- Transform drags are gated by a screen-space gizmo hit test, so empty viewport
  drags no longer move, rotate, or scale the selected object.
- Transform snapping is available from the viewport controls and the `S`
  shortcut while a transform tool is active. It snaps translation relative to
  drag start in 0.25 unit steps, rotation in 15 degree steps, and scale in 0.1
  steps.
- Move-gizmo dragging now follows the projected handle direction on screen.
  Local/world orientation controls the axis basis; unconstrained move stays on
  the camera view plane.
- Orbit, view presets, projection toggles, keyboard movement, and dolly now use
  the selected surface point as the navigation pivot after mouse picking.
  Visible selected-object bounds remain the fallback for UI/restored selections
  that do not have a picked point. BIM/IFC auxiliary selections provide the
  same fallback from their element metadata.
- The viewport can switch between Revit-style and Blender-style middle-mouse
  navigation. Revit style is the default for BIM/editor workflows: MMB pans and
  Shift+MMB orbits. Blender style keeps MMB orbit and Shift+MMB pan.
- Arrow keys move the camera along the active view right/up basis when the
  viewport owns keyboard input. Shift makes the keyboard movement coarser and
  Ctrl makes it finer.
- The viewport navigation widget provides clickable axis endpoints, a labeled
  view cube, perspective/orthographic switching, free rotation, and direct
  panning.

Remaining work is mostly interaction polish: marquee selection, double-click
frame selection, undo/redo integration, configurable bindings, and automated
tests for the interaction state machine.

## Interaction Goals

- Make the render surface feel like a viewport, not only a fly camera.
- Preserve the existing fast debug workflow for rendering engineers.
- Keep ImGui text fields, sliders, combo boxes, and drag controls fully
  protected from viewport shortcuts.
- Support both large scene navigation and precise inspection of selected
  geometry.
- Define input ownership in one place so future picking, gizmos, and viewport
  tools can be added without scattering conditions through renderer code.

## Rendering Space Definition

The rendering space is the area that displays the Vulkan scene and is not
currently owned by an active ImGui widget or window.

For the current full-window renderer, the rendering space is the whole
framebuffer with ImGui windows overlaid. An input event belongs to the rendering
space only when:

1. The application window is focused.
2. No ImGui item wants that event, unless the viewport already owns an active
   drag or pointer lock.
3. The pointer is inside the scene viewport rectangle. Today that rectangle is
   the full framebuffer; later it can become an explicit docked viewport rect.

## Input Ownership Rules

Input should be routed by priority:

1. Window lifecycle events: resize, focus loss, close.
2. ImGui capture: text input, combo boxes, sliders, drag controls, menus, and
   hovered ImGui windows.
3. Active render-space gesture: look, orbit, pan, dolly, marquee, gizmo drag.
4. Render-space hover and shortcuts.
5. Global debug shortcuts that are deliberately allowed outside the viewport.

When a render-space gesture starts, the viewport owns that gesture until the
matching release or cancellation event. This prevents a drag from being stolen
if the cursor passes over an ImGui window. Focus loss and Escape cancel any
active gesture and release pointer lock.

## Interaction Modes

### Inspect Mode

Default mode with visible cursor.

- LMB click: select the mesh or scene node under the cursor.
- LMB drag on empty space: reserved for marquee selection.
- Double LMB click on a selectable item: frame that item.
- Hover: show optional lightweight object or primitive information.
- Mouse wheel: in perspective, dolly along the camera forward vector; in
  orthographic, zoom by changing orthographic view height.
- F: frame selected item; frame the whole scene when nothing is selected.
- Home: reset camera to scene bounds.
- Esc: clear current tool drag, then clear selection if no drag is active.

### Fly Look Mode

Fast first-person navigation for inspecting large scenes.

- Hold RMB: enter pointer-locked look mode.
- Release RMB: leave look mode and restore the cursor.
- Mouse move while RMB is held: yaw and pitch the camera.
- W/S: move forward/back on the ground plane.
- A/D: strafe left/right on the ground plane.
- Space or E: move up.
- Ctrl or Q: move down.
- Shift: fast multiplier.
- Alt or C: slow precision multiplier.
- Mouse wheel while RMB is held: adjust fly speed, not camera position.

This keeps the current behavior intact but makes speed control and cancellation
explicit.

### Orbit Mode

Precise inspection around a stable pivot.

- Alt+LMB drag: orbit around the current pivot.
- Blender navigation: MMB drag orbits when no object-editing tool is active.
- Revit navigation: Shift+MMB drag orbits.
- Pivot priority: selected surface point, selected item bounds center, cursor
  hit point, scene bounds center.
- Shift while orbiting: reduce sensitivity for precision.
- Double MMB click or F: recenter the pivot on the selected item or scene.

Orbit should not change the selected object. It only changes camera position and
orientation.

### Pan Mode

Move the camera target laterally without changing viewing direction.

- Revit navigation: MMB drag pans parallel to the camera image plane.
- Blender navigation: Shift+MMB drag pans parallel to the camera image plane.
- Alt+RMB drag outside fly-look capture: pan.
- Shift+RMB drag: pan parallel to the camera image plane.
- RMB drag in orthographic projection: pan instead of fly-look.
- Arrow keys: move in the camera image plane using the current camera right/up
  basis; Shift is coarse, Ctrl is fine.
- View navigation widget MMB or RMB drag on the circular disk: pan.
- Pan speed scales with distance from the camera to the active pivot.

### Dolly/Zoom Mode

Change distance while keeping the view direction stable.

- Wheel in perspective inspect/orbit mode: move along the current camera forward
  vector.
- Ctrl+wheel: fine dolly.
- Shift+wheel: coarse dolly.
- Wheel in orthographic mode: change orthographic view height.

### Transform Mode

Object editing should be a separate tool state, not mixed into camera controls.

- W: translate gizmo.
- E: rotate gizmo.
- R: scale gizmo.
- X/Y/Z after a transform key: constrain to axis.
- L: toggle local/world orientation.
- S: toggle transform snapping.
- Delete: remove selected editable object when supported.
- Esc: cancel active transform; if idle, exit transform tool.

Gizmo drags own pointer input until release. Camera gestures should not start
while a gizmo handle is hot or active.

Move-tool semantics:

- Free move always drags in the camera view plane, matching common Blender and
  Revit behavior for unconstrained object movement.
- Axis-constrained move projects the selected local/world axis to screen and
  maps mouse movement onto that visible handle direction.
- Local/world changes the handle orientation and constrained movement basis; it
  does not change free-move into an object-local screen plane.

## Shortcut Scope

Shortcuts should be classified instead of checked ad hoc.

| Scope | Examples | Allowed When ImGui Captures Keyboard |
| --- | --- | --- |
| Text input | characters, Backspace, arrows inside fields | Yes, owned by ImGui |
| Viewport actions | F, Home, W/E/R tools, selection keys | No |
| Fly movement | WASD, Space, Q/E, Shift/Ctrl modifiers | Only while RMB-look owns input |
| Debug global | F6 directional-only, F7 point-light stencil, F8 freeze culling | Prefer no while text input is active; allow when viewport or debug UI is focused |
| Application global | Escape cancellation, screenshot, close | Yes, but route carefully |

All toggle actions must be edge-triggered. Continuous actions must use the
per-frame input snapshot and `deltaTime`.

## Mouse Button Map

| Input | Rendering Space Behavior |
| --- | --- |
| LMB click | Select or focus UI gizmo handle |
| LMB drag | Gizmo drag or marquee selection |
| Alt+LMB drag | Orbit |
| RMB hold | Fly look |
| RMB drag in orthographic view | Pan |
| Alt+RMB drag | Pan |
| Shift+RMB drag | Pan |
| MMB drag | Revit: pan; Blender: orbit |
| Shift+MMB drag | Revit: orbit; Blender: pan |
| Wheel | Dolly |
| RMB+Wheel | Adjust fly speed |
| Navigation widget axis/cube click | Snap to top/bottom/left/right/front/back view |
| Navigation widget LMB disk drag | Free rotate around the active pivot |
| Navigation widget MMB/RMB disk drag | Pan camera left/right/up/down |

## Keyboard Map

| Key | Behavior |
| --- | --- |
| W/A/S/D | Fly movement while RMB-look is active; transform tool selection when idle |
| Q/E | Down/up in fly mode; rotate tool on E when idle |
| Space | Up in fly mode |
| Shift | Fast movement or coarse viewport adjustment |
| Ctrl | Down in fly mode or fine viewport adjustment |
| Alt | Camera chord modifier for orbit/pan |
| Arrow keys | Move camera along current view right/up; Shift is coarse, Ctrl is fine |
| F | Frame selection or scene |
| Home | Reset camera to scene bounds |
| Esc | Cancel active gesture/tool, release pointer lock, then clear selection |
| Delete | Delete selected editable item when supported |
| F6/F7/F8 | Existing debug toggles |

Where key meaning overlaps, active mode wins: fly-look owns WASD/QE/Space while
RMB is held; transform mode owns W/E/R only when the viewport is idle.

## Proposed Architecture

Introduce a render-space interaction layer between raw GLFW callbacks and
camera/scene mutations:

```text
WindowInputBridge
  -> InputManager event queue and per-frame snapshot
  -> RenderSurfaceInteractionController
       -> GuiManager capture state
       -> CameraController navigation commands
       -> SceneController selection and transform commands
       -> DebugRenderState shortcut commands
```

Responsibilities:

- `WindowInputBridge`: only translates GLFW callbacks into typed events.
- `InputManager`: stores current key/button state, modifiers, pointer position,
  pointer delta, wheel delta, and transition edges.
- `RenderSurfaceInteractionController`: decides who owns each event, advances
  the interaction state machine, and emits camera/scene/debug commands.
- `CameraController`: applies camera operations such as fly, orbit, drag pan,
  view-basis movement, dolly, frame selection, and reset to bounds.
- `GuiManager`: reports ImGui capture and later may expose the explicit viewport
  rectangle when docking is introduced.

This keeps `RendererFrontend::processInput()` orchestration-level and avoids
putting gesture details into renderer frame submission code.

## State Machine

```text
Idle
  -> Hover
  -> SelectPress
  -> MarqueeSelect
  -> FlyLook
  -> Orbit
  -> Pan
  -> Dolly
  -> GizmoDrag
```

Common transitions:

- Focus lost: any state -> Idle, clear pressed keys, release pointer lock.
- Escape: active state -> Idle, cancel pending gesture.
- Button release: drag state -> Idle.
- ImGui capture while Idle: remain Idle and do not start viewport gestures.
- ImGui capture during active viewport drag: keep current viewport owner until
  release.

## Camera Behavior Details

- Movement speed should scale from scene bounds and remain user-adjustable.
- Fly movement remains ground-plane based for W/A/S/D, with explicit up/down
  controls.
- Orbit uses the selected surface point as its pivot, with selected object
  bounds as fallback, so large BIM or glTF scenes remain inspectable even when
  node origins are far from visible geometry.
- Perspective wheel dolly moves along the current camera forward vector and uses
  the active pivot only to scale movement speed.
- Wheel dolly speed should scale from the active pivot or scene size so large
  and small scenes feel consistent.
- Near/far planes should not be changed by interaction except through existing
  camera reset or explicit camera settings.
- Camera commands update CPU camera state only; GPU camera buffers continue to
  upload during `drawFrame()` after synchronization.

## Implementation Phases

1. Define the input event model: done
   - Add typed mouse button, key, modifier, cursor, scroll, and focus events.
   - Track button/key transitions separately from held state.
   - Add GLFW scroll callback forwarding.
2. Add the interaction controller: done
   - Route ImGui capture, active gestures, and viewport shortcuts.
   - Move F6/F7/F8 edge handling behind named bindings.
   - Preserve existing RMB-look and WASD behavior first.
3. Add orbit, pan, dolly, and frame-selection camera commands: mostly done
   - Add pivot tracking and scene-bounds fallback.
   - Add speed adjustment through RMB+wheel.
   - Add perspective/orthographic projection switching and preset view snaps.
   - Add view-widget free rotation and panning.
4. Add selection support: mostly done
   - Start with CPU ray against scene bounds or primitive bounds. Superseded by
     GPU picking for visible scene geometry.
   - Promote selected node into existing GUI mesh selection.
   - Use opaque pick ID plus depth visibility for pixel-accurate selection.
   - Use transparent-pick rendering for transparent geometry.
   - Include auxiliary BIM geometry in selection and hover paths.
5. Add transform tooling: implemented for current direct-manipulation scope
   - Use a renderer-native gizmo pass.
   - Keep transform commands routed through `SceneController`/`CameraController`.
   - Show move/scale handles for the selected object.
   - Show rotate rings for the selected object.
   - Start transform drags only when the pointer hits the center box, move
     arrow, rotate ring, or scale box handle.
   - Move selected objects along the projected screen direction of the visible
     local/world handle.
   - Highlight hovered gizmo handles without changing the explicit transform
     constraint.
   - Add toggle-based snapping for move, rotate, and scale drags.
   - Keep selected-object visualization as an orange outline/stroke.
   - Deferred: undo/redo, richer visual hover affordances, and configurable
     snap increments.
6. Add discoverability: implemented for current scope
   - Add a compact viewport navigation widget with axis endpoints, view cube,
     projection toggle, free rotation, and pan.
   - Add a navigation style selector for Revit middle-mouse and Blender
     middle-mouse conventions.
   - Add tooltips on viewport tool buttons, the snapping toggle, and
     navigation-widget drag regions.
   - Add configurable bindings after behavior is stable.

## Documented Changes - 2026-05-02

- Added renderer-space panning controls for difficult orthographic and snapped
  views:
  - right-drag pans when the camera is orthographic;
  - Shift+right-drag pans in any projection;
  - middle/right drag on the navigation widget disk pans the camera.
- Kept left-drag on the navigation widget disk as free rotation, so axis clicks
  and cube view snaps remain separate from drag gestures.
- Added the perspective/orthographic projection toggle to the navigation widget
  and made the center cube projection match the active camera mode.
- Updated selection to use GPU pick/depth visibility and transparent-pick
  passes so visible geometry owns the click target.
- Replaced selected-object vertex/edge clutter with a single orange selection
  outline that is not culled by the selected object's regular visibility.
- Added renderer-native transform gizmo rendering for selected objects and
  changed scale handles toward box/plane-style endpoints.
- Added screen-space transform gizmo hit testing:
  - move drags start from arrow shafts/tips or the center box;
  - rotate drags start from axis rings;
  - scale drags start from axis lines/box handles;
  - left-dragging empty space while a transform tool is active no longer
    transforms the selected object.
- Added gizmo hover feedback that highlights the hovered handle while leaving
  the explicit axis constraint unchanged.
- Added transform snapping through the viewport Snap checkbox and `S` shortcut:
  - move snaps in 0.25 unit increments relative to drag start;
  - rotate snaps in 15 degree increments relative to drag start;
  - scale snaps in 0.1 increments relative to drag start.
- Corrected Move local/world behavior:
  - local/world now controls the displayed and constrained axis basis;
  - constrained move follows the handle's projected screen direction;
  - free move uses the camera view plane for predictable object placement.
- Added Revit/Blender navigation style selection:
  - Revit: MMB pan, Shift+MMB orbit;
  - Blender: MMB orbit, Shift+MMB pan.
- Added arrow-key view panning with Shift for coarse steps and Ctrl for fine
  steps.
- Updated keyboard view movement so arrow keys follow the current camera
  right/up basis instead of using drag-pan signs.
- Updated perspective mouse-wheel dolly to move along the current camera forward
  vector, with pivot distance used only for speed scaling.
- Updated selected-object navigation pivots to use the picked surface point for
  mouse selections, with scene draw bounds and BIM element bounds as fallback,
  fixing orthographic/Blender orbit around off-center origins without snapping
  the camera to the object center.
- Transparent GPU picks use the transparent-pick depth image for that surface
  point when available; opaque picks fall back to the main scene depth.
- Added viewport tool button tooltips for the current Select/Move/Rotate/Scale
  interaction model.

## Tests And Validation

- Unit-test the interaction state machine without Vulkan.
- Unit-test ImGui capture rules with mocked capture flags.
- Unit-test edge-triggered toggles so key repeat cannot flip state repeatedly.
- Unit-test focus loss clearing pressed keys and pointer lock.
- Add manual validation scenarios:
  - Type in the model path field; WASD/F/Home must not move the camera.
  - Hold RMB over the viewport; mouse and WASD move the camera even if the
    cursor crosses an ImGui panel while locked.
  - Release RMB; cursor returns and UI controls work immediately.
  - Alt+LMB and MMB orbit around a selected object.
  - In orthographic top/front/side views, RMB drag pans left/right/up/down.
  - On the navigation widget, LMB disk drag rotates while MMB/RMB disk drag pans.
  - Clicking navigation widget axis endpoints snaps to the matching view.
  - Toggle perspective/orthographic from the navigation widget and verify both
    the main camera and widget cube projection update.
  - Select a mesh partially hidden behind another mesh; the occluded mesh must
    not be selected through visible geometry.
  - Select transparent geometry and verify transparent objects can still be
    picked when visible.
  - Selected objects show a visible single-color orange outline rather than
    dense vertex or triangle lines.
  - In Move/Rotate/Scale tools, dragging empty viewport space must not transform
    the selected object.
  - Move arrows, rotate rings, scale box handles, and the center box should each
    start a transform drag and set the matching active axis.
  - In Move, Local and World modes should visibly change the axis basis, and
    dragging an axis handle should move along that visible projected handle.
  - Free Move should follow the camera view plane in both Local and World modes.
  - Hovering a gizmo handle should highlight it without changing the selected
    constraint shown in the Axis control.
  - With Snap enabled, Move, Rotate, and Scale drags should advance in visible
    stepped increments from the drag start transform.
  - In Revit navigation style, MMB pans and Shift+MMB orbits.
  - In Blender navigation style, MMB orbits and Shift+MMB pans.
  - In orthographic mode, Blender MMB orbit rotates around the selected surface
    point for mouse-picked scene graph and BIM selections.
  - Arrow keys move the camera left/right/up/down in the current view only when
    the viewport owns keyboard input.
  - Perspective wheel dolly moves along the camera view direction and feels
    stable on small sample models and large BIM scenes.

## Acceptance Criteria

- There is one documented owner for each mouse and keyboard gesture.
- ImGui capture and viewport capture never produce simultaneous camera and UI
  changes.
- Existing RMB-look, WASD movement, and F6/F7/F8 debug toggles still work.
- Scroll input is supported and does not interfere with ImGui widgets.
- Camera navigation supports fly, orbit, pan, dolly, frame selection, and reset.
- Selection and transform interactions have explicit states and cancellation
  behavior.
