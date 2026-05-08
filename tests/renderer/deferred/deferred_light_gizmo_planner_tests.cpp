#include "Container/renderer/deferred/DeferredLightGizmoPlanner.h"
#include "Container/renderer/lighting/EditableLight.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace {

using container::renderer::DeferredLightGizmoPlanInputs;
using container::renderer::DeferredLightGizmoPickInputs;
using container::renderer::buildDeferredLightGizmoPlan;
using container::renderer::kMaxDeferredLightGizmos;
using container::renderer::pickDeferredLightGizmoAtCursor;

void expectVec3Near(const glm::vec3 &actual, const glm::vec3 &expected) {
  EXPECT_NEAR(actual.x, expected.x, 1.0e-5f);
  EXPECT_NEAR(actual.y, expected.y, 1.0e-5f);
  EXPECT_NEAR(actual.z, expected.z, 1.0e-5f);
}

container::gpu::PointLightData pointLight(const glm::vec3 &position,
                                          const glm::vec3 &color) {
  container::gpu::PointLightData light{};
  light.positionRadius = glm::vec4(position, 10.0f);
  light.colorIntensity = glm::vec4(color, 5.0f);
  return light;
}

container::renderer::EditableLightEntity editableLight(
    container::renderer::EditableLightType type, uint32_t index,
    const glm::vec3 &position, bool selected) {
  container::renderer::EditableLightEntity light{};
  light.id = {.type = type,
              .source = container::renderer::EditableLightSource::Manual,
              .index = index};
  light.type = type;
  light.source = container::renderer::EditableLightSource::Manual;
  light.position = position;
  light.direction = {0.0f, -1.0f, 0.0f};
  light.tangent = {1.0f, 0.0f, 0.0f};
  light.bitangent = {0.0f, 0.0f, 1.0f};
  light.color = {0.25f, 0.5f, 1.0f};
  light.intensity = 4.0f;
  light.range = 6.0f;
  light.areaHalfSize = {1.5f, 0.75f};
  light.selected = selected;
  return light;
}

container::gpu::CameraData identityCamera() {
  container::gpu::CameraData camera{};
  camera.viewProj = glm::mat4(1.0f);
  camera.cameraWorldPosition = glm::vec4(0.0f, 0.0f, 4.0f, 1.0f);
  return camera;
}

container::renderer::DeferredLightGizmoVisual pickVisual(
    container::renderer::EditableLightType type, uint32_t index,
    const glm::vec3 &position, bool selectable = true) {
  container::renderer::DeferredLightGizmoVisual visual{};
  visual.editableLightId = {
      .type = type,
      .source = container::renderer::EditableLightSource::Manual,
      .index = index};
  visual.lightType = type;
  visual.worldPosition = position;
  visual.worldRadius = 0.5f;
  visual.selectable = selectable;
  return visual;
}

} // namespace

TEST(DeferredLightGizmoPlannerTests, EmitsDirectionalGizmoWithoutPointLights) {
  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneCenter = {1.0f, 2.0f, 3.0f};
  inputs.sceneWorldRadius = 4.0f;
  inputs.cameraPosition = {1.0f, 2.0f, 3.0f};
  inputs.directionalDirection = {0.0f, -1.0f, 0.0f};
  inputs.directionalColor = {0.25f, 0.5f, 1.0f};

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, 1u);
  expectVec3Near(glm::vec3(plan.pushConstants[0].positionRadius),
                 glm::vec3(1.0f, 6.6f, 3.0f));
  EXPECT_FLOAT_EQ(plan.pushConstants[0].positionRadius.w, 0.25f);
  expectVec3Near(glm::vec3(plan.pushConstants[0].colorIntensity),
                 glm::vec3(0.675f, 0.725f, 0.675f));
  EXPECT_FLOAT_EQ(plan.pushConstants[0].colorIntensity.w, 1.0f);
}

