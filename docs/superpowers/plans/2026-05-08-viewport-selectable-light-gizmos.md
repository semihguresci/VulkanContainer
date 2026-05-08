# Viewport Selectable Light Gizmos Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make visible directional, point, spot, and area light gizmos selectable from the viewport and synchronize that selection with the existing editable light inspector and transform gizmos.

**Architecture:** Extend the deferred light gizmo planner so it emits both draw push constants and selectable gizmo metadata keyed by `EditableLightId`. Add a CPU projection picker over that metadata, then call it from `RendererFrontend` before existing mesh/BIM fallback picking clears empty selections. Keep `LightingManager` as the owner of selected light state.

**Tech Stack:** C++23, GLM, Vulkan frame extent data, existing renderer light structs, GTest/CTest, ImGui editor selection path.

---

### Task 1: Planner Metadata And Selected Visuals

**Files:**
- Modify: `include/Container/renderer/deferred/DeferredLightGizmoPlanner.h`
- Modify: `src/renderer/deferred/DeferredLightGizmoPlanner.cpp`
- Modify: `tests/renderer/deferred/deferred_light_gizmo_planner_tests.cpp`

- [x] **Step 1: Write failing metadata tests**

Add tests that construct `EditableLightEntity` values and assert `buildDeferredLightGizmoPlan()` stores ids, light types, positions, selected flags, and selected marker emphasis.

- [x] **Step 2: Run RED**

Run: `cmake --build out/build/windows-debug --target deferred_light_gizmo_planner_tests`

Expected: FAIL because the planner does not expose editable-light metadata.

- [x] **Step 3: Implement minimal planner metadata**

Add `DeferredLightGizmoVisual`, an `editableLights` input span, and a `visuals` array in the plan. Build from editable entities when provided, preserving the legacy directional-plus-point behavior when the editable span is empty.

- [x] **Step 4: Run GREEN**

Run: `cmake --build out/build/windows-debug --target deferred_light_gizmo_planner_tests && ctest --test-dir out/build/windows-debug -R deferred_light_gizmo_planner_tests --output-on-failure`

Expected: PASS.

### Task 2: CPU Light Gizmo Picker

**Files:**
- Modify: `include/Container/renderer/deferred/DeferredLightGizmoPlanner.h`
- Modify: `src/renderer/deferred/DeferredLightGizmoPlanner.cpp`
- Modify: `tests/renderer/deferred/deferred_light_gizmo_planner_tests.cpp`

- [x] **Step 1: Write failing picker tests**

Add tests for `pickDeferredLightGizmoAtCursor()` covering projected hits, invalid IDs, behind-camera targets, and overlapping hits choosing the closest cursor target.

- [x] **Step 2: Run RED**

Run: `cmake --build out/build/windows-debug --target deferred_light_gizmo_planner_tests`

Expected: FAIL because the picker API does not exist.

- [x] **Step 3: Implement picker**

Project each selectable visual through `CameraData::viewProj`, reject invalid projection or zero framebuffer size, compare cursor distance to a bounded pixel radius, and return the best hit with id and visual index.

- [x] **Step 4: Run GREEN**

Run: `cmake --build out/build/windows-debug --target deferred_light_gizmo_planner_tests && ctest --test-dir out/build/windows-debug -R deferred_light_gizmo_planner_tests --output-on-failure`

Expected: PASS.

### Task 3: Use Editable Gizmos In LightingManager

**Files:**
- Modify: `src/renderer/lighting/LightingManager.cpp`
- Modify: `tests/validation/rendering_convention_tests.cpp`

- [x] **Step 1: Write failing source-contract test**

Assert `LightingManager::drawLightGizmos()` passes `editableLights_` to the gizmo planner and no longer limits visible selectable gizmos to `pointLightsSsbo_`.

- [x] **Step 2: Run RED**

Run: `cmake --build out/build/windows-debug --target rendering_convention_tests`

Expected: FAIL because `drawLightGizmos()` still passes point lights only.

- [x] **Step 3: Update draw plan inputs**

Pass `editableLights_` into `buildDeferredLightGizmoPlan()`. Keep descriptor recording unchanged.

- [x] **Step 4: Run GREEN**

Run: `cmake --build out/build/windows-debug --target rendering_convention_tests && ctest --test-dir out/build/windows-debug -R rendering_convention_tests --output-on-failure`

Expected: PASS.

### Task 4: Viewport Selection Routing

**Files:**
- Modify: `include/Container/renderer/core/RendererFrontend.h`
- Modify: `src/renderer/core/RendererFrontend.cpp`
- Modify: `tests/validation/rendering_convention_tests.cpp`

- [x] **Step 1: Write failing routing test**

Assert `RendererFrontend` has `pickEditableLightAtCursor`, checks it before empty GPU-pick clearing, and routes hits through `selectEditableLight`.

- [x] **Step 2: Run RED**

Run: `cmake --build out/build/windows-debug --target rendering_convention_tests`

Expected: FAIL because viewport light picking is absent.

- [x] **Step 3: Implement routing**

Add a frontend helper that builds a gizmo plan from `LightingManager::editableLights()` and calls `pickDeferredLightGizmoAtCursor()`. In `selectMeshNodeAtCursor()`, try this helper before mesh/BIM pick routing so a visible light marker can be selected even when it overlays model geometry. On success, clear mesh/BIM selection, select the light, clear hover, clear selected draw command caches, and set the status message.

- [x] **Step 4: Run GREEN**

Run: `cmake --build out/build/windows-debug --target rendering_convention_tests && ctest --test-dir out/build/windows-debug -R rendering_convention_tests --output-on-failure`

Expected: PASS.

### Task 5: Focused Verification

**Files:**
- No production changes.

- [x] **Step 1: Build focused targets**

Run: `cmake --build out/build/windows-debug --target deferred_light_gizmo_planner_tests rendering_convention_tests editable_light_tests deferred_transform_gizmo_tests`

Expected: build succeeds.

- [x] **Step 2: Run focused tests**

Run: `ctest --test-dir out/build/windows-debug -R "deferred_light_gizmo_planner_tests|rendering_convention_tests|editable_light_tests|deferred_transform_gizmo_tests" --output-on-failure`

Expected: all selected tests pass.

- [x] **Step 3: Check whitespace**

Run: `git diff --check`

Expected: no whitespace errors; line-ending warnings from existing Windows files are acceptable.
