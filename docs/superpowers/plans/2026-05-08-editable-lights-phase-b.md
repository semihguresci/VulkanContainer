# Editable Lights Phase B Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make visible directional, point, spot, and area lights selectable and editable from the existing editor UI, with generated lights staying live and manual lights persisted in the current session.

**Architecture:** Add a small renderer-owned editable light contract that converts current GPU light data into editor entities and converts edited entities back into renderer light buffers. `LightingManager` remains the authoritative runtime light owner; `GuiManager` only renders controls and emits callbacks. Viewport transform gizmos operate on the selected editable light when no mesh/BIM object is selected.

**Tech Stack:** C++23, GLM, Vulkan renderer data structures, ImGui editor controls, GTest/CTest.

---

### Task 1: Editable Light Contract

**Files:**
- Create: `include/Container/renderer/lighting/EditableLight.h`
- Create: `src/renderer/lighting/EditableLight.cpp`
- Create: `tests/renderer/deferred/editable_light_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.tests.cmake`

- [ ] **Step 1: Write failing conversion tests**

Add tests that include `Container/renderer/lighting/EditableLight.h` and verify:

```cpp
TEST(EditableLightTests, DirectionalEntityNormalizesEditedDirection) {
  container::gpu::LightingData data{};
  data.directionalDirection = glm::vec4(0.0f, -2.0f, 0.0f, 0.0f);
  data.directionalColorIntensity = glm::vec4(1.0f, 0.8f, 0.6f, 3.0f);

  const auto entity = container::renderer::editableDirectionalLight(
      container::renderer::EditableLightSource::Generated,
      glm::vec3(4.0f, 5.0f, 6.0f), data, true);

  EXPECT_EQ(entity.type, container::renderer::EditableLightType::Directional);
  EXPECT_EQ(entity.source, container::renderer::EditableLightSource::Generated);
  EXPECT_NEAR(entity.direction.y, -1.0f, 1.0e-5f);
  EXPECT_FLOAT_EQ(entity.intensity, 3.0f);
}

TEST(EditableLightTests, SpotEntityRoundTripsConeAndDirection) {
  container::gpu::PointLightData spot{};
  spot.positionRadius = glm::vec4(1.0f, 2.0f, 3.0f, 9.0f);
  spot.colorIntensity = glm::vec4(0.2f, 0.4f, 0.6f, 7.0f);
  spot.directionInnerCos = glm::vec4(0.0f, 0.0f, -2.0f, std::cos(glm::radians(15.0f)));
  spot.coneOuterCosType = glm::vec4(std::cos(glm::radians(30.0f)), 1.0f, 0.0f, 0.0f);

  auto entity = container::renderer::editablePointLight(
      container::renderer::EditableLightSource::Imported, 2u, spot, true);
  entity.direction = glm::vec3(0.0f, -3.0f, 0.0f);
  entity.outerConeDegrees = 45.0f;

  const auto roundTrip = container::renderer::pointLightDataFromEditable(entity);

  EXPECT_EQ(entity.type, container::renderer::EditableLightType::Spot);
  EXPECT_NEAR(roundTrip.directionInnerCos.y, -1.0f, 1.0e-5f);
  EXPECT_NEAR(roundTrip.coneOuterCosType.x, std::cos(glm::radians(45.0f)), 1.0e-5f);
  EXPECT_FLOAT_EQ(roundTrip.coneOuterCosType.y, container::gpu::kLightTypeSpot);
}
```

- [ ] **Step 2: Run RED**

Run: `cmake --build out/build/windows-debug --target editable_light_tests`

Expected: FAIL because `EditableLight.h` and the test target do not exist.

- [ ] **Step 3: Implement the conversion contract**

Define:

