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
- Mouse wheel: dolly toward or away from the cursor hit point if available;
  otherwise dolly along the camera forward vector.
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
- MMB drag: orbit when no object-editing tool is active.
- Pivot priority: selected item center, cursor hit point, scene bounds center.
- Shift while orbiting: reduce sensitivity for precision.
- Double MMB click or F: recenter the pivot on the selected item or scene.

Orbit should not change the selected object. It only changes camera position and
orientation.

### Pan Mode

Move the camera target laterally without changing viewing direction.

- Shift+MMB drag: pan parallel to the camera image plane.
- Alt+RMB drag outside fly-look capture: pan.
- Pan speed scales with distance from the camera to the active pivot.

### Dolly/Zoom Mode

Change distance while keeping the target stable.

- Wheel in inspect/orbit mode: dolly.
- Ctrl+wheel: fine dolly.
- Shift+wheel: coarse dolly.
- Dolly should clamp before crossing the pivot or near plane.

### Transform Mode

Object editing should be a separate tool state, not mixed into camera controls.

- W: translate gizmo.
- E: rotate gizmo.
- R: scale gizmo.
- X/Y/Z after a transform key: constrain to axis.
- L: toggle local/world orientation.
- Delete: remove selected editable object when supported.
- Esc: cancel active transform; if idle, exit transform tool.

Gizmo drags own pointer input until release. Camera gestures should not start
while a gizmo handle is hot or active.

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
| Alt+RMB drag | Pan |
| MMB drag | Orbit |
| Shift+MMB drag | Pan |
| Wheel | Dolly |
| RMB+Wheel | Adjust fly speed |

## Keyboard Map

| Key | Behavior |
| --- | --- |
| W/A/S/D | Fly movement while RMB-look is active; transform tool selection when idle |
| Q/E | Down/up in fly mode; rotate tool on E when idle |
| Space | Up in fly mode |
| Shift | Fast movement or coarse viewport adjustment |
| Ctrl | Down in fly mode or fine viewport adjustment |
| Alt | Camera chord modifier for orbit/pan |
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
- `CameraController`: applies camera operations such as fly, orbit, pan, dolly,
  frame selection, and reset to bounds.
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
- Orbit and dolly use a pivot so large BIM or glTF scenes remain inspectable.
- Wheel dolly should use exponential scaling so large and small scenes feel
  consistent.
- Near/far planes should not be changed by interaction except through existing
  camera reset or explicit camera settings.
- Camera commands update CPU camera state only; GPU camera buffers continue to
  upload during `drawFrame()` after synchronization.

## Implementation Phases

1. Define the input event model:
   - Add typed mouse button, key, modifier, cursor, scroll, and focus events.
   - Track button/key transitions separately from held state.
   - Add GLFW scroll callback forwarding.
2. Add the interaction controller:
   - Route ImGui capture, active gestures, and viewport shortcuts.
   - Move F6/F7/F8 edge handling behind named bindings.
   - Preserve existing RMB-look and WASD behavior first.
3. Add orbit, pan, dolly, and frame-selection camera commands:
   - Add pivot tracking and scene-bounds fallback.
   - Add speed adjustment through RMB+wheel.
4. Add selection support:
   - Start with CPU ray against scene bounds or primitive bounds.
   - Promote selected node into existing GUI mesh selection.
   - Defer GPU picking until there is a concrete need.
5. Add transform tooling:
   - Use a gizmo library such as ImGuizmo or a renderer-native gizmo pass.
   - Keep transform commands routed through `SceneController`/`CameraController`.
6. Add discoverability:
   - Add a compact viewport help overlay or menu entry.
   - Add configurable bindings after behavior is stable.

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
  - Wheel dolly feels stable on small sample models and large BIM scenes.

## Acceptance Criteria

- There is one documented owner for each mouse and keyboard gesture.
- ImGui capture and viewport capture never produce simultaneous camera and UI
  changes.
- Existing RMB-look, WASD movement, and F6/F7/F8 debug toggles still work.
- Scroll input is supported and does not interfere with ImGui widgets.
- Camera navigation supports fly, orbit, pan, dolly, frame selection, and reset.
- Selection and transform interactions have explicit states and cancellation
  behavior.
