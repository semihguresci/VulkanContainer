#include "Container/renderer/deferred/DeferredLightGizmoPlanner.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace {

using container::renderer::DeferredLightGizmoPlanInputs;
using container::renderer::buildDeferredLightGizmoPlan;
using container::renderer::kMaxDeferredLightGizmos;

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
