#pragma once

#include "Container/renderer/lighting/LightPushConstants.h"
#include "Container/utility/SceneData.h"

#include <array>
#include <cstdint>
#include <span>

namespace container::renderer {

inline constexpr uint32_t kMaxDeferredLightGizmos = 256u;

struct DeferredLightGizmoPlanInputs {
  glm::vec3 sceneCenter{0.0f, 0.0f, 0.0f};
  float sceneWorldRadius{1.0f};
  glm::vec3 cameraPosition{0.0f, 0.0f, 0.0f};
  glm::vec3 directionalDirection{0.0f, -1.0f, 0.0f};
  glm::vec3 directionalColor{1.0f, 1.0f, 1.0f};
  std::span<const container::gpu::PointLightData> pointLights{};
};

struct DeferredLightGizmoPlan {
  std::array<LightPushConstants, kMaxDeferredLightGizmos + 1u>
      pushConstants{};
  uint32_t pushConstantCount{0u};
};

[[nodiscard]] DeferredLightGizmoPlan
buildDeferredLightGizmoPlan(const DeferredLightGizmoPlanInputs &inputs);

} // namespace container::renderer
