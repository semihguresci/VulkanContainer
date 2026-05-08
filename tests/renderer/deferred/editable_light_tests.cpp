#include "Container/renderer/lighting/EditableLight.h"

#include <gtest/gtest.h>

#include <glm/gtc/constants.hpp>

#include <cmath>

namespace {

TEST(EditableLightTests, DirectionalEntityNormalizesEditedDirection) {
  container::gpu::LightingData data{};
  data.directionalDirection = glm::vec4(0.0f, -2.0f, 0.0f, 0.0f);
  data.directionalColorIntensity = glm::vec4(1.0f, 0.8f, 0.6f, 3.0f);

  const auto entity = container::renderer::editableDirectionalLight(
      container::renderer::EditableLightSource::Generated,
      glm::vec3(4.0f, 5.0f, 6.0f), data, true);

  EXPECT_EQ(entity.type, container::renderer::EditableLightType::Directional);
  EXPECT_EQ(entity.source,
            container::renderer::EditableLightSource::Generated);
  EXPECT_NEAR(entity.direction.y, -1.0f, 1.0e-5f);
  EXPECT_FLOAT_EQ(entity.intensity, 3.0f);
  EXPECT_TRUE(entity.selected);
}

TEST(EditableLightTests, SpotEntityRoundTripsConeAndDirection) {
  container::gpu::PointLightData spot{};
  spot.positionRadius = glm::vec4(1.0f, 2.0f, 3.0f, 9.0f);
  spot.colorIntensity = glm::vec4(0.2f, 0.4f, 0.6f, 7.0f);
  spot.directionInnerCos =
      glm::vec4(0.0f, 0.0f, -2.0f, std::cos(glm::radians(15.0f)));
  spot.coneOuterCosType =
      glm::vec4(std::cos(glm::radians(30.0f)), 1.0f, 0.0f, 0.0f);

  auto entity = container::renderer::editablePointLight(
      container::renderer::EditableLightSource::Imported, 2u, spot, true);
  entity.direction = glm::vec3(0.0f, -3.0f, 0.0f);
  entity.outerConeDegrees = 45.0f;

  const auto roundTrip =
      container::renderer::pointLightDataFromEditable(entity);

  EXPECT_EQ(entity.type, container::renderer::EditableLightType::Spot);
  EXPECT_NEAR(roundTrip.directionInnerCos.y, -1.0f, 1.0e-5f);
  EXPECT_NEAR(roundTrip.coneOuterCosType.x, std::cos(glm::radians(45.0f)),
              1.0e-5f);
  EXPECT_FLOAT_EQ(roundTrip.coneOuterCosType.y,
                  container::gpu::kLightTypeSpot);
}

TEST(EditableLightTests, AreaEntityRoundTripsShapeAndHalfSize) {
  container::gpu::AreaLightData area{};
  area.positionRange = glm::vec4(2.0f, 3.0f, 4.0f, 18.0f);
  area.colorIntensity = glm::vec4(0.5f, 0.7f, 1.0f, 11.0f);
  area.directionType =
      glm::vec4(0.0f, -4.0f, 0.0f, container::gpu::kAreaLightTypeDisk);
  area.tangentHalfSize = glm::vec4(1.0f, 0.0f, 0.0f, 2.5f);
  area.bitangentHalfSize = glm::vec4(0.0f, 0.0f, 1.0f, 2.5f);

  auto entity = container::renderer::editableAreaLight(
      container::renderer::EditableLightSource::Manual, 3u, area, false);
  entity.areaHalfSize = glm::vec2(4.0f, 5.0f);

  const auto roundTrip = container::renderer::areaLightDataFromEditable(entity);

  EXPECT_EQ(entity.type, container::renderer::EditableLightType::Area);
  EXPECT_EQ(entity.source, container::renderer::EditableLightSource::Manual);
  EXPECT_FALSE(entity.selected);
  EXPECT_NEAR(roundTrip.directionType.y, -1.0f, 1.0e-5f);
  EXPECT_FLOAT_EQ(roundTrip.directionType.w,
                  container::gpu::kAreaLightTypeDisk);
  EXPECT_FLOAT_EQ(roundTrip.tangentHalfSize.w, 4.0f);
  EXPECT_FLOAT_EQ(roundTrip.bitangentHalfSize.w, 5.0f);
}

} // namespace
