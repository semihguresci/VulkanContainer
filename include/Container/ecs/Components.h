#pragma once

#include "Container/utility/SceneData.h"

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <limits>

namespace container::ecs {

// Per-entity transform, mirroring SceneNode::localTransform / worldTransform.
struct TransformComponent {
  glm::mat4 localTransform{1.0f};
  glm::mat4 worldTransform{1.0f};
};

// Identifies which mesh primitive this entity draws.
struct MeshComponent {
  uint32_t primitiveIndex{std::numeric_limits<uint32_t>::max()};
};

// Material binding for this entity.
struct MaterialComponent {
  uint32_t materialIndex{0};
};

// GPU-visible point light payload owned by an ECS entity.
struct LightComponent {
  container::gpu::PointLightData data{};
};

// GPU-visible active camera payload owned by an ECS entity.
struct CameraComponent {
  container::gpu::CameraData data{};
  float nearPlane{0.1f};
  float farPlane{100.0f};
};

// Tag component — marks entities that should produce draw calls.
struct RenderableTag {};

// Tag component for point-light entities.
struct LightTag {};

// Tag component for the active camera entity.
struct CameraTag {};

// Optional: back-reference to SceneGraph node index for bridge code.
struct SceneNodeRef {
  uint32_t nodeIndex{std::numeric_limits<uint32_t>::max()};
};

} // namespace container::ecs
