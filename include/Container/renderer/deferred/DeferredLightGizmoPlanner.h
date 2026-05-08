#pragma once

#include "Container/renderer/lighting/EditableLight.h"
#include "Container/renderer/lighting/LightPushConstants.h"
#include "Container/utility/SceneData.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace container::renderer {

inline constexpr uint32_t kMaxDeferredLightGizmos = 256u;

struct DeferredLightGizmoPlanInputs {
  glm::vec3 sceneCenter{0.0f, 0.0f, 0.0f};
  float sceneWorldRadius{1.0f};
  glm::vec3 cameraPosition{0.0f, 0.0f, 0.0f};
  glm::vec3 directionalDirection{0.0f, -1.0f, 0.0f};
  glm::vec3 directionalColor{1.0f, 1.0f, 1.0f};
  std::span<const EditableLightEntity> editableLights{};
  std::span<const container::gpu::PointLightData> pointLights{};
};

struct DeferredLightGizmoVisual {
  EditableLightId editableLightId{};
  EditableLightType lightType{EditableLightType::Point};
  glm::vec3 worldPosition{0.0f};
  float worldRadius{0.25f};
  bool selected{false};
  bool selectable{false};
};

struct DeferredLightGizmoPlan {
  std::array<LightPushConstants, kMaxDeferredLightGizmos + 1u>
      pushConstants{};
  std::array<DeferredLightGizmoVisual, kMaxDeferredLightGizmos + 1u> visuals{};
  uint32_t pushConstantCount{0u};
  uint32_t visualCount{0u};
};

struct DeferredLightGizmoPickInputs {
  std::span<const DeferredLightGizmoVisual> visuals{};
  container::gpu::CameraData cameraData{};
  glm::uvec2 framebufferExtent{0u, 0u};
  glm::vec2 cursor{0.0f, 0.0f};
  float hitRadiusPixels{18.0f};
};

struct DeferredLightGizmoPickResult {
  EditableLightId editableLightId{};
  uint32_t visualIndex{std::numeric_limits<uint32_t>::max()};
  float distancePixels{0.0f};
  float cameraDistance{0.0f};
};

[[nodiscard]] DeferredLightGizmoPlan
buildDeferredLightGizmoPlan(const DeferredLightGizmoPlanInputs &inputs);

[[nodiscard]] std::optional<DeferredLightGizmoPickResult>
pickDeferredLightGizmoAtCursor(const DeferredLightGizmoPickInputs &inputs);

} // namespace container::renderer
