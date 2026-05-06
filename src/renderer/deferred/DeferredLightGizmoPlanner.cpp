#include "Container/renderer/deferred/DeferredLightGizmoPlanner.h"

#include <algorithm>

namespace container::renderer {

namespace {

[[nodiscard]] float gizmoExtentForPosition(const glm::vec3 &worldPosition,
                                           const glm::vec3 &cameraPosition,
                                           float radiusBias) {
  const float dist =
      glm::max(glm::length(worldPosition - cameraPosition), 0.1f);
  return std::clamp(dist * 0.03f + radiusBias, 0.25f, 6.0f);
}

[[nodiscard]] glm::vec3 normalizedDisplayColor(const glm::vec3 &color,
                                               const glm::vec3 &tint,
                                               float tintWeight) {
  glm::vec3 normalized = color;
  const float maxChannel =
      std::max({normalized.r, normalized.g, normalized.b, 0.0001f});
  normalized /= maxChannel;
  return glm::mix(glm::max(normalized, glm::vec3(0.35f)), tint, tintWeight);
}

} // namespace

DeferredLightGizmoPlan
buildDeferredLightGizmoPlan(const DeferredLightGizmoPlanInputs &inputs) {
  DeferredLightGizmoPlan plan{};

  const glm::vec3 directionalColor = normalizedDisplayColor(
      inputs.directionalColor, glm::vec3(1.0f, 0.95f, 0.35f), 0.5f);
  const glm::vec3 directionalPosition =
      inputs.sceneCenter -
      inputs.directionalDirection * (inputs.sceneWorldRadius * 1.15f);
  const float directionalExtent = gizmoExtentForPosition(
      directionalPosition, inputs.cameraPosition,
      std::clamp(inputs.sceneWorldRadius * 0.02f, 0.05f, 1.0f));

  LightPushConstants &directionalGizmo =
      plan.pushConstants[plan.pushConstantCount++];
  directionalGizmo.positionRadius =
      glm::vec4(directionalPosition, directionalExtent);
  directionalGizmo.colorIntensity = glm::vec4(directionalColor, 1.0f);

  const uint32_t pointCount = std::min<uint32_t>(
      static_cast<uint32_t>(inputs.pointLights.size()),
      kMaxDeferredLightGizmos);
  for (uint32_t i = 0u; i < pointCount; ++i) {
    const container::gpu::PointLightData &pointLight = inputs.pointLights[i];
    const glm::vec3 pointColor = normalizedDisplayColor(
        glm::vec3(pointLight.colorIntensity), glm::vec3(1.0f), 0.35f);
    const glm::vec3 pointPosition = glm::vec3(pointLight.positionRadius);
    const float pointExtent = gizmoExtentForPosition(
        pointPosition, inputs.cameraPosition,
        std::clamp(inputs.sceneWorldRadius * 0.015f, 0.04f, 0.75f));

    LightPushConstants &pointGizmo =
        plan.pushConstants[plan.pushConstantCount++];
    pointGizmo.positionRadius = glm::vec4(pointPosition, pointExtent);
    pointGizmo.colorIntensity = glm::vec4(pointColor, 1.0f);
  }

  return plan;
}

} // namespace container::renderer