```cpp
enum class EditableLightType : uint32_t { Directional, Point, Spot, Area };
enum class EditableLightSource : uint32_t { Generated, Imported, Manual };

struct EditableLightId {
  EditableLightType type{EditableLightType::Point};
  EditableLightSource source{EditableLightSource::Generated};
  uint32_t index{std::numeric_limits<uint32_t>::max()};
};

struct EditableLightEntity {
  EditableLightId id{};
  EditableLightType type{EditableLightType::Point};
  EditableLightSource source{EditableLightSource::Generated};
  std::string label{};
  glm::vec3 position{0.0f};
  glm::vec3 direction{0.0f, -1.0f, 0.0f};
  glm::vec3 color{1.0f};
  float intensity{1.0f};
  float range{1.0f};
  float innerConeDegrees{0.0f};
  float outerConeDegrees{0.0f};
  glm::vec2 areaHalfSize{0.5f};
  float areaShape{container::gpu::kAreaLightTypeRectangle};
  bool selected{false};
  bool editable{true};
};
```

Implement helpers for directional, point/spot, and area conversion. Clamp invalid values, normalize directions, and keep GPU layout fields compatible with `PointLightData` and `AreaLightData`.

- [ ] **Step 4: Run GREEN**

Run: `cmake --build out/build/windows-debug --target editable_light_tests && ctest --test-dir out/build/windows-debug -R editable_light_tests --output-on-failure`

Expected: PASS.

### Task 2: LightingManager Editable State

**Files:**
- Modify: `include/Container/renderer/lighting/LightingManager.h`
- Modify: `src/renderer/lighting/LightingManager.cpp`
- Modify: `tests/validation/rendering_convention_tests.cpp`

- [ ] **Step 1: Write failing source-contract tests**

Add a validation test that asserts `LightingManager` exposes editable lights and manual creation without coupling renderer planners to `GuiManager`:

```cpp
TEST(RenderingConventionTests, LightingManagerOwnsEditableLightState) {
  const std::string header =
      readRepoTextFile("include/Container/renderer/lighting/LightingManager.h");
  const std::string source =
      readRepoTextFile("src/renderer/lighting/LightingManager.cpp");

  EXPECT_TRUE(contains(header, "EditableLightEntity"));
  EXPECT_TRUE(contains(header, "editableLights() const"));
  EXPECT_TRUE(contains(header, "selectEditableLight"));
  EXPECT_TRUE(contains(header, "updateEditableLight"));
  EXPECT_TRUE(contains(header, "addManualEditableLight"));
  EXPECT_TRUE(contains(source, "applyEditableLightOverrides"));
  EXPECT_TRUE(contains(source, "appendManualEditableLights"));
  EXPECT_EQ(source.find("GuiManager"), std::string::npos);
}
```

- [ ] **Step 2: Run RED**

Run: `cmake --build out/build/windows-debug --target rendering_convention_tests && ctest --test-dir out/build/windows-debug -R rendering_convention_tests --output-on-failure`

Expected: FAIL because the editable-light APIs do not exist.

- [ ] **Step 3: Implement runtime editable state**

Add `LightingManager` members for:

```cpp
std::vector<EditableLightEntity> editableLights_{};
EditableLightId selectedEditableLight_{};
std::optional<EditableLightEntity> directionalOverride_{};
std::vector<std::optional<container::gpu::PointLightData>> generatedPointOverrides_{};
std::vector<std::optional<container::gpu::PointLightData>> importedPointOverrides_{};
std::vector<std::optional<container::gpu::AreaLightData>> importedAreaOverrides_{};
std::vector<container::gpu::PointLightData> manualPointLights_{};
std::vector<container::gpu::AreaLightData> manualAreaLights_{};
```

Clear generated overrides when `setLightingSettings()` changes generator settings. Build `editableLights_` after each lighting update. Apply overrides before publishing SSBOs. Append manual lights after generated/imported lights.

- [ ] **Step 4: Run GREEN**

Run: `cmake --build out/build/windows-debug --target rendering_convention_tests && ctest --test-dir out/build/windows-debug -R rendering_convention_tests --output-on-failure`

Expected: PASS.

### Task 3: ImGui Light Selection And Inspector

**Files:**
- Modify: `include/Container/utility/GuiManager.h`
- Modify: `src/utility/GuiManager.cpp`
- Modify: `src/renderer/core/RendererFrontend.cpp`
- Modify: `tests/validation/rendering_convention_tests.cpp`

- [ ] **Step 1: Write failing UI contract tests**

Add a validation test asserting the scene controls receive editable lights and expose selection, manual creation, and direction editing:

