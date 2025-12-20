#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>

struct CameraData {
  alignas(16) glm::mat4 viewProj{1.0f};
};

struct ObjectData {
  alignas(16) glm::mat4 model{1.0f};
  alignas(16) glm::vec4 color{1.0f};
  alignas(16) glm::vec3 emissiveColor{0.0f, 0.0f, 0.0f};
  alignas(4) float emissiveStrength{1.0f};
  alignas(8) glm::vec2 metallicRoughness{1.0f, 1.0f};
  alignas(4) uint32_t baseColorTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t normalTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t occlusionTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t emissiveTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t metallicRoughnessTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t padding{0};
};

struct BindlessPushConstants {
  uint32_t objectIndex{0};
};