TEST(DeferredLightGizmoPlannerTests, PointGizmosCopyPositionsAndUseExtentBias) {
  const std::array<container::gpu::PointLightData, 2> pointLights = {
      pointLight({2.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 0.5f}),
      pointLight({0.0f, 4.0f, 0.0f}, {0.2f, 0.4f, 1.0f}),
  };

  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 10.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.pointLights = pointLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, 3u);
  expectVec3Near(glm::vec3(plan.pushConstants[1].positionRadius),
                 glm::vec3(2.0f, 0.0f, 0.0f));
  expectVec3Near(glm::vec3(plan.pushConstants[2].positionRadius),
                 glm::vec3(0.0f, 4.0f, 0.0f));
  EXPECT_FLOAT_EQ(plan.pushConstants[1].positionRadius.w, 0.25f);
  EXPECT_FLOAT_EQ(plan.pushConstants[2].positionRadius.w, 0.27f);
}

TEST(DeferredLightGizmoPlannerTests, PointColorsNormalizeTowardWhite) {
  const std::array<container::gpu::PointLightData, 1> pointLights = {
      pointLight({0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 0.0f}),
  };
  DeferredLightGizmoPlanInputs inputs{};
  inputs.pointLights = pointLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, 2u);
  expectVec3Near(glm::vec3(plan.pushConstants[1].colorIntensity),
                 glm::vec3(1.0f, 0.675f, 0.5775f));
}

TEST(DeferredLightGizmoPlannerTests, PointGizmoCountClampsToMaximum) {
  std::vector<container::gpu::PointLightData> pointLights(
      kMaxDeferredLightGizmos + 8u);
  for (uint32_t i = 0u; i < pointLights.size(); ++i) {
    pointLights[i] = pointLight({static_cast<float>(i), 0.0f, 0.0f},
                                {1.0f, 1.0f, 1.0f});
  }

  DeferredLightGizmoPlanInputs inputs{};
  inputs.pointLights = pointLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  EXPECT_EQ(plan.pushConstantCount, kMaxDeferredLightGizmos + 1u);
}

TEST(DeferredLightGizmoPlannerTests, ExtentsClampToSupportedRange) {
  const std::array<container::gpu::PointLightData, 2> pointLights = {
      pointLight({1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}),
      pointLight({1000.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}),
  };
  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 1.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.pointLights = pointLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, 3u);
  EXPECT_FLOAT_EQ(plan.pushConstants[1].positionRadius.w, 0.25f);
  EXPECT_FLOAT_EQ(plan.pushConstants[2].positionRadius.w, 6.0f);
}

TEST(DeferredLightGizmoPlannerTests,
     EditableLightGizmosPreserveIdsAndSelectionMetadata) {
  const std::array editableLights = {
      editableLight(container::renderer::EditableLightType::Directional, 0u,
                    {0.0f, 5.0f, 0.0f}, false),
      editableLight(container::renderer::EditableLightType::Spot, 2u,
                    {2.0f, 1.0f, 0.0f}, true),
      editableLight(container::renderer::EditableLightType::Area, 3u,
                    {-2.0f, 1.0f, 0.0f}, false),
  };

  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 8.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.editableLights = editableLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, editableLights.size());
  ASSERT_EQ(plan.visualCount, editableLights.size());
  EXPECT_EQ(plan.visuals[0].editableLightId, editableLights[0].id);
  EXPECT_EQ(plan.visuals[1].editableLightId, editableLights[1].id);
  EXPECT_EQ(plan.visuals[2].editableLightId, editableLights[2].id);
  EXPECT_EQ(plan.visuals[1].lightType,
            container::renderer::EditableLightType::Spot);
  EXPECT_TRUE(plan.visuals[1].selected);
  EXPECT_TRUE(plan.visuals[1].selectable);
  expectVec3Near(plan.visuals[2].worldPosition, editableLights[2].position);
}

