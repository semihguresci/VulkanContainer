#pragma once

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

// Tag component — marks entities that should produce draw calls.
struct RenderableTag {};

// Optional: back-reference to SceneGraph node index for bridge code.
struct SceneNodeRef {
  uint32_t nodeIndex{std::numeric_limits<uint32_t>::max()};
};

}  // namespace container::ecs
