#pragma once

#include "Container/common/CommonMath.h"
namespace utility::material {

enum class AlphaMode : uint32_t {
  Opaque = 0,
  Mask = 1,
  Blend = 2,
};

struct Material {
  glm::vec4 baseColor{1.0f};
  glm::vec3 emissiveColor{0.0f};
  float metallicFactor{1.0f};
  float roughnessFactor{1.0f};
  float alphaCutoff{0.5f};
  uint32_t baseColorTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t normalTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t occlusionTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t emissiveTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t metallicRoughnessTextureIndex{std::numeric_limits<uint32_t>::max()};
  AlphaMode alphaMode{AlphaMode::Opaque};
  bool doubleSided{false};
};
}  // namespace utility::material