TEST(DeferredLightGizmoPlannerTests, SelectedEditableLightUsesEmphasizedMarker) {
  const std::array editableLights = {
      editableLight(container::renderer::EditableLightType::Point, 0u,
                    {2.0f, 0.0f, 0.0f}, false),
      editableLight(container::renderer::EditableLightType::Point, 1u,
                    {2.0f, 0.0f, 0.0f}, true),
  };

  DeferredLightGizmoPlanInputs inputs{};
  inputs.sceneWorldRadius = 4.0f;
  inputs.cameraPosition = {0.0f, 0.0f, 0.0f};
  inputs.editableLights = editableLights;

  const auto plan = buildDeferredLightGizmoPlan(inputs);

  ASSERT_EQ(plan.pushConstantCount, 2u);
  EXPECT_GT(plan.pushConstants[1].positionRadius.w,
            plan.pushConstants[0].positionRadius.w);
  EXPECT_GT(plan.pushConstants[1].colorIntensity.g,
            plan.pushConstants[0].colorIntensity.g);
}

TEST(DeferredLightGizmoPlannerTests, PickEditableLightGizmoAtCursor) {
  const std::array visuals = {
      pickVisual(container::renderer::EditableLightType::Point, 4u,
                 {0.0f, 0.0f, 0.0f}),
  };

  const auto hit = pickDeferredLightGizmoAtCursor(
      DeferredLightGizmoPickInputs{.visuals = visuals,
                                   .cameraData = identityCamera(),
                                   .framebufferExtent = {100u, 100u},
                                   .cursor = {53.0f, 50.0f},
                                   .hitRadiusPixels = 12.0f});

  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->editableLightId, visuals[0].editableLightId);
  EXPECT_EQ(hit->visualIndex, 0u);
  EXPECT_NEAR(hit->distancePixels, 3.0f, 1.0e-5f);
}

TEST(DeferredLightGizmoPlannerTests,
     PickerIgnoresUnselectableAndInvalidLightIds) {
  std::array visuals = {
      pickVisual(container::renderer::EditableLightType::Point, 0u,
                 {0.0f, 0.0f, 0.0f}, false),
      pickVisual(container::renderer::EditableLightType::Point, 1u,
                 {0.0f, 0.0f, 0.0f}, true),
  };
  visuals[1].editableLightId.index =
      container::renderer::kInvalidEditableLightIndex;

  const auto hit = pickDeferredLightGizmoAtCursor(
      DeferredLightGizmoPickInputs{.visuals = visuals,
                                   .cameraData = identityCamera(),
                                   .framebufferExtent = {100u, 100u},
                                   .cursor = {50.0f, 50.0f},
                                   .hitRadiusPixels = 12.0f});

  EXPECT_FALSE(hit.has_value());
}

TEST(DeferredLightGizmoPlannerTests, PickerChoosesClosestCursorTarget) {
  const std::array visuals = {
      pickVisual(container::renderer::EditableLightType::Point, 0u,
                 {0.0f, 0.0f, 0.0f}),
      pickVisual(container::renderer::EditableLightType::Area, 1u,
                 {0.18f, 0.0f, 0.0f}),
  };

  const auto hit = pickDeferredLightGizmoAtCursor(
      DeferredLightGizmoPickInputs{.visuals = visuals,
                                   .cameraData = identityCamera(),
                                   .framebufferExtent = {100u, 100u},
                                   .cursor = {59.0f, 50.0f},
                                   .hitRadiusPixels = 14.0f});

  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->editableLightId, visuals[1].editableLightId);
  EXPECT_EQ(hit->visualIndex, 1u);
}

TEST(DeferredLightGizmoPlannerTests, PickerRejectsBehindCameraTargets) {
  auto camera = identityCamera();
  camera.viewProj = glm::mat4(0.0f);
  camera.viewProj[0][0] = 1.0f;
  camera.viewProj[1][1] = 1.0f;
  camera.viewProj[2][2] = 1.0f;
  camera.viewProj[2][3] = -1.0f;
  const std::array visuals = {
      pickVisual(container::renderer::EditableLightType::Point, 0u,
                 {0.0f, 0.0f, 1.0f}),
  };

  const auto hit = pickDeferredLightGizmoAtCursor(
      DeferredLightGizmoPickInputs{.visuals = visuals,
                                   .cameraData = camera,
                                   .framebufferExtent = {100u, 100u},
                                   .cursor = {50.0f, 50.0f},
                                   .hitRadiusPixels = 12.0f});

  EXPECT_FALSE(hit.has_value());
}
