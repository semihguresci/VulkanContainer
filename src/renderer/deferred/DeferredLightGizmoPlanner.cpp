#include "Container/renderer/deferred/DeferredLightGizmoPlanner.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

[[nodiscard]] glm::vec3 tintForLightType(EditableLightType type) {
  switch (type) {
  case EditableLightType::Directional:
    return {1.0f, 0.95f, 0.35f};
  case EditableLightType::Spot:
    return {0.45f, 0.75f, 1.0f};
  case EditableLightType::Area:
    return {1.0f, 0.55f, 0.85f};
  case EditableLightType::Point:
    return {1.0f, 1.0f, 1.0f};
  }
  return {1.0f, 1.0f, 1.0f};
}

[[nodiscard]] float radiusBiasForLightType(EditableLightType type,
                                           float sceneWorldRadius) {
  switch (type) {
  case EditableLightType::Directional:
    return std::clamp(sceneWorldRadius * 0.02f, 0.05f, 1.0f);
  case EditableLightType::Area:
    return std::clamp(sceneWorldRadius * 0.018f, 0.05f, 0.9f);
  case EditableLightType::Spot:
  case EditableLightType::Point:
    return std::clamp(sceneWorldRadius * 0.015f, 0.04f, 0.75f);
  }
  return 0.04f;
}

void appendGizmo(DeferredLightGizmoPlan &plan,
                 const EditableLightEntity &light,
                 const DeferredLightGizmoPlanInputs &inputs) {
  if (plan.pushConstantCount >= plan.pushConstants.size() ||
      plan.visualCount >= plan.visuals.size()) {
    return;
  }

  const glm::vec3 color = normalizedDisplayColor(
      light.color, tintForLightType(light.type),
      light.type == EditableLightType::Point ? 0.35f : 0.45f);
  const glm::vec3 displayColor =
      light.selected ? glm::mix(color, glm::vec3(0.25f, 1.0f, 0.95f), 0.45f)
                     : color;
  const float extent = gizmoExtentForPosition(
      light.position, inputs.cameraPosition,
      radiusBiasForLightType(light.type, inputs.sceneWorldRadius));
  const float displayExtent =
      std::clamp(light.selected ? extent * 1.35f : extent, 0.25f, 6.0f);

  LightPushConstants &pushConstants =
      plan.pushConstants[plan.pushConstantCount++];
  pushConstants.positionRadius = glm::vec4(light.position, displayExtent);
  pushConstants.colorIntensity = glm::vec4(displayColor, 1.0f);
  pushConstants.directionInnerCos = glm::vec4(light.direction, 1.0f);
  pushConstants.coneOuterCosType =
      glm::vec4(0.0f,
                light.type == EditableLightType::Spot
                    ? container::gpu::kLightTypeSpot
                    : container::gpu::kLightTypePoint,
                0.0f, 0.0f);

  DeferredLightGizmoVisual &visual = plan.visuals[plan.visualCount++];
  visual.editableLightId = light.id;
  visual.lightType = light.type;
  visual.worldPosition = light.position;
  visual.worldRadius = displayExtent;
  visual.selected = light.selected;
  visual.selectable = light.editable && isValidEditableLightId(light.id);
}

void appendLegacyVisual(DeferredLightGizmoPlan &plan, EditableLightType type,
                        const glm::vec3 &position, float radius) {
  if (plan.visualCount >= plan.visuals.size()) {
    return;
  }

  DeferredLightGizmoVisual &visual = plan.visuals[plan.visualCount++];
  visual.lightType = type;
  visual.worldPosition = position;
  visual.worldRadius = radius;
  visual.selected = false;
  visual.selectable = false;
}

[[nodiscard]] std::optional<glm::vec2>
projectToFramebuffer(const container::gpu::CameraData &cameraData,
                     const glm::uvec2 &framebufferExtent,
                     const glm::vec3 &worldPosition) {
  if (framebufferExtent.x == 0u || framebufferExtent.y == 0u) {
    return std::nullopt;
  }

  const glm::vec4 clip =
      cameraData.viewProj * glm::vec4(worldPosition, 1.0f);
  if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
      !std::isfinite(clip.w) || clip.w <= 0.0001f) {
    return std::nullopt;
  }

  const glm::vec2 ndc{clip.x / clip.w, clip.y / clip.w};
  if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y)) {
    return std::nullopt;
  }

  return glm::vec2{
      (ndc.x * 0.5f + 0.5f) * static_cast<float>(framebufferExtent.x),
      (1.0f - (ndc.y * 0.5f + 0.5f)) *
          static_cast<float>(framebufferExtent.y),
  };
}

} // namespace

DeferredLightGizmoPlan
buildDeferredLightGizmoPlan(const DeferredLightGizmoPlanInputs &inputs) {
  DeferredLightGizmoPlan plan{};

  if (!inputs.editableLights.empty()) {
    for (const EditableLightEntity &light : inputs.editableLights) {
      appendGizmo(plan, light, inputs);
    }
    return plan;
  }

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
  appendLegacyVisual(plan, EditableLightType::Directional, directionalPosition,
                     directionalExtent);

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
    appendLegacyVisual(plan, EditableLightType::Point, pointPosition,
                       pointExtent);
  }

  return plan;
}

std::optional<DeferredLightGizmoPickResult>
pickDeferredLightGizmoAtCursor(const DeferredLightGizmoPickInputs &inputs) {
  if (inputs.visuals.empty()) {
    return std::nullopt;
  }

  const float hitRadius = std::clamp(
      std::isfinite(inputs.hitRadiusPixels) ? inputs.hitRadiusPixels : 18.0f,
      2.0f, 64.0f);
  const float hitRadius2 = hitRadius * hitRadius;

  std::optional<DeferredLightGizmoPickResult> bestHit;
  float bestDistance2 = std::numeric_limits<float>::max();
  float bestCameraDistance = std::numeric_limits<float>::max();
  const glm::vec3 cameraPosition{inputs.cameraData.cameraWorldPosition};

  for (uint32_t i = 0u; i < inputs.visuals.size(); ++i) {
    const DeferredLightGizmoVisual &visual = inputs.visuals[i];
    if (!visual.selectable || !isValidEditableLightId(visual.editableLightId)) {
      continue;
    }

    const auto screenPosition = projectToFramebuffer(
        inputs.cameraData, inputs.framebufferExtent, visual.worldPosition);
    if (!screenPosition) {
      continue;
    }

    const glm::vec2 delta = inputs.cursor - *screenPosition;
    const float distance2 = glm::dot(delta, delta);
    if (!std::isfinite(distance2) || distance2 > hitRadius2) {
      continue;
    }

    const float cameraDistance =
        glm::length(visual.worldPosition - cameraPosition);
    const bool betterCursorHit = distance2 < bestDistance2;
    const bool sameCursorHit =
        std::abs(distance2 - bestDistance2) <= 1.0e-4f;
    if (betterCursorHit ||
        (sameCursorHit && cameraDistance < bestCameraDistance)) {
      bestDistance2 = distance2;
      bestCameraDistance = cameraDistance;
      bestHit = DeferredLightGizmoPickResult{
          .editableLightId = visual.editableLightId,
          .visualIndex = i,
          .distancePixels = std::sqrt(distance2),
          .cameraDistance = cameraDistance};
    }
  }

  return bestHit;
}

} // namespace container::renderer
