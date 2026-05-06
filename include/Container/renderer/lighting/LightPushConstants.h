#pragma once

#include "Container/common/CommonMath.h"

#include <cstddef>

namespace container::renderer {

// Push constants shared by deferred point lighting and light-gizmo rendering.
struct LightPushConstants {
  glm::vec4 positionRadius{0.0f, 0.0f, 0.0f, 1.0f};
  glm::vec4 colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
  glm::vec4 directionInnerCos{0.0f, 0.0f, -1.0f, 1.0f};
  glm::vec4 coneOuterCosType{0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(LightPushConstants) == 64,
              "LightPushConstants size mismatch with "
              "shaders/push_constants_common.slang LightPushConstants.");
static_assert(offsetof(LightPushConstants, positionRadius) == 0,
              "LightPushConstants.positionRadius offset");
static_assert(offsetof(LightPushConstants, colorIntensity) == 16,
              "LightPushConstants.colorIntensity offset");
static_assert(offsetof(LightPushConstants, directionInnerCos) == 32,
              "LightPushConstants.directionInnerCos offset");
static_assert(offsetof(LightPushConstants, coneOuterCosType) == 48,
              "LightPushConstants.coneOuterCosType offset");

} // namespace container::renderer