```cpp
TEST(RenderingConventionTests, SceneControlsExposeEditableLightInspector) {
  const std::string header = readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string gui = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string frontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(header, "EditableLightEntity"));
  EXPECT_TRUE(contains(header, "addManualEditableLight"));
  EXPECT_TRUE(contains(gui, "Editable lights"));
  EXPECT_TRUE(contains(gui, "Directional direction"));
  EXPECT_TRUE(contains(gui, "Add point"));
  EXPECT_TRUE(contains(gui, "Add spot"));
  EXPECT_TRUE(contains(gui, "Add area"));
  EXPECT_TRUE(contains(frontend, "editableLights()"));
  EXPECT_TRUE(contains(frontend, "updateEditableLight"));
}
```

- [ ] **Step 2: Run RED**

Run: `cmake --build out/build/windows-debug --target rendering_convention_tests && ctest --test-dir out/build/windows-debug -R rendering_convention_tests --output-on-failure`

Expected: FAIL because the UI contract is absent.

- [ ] **Step 3: Implement the inspector**

Extend `drawSceneControls()` with editable light list, selected-light inspector, and add buttons. Use `DragFloat3` for position/direction, `ColorEdit3` for color, and sliders/drags for intensity/range/cone/area size. Directional lights edit direction directly through the inspector and normalize through `LightingManager::updateEditableLight()`.

- [ ] **Step 4: Run GREEN**

Run: `cmake --build out/build/windows-debug --target rendering_convention_tests && ctest --test-dir out/build/windows-debug -R rendering_convention_tests --output-on-failure`

Expected: PASS.

### Task 4: Viewport Transform Gizmo For Selected Lights

**Files:**
- Modify: `src/renderer/core/RendererFrontend.cpp`
- Modify: `tests/validation/rendering_convention_tests.cpp`

- [ ] **Step 1: Write failing transform contract tests**

Add a validation test asserting the transform gizmo considers selected lights and transform drag updates them:

```cpp
TEST(RenderingConventionTests, TransformGizmoCanOperateOnSelectedEditableLights) {
  const std::string frontend =
      readRepoTextFile("src/renderer/core/RendererFrontend.cpp");

  EXPECT_TRUE(contains(frontend, "selectedEditableLight()"));
  EXPECT_TRUE(contains(frontend, "translateSelectedEditableLight"));
  EXPECT_TRUE(contains(frontend, "rotateSelectedEditableLight"));
  EXPECT_TRUE(contains(frontend, "scaleSelectedEditableLight"));
  EXPECT_TRUE(contains(frontend, "Selected editable light"));
}
```

- [ ] **Step 2: Run RED**

Run: `cmake --build out/build/windows-debug --target rendering_convention_tests && ctest --test-dir out/build/windows-debug -R rendering_convention_tests --output-on-failure`

Expected: FAIL because transform drag ignores editable lights.

- [ ] **Step 3: Implement light transform hooks**

When no mesh node is selected and `LightingManager` has a selected editable light, `buildTransformGizmoState()` returns a gizmo centered on the light. Translate moves point/spot/area lights, rotate changes directional/spot/area direction, and scale changes range or area half-size.

- [ ] **Step 4: Run GREEN**

Run: `cmake --build out/build/windows-debug --target rendering_convention_tests && ctest --test-dir out/build/windows-debug -R rendering_convention_tests --output-on-failure`

Expected: PASS.

### Task 5: Focused Verification

**Files:**
- No production changes.

- [ ] **Step 1: Build edited targets**

Run: `cmake --build out/build/windows-debug --target editable_light_tests rendering_convention_tests deferred_light_gizmo_planner_tests deferred_transform_gizmo_tests`

Expected: build succeeds.

- [ ] **Step 2: Run focused tests**

Run: `ctest --test-dir out/build/windows-debug -R "editable_light_tests|rendering_convention_tests|deferred_light_gizmo_planner_tests|deferred_transform_gizmo_tests" --output-on-failure`

Expected: all selected tests pass.

- [ ] **Step 3: Check worktree scope**

Run: `git status --short`

Expected: Phase B files are changed in addition to pre-existing Phase A dirty files; no unrelated resets or generated noise.
