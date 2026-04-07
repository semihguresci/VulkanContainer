#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <limits>

struct CameraData {
  alignas(16) glm::mat4 viewProj{1.0f};
  alignas(16) glm::mat4 inverseViewProj{1.0f};
  alignas(16) glm::vec4 cameraWorldPosition{0.0f, 0.0f, 0.0f, 1.0f};
};

struct ObjectData {
  alignas(16) glm::mat4 model{1.0f};
  alignas(16) glm::mat4 normalMatrix{1.0f};
  alignas(16) glm::vec4 color{1.0f};
  alignas(16) glm::vec3 emissiveColor{0.0f, 0.0f, 0.0f};
  alignas(4) float emissiveStrength{1.0f};
  alignas(8) glm::vec2 metallicRoughness{1.0f, 1.0f};
  alignas(4) float alphaCutoff{0.5f};
  alignas(4) uint32_t baseColorTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t normalTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t occlusionTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t emissiveTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t metallicRoughnessTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t flags{0};
  alignas(8) glm::vec2 padding{0.0f};
};

struct BindlessPushConstants {
  uint32_t objectIndex{0};
};

inline constexpr uint32_t kObjectFlagAlphaMask = 1u << 0;
inline constexpr uint32_t kObjectFlagAlphaBlend = 1u << 1;
inline constexpr uint32_t kObjectFlagDoubleSided = 1u << 2;

inline constexpr uint32_t kMaxDeferredPointLights = 4;

struct PointLightData {
  alignas(16) glm::vec4 positionRadius{0.0f, 0.0f, 0.0f, 1.0f};
  alignas(16) glm::vec4 colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
};

struct LightingData {
  alignas(16) glm::vec4 directionalDirection{0.0f, 0.0f, 1.0f, 0.0f};
  alignas(16) glm::vec4 directionalColorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
  alignas(4) uint32_t pointLightCount{0};
  alignas(16) std::array<PointLightData, kMaxDeferredPointLights> pointLights{};
};

