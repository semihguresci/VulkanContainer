#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <limits>

namespace container::gpu {

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
  alignas(4) float normalTextureScale{1.0f};
  alignas(4) float occlusionStrength{1.0f};
  alignas(4) uint32_t baseColorTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t normalTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t occlusionTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t emissiveTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t metallicRoughnessTextureIndex{std::numeric_limits<uint32_t>::max()};
  alignas(4) uint32_t flags{0};
  // Bounding sphere in world space: xyz = center, w = radius.
  alignas(16) glm::vec4 boundingSphere{0.0f, 0.0f, 0.0f, 0.0f};
};

struct NormalValidationSettings
{
  bool enabled{false};
  bool showFaceFill{true};
  float lineLength{0.16f};
  float lineOffset{0.002f};
  float lineWidth{1.0f};
  float faceAlpha{1.0f};
};

struct BindlessPushConstants {
  uint32_t objectIndex{0};
};

inline constexpr uint32_t kObjectFlagAlphaMask = 1u << 0;
inline constexpr uint32_t kObjectFlagAlphaBlend = 1u << 1;
inline constexpr uint32_t kObjectFlagDoubleSided = 1u << 2;

inline constexpr uint32_t kMaxDeferredPointLights = 4;
inline constexpr uint32_t kMaxClusteredLights     = 4096;
inline constexpr uint32_t kTileSize               = 16;   // pixels
inline constexpr uint32_t kMaxLightsPerTile       = 256;
inline constexpr uint32_t kShadowCascadeCount = 4;
inline constexpr uint32_t kShadowMapResolution = 2048;

struct ShadowCascadeData {
  alignas(16) glm::mat4 viewProj{1.0f};
  alignas(4)  float     splitDepth{0.0f};
  alignas(4)  float     padding[3]{};
};

struct ShadowData {
  ShadowCascadeData cascades[kShadowCascadeCount];
};

struct ShadowPushConstants {
  uint32_t objectIndex{0};
  uint32_t cascadeIndex{0};
};

struct PointLightData {
  alignas(16) glm::vec4 positionRadius{0.0f, 0.0f, 0.0f, 1.0f};
  alignas(16) glm::vec4 colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
};

struct PostProcessPushConstants {
  uint32_t outputMode{0};
  uint32_t bloomEnabled{0};
  float    bloomIntensity{0.3f};
  float    cameraNear{0.1f};
  float    cameraFar{100.0f};
  float    cascadeSplits[kShadowCascadeCount]{};
  uint32_t tileCountX{0};
  uint32_t totalLights{0};
};

struct LightingData {
  alignas(16) glm::vec4 directionalDirection{0.0f, 0.0f, 1.0f, 0.0f};
  alignas(16) glm::vec4 directionalColorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
  alignas(4) uint32_t pointLightCount{0};
  alignas(4) std::array<uint32_t, 3> featureFlags{};
  alignas(16) std::array<PointLightData, kMaxDeferredPointLights> pointLights{};
};

struct TileLightGrid {
  uint32_t offset{0};
  uint32_t count{0};
};

struct TileCullPushConstants {
  uint32_t tileCountX{0};
  uint32_t tileCountY{0};
  uint32_t totalLights{0};
  uint32_t pad0{0};
};

struct TiledLightingPushConstants {
  uint32_t tileCountX{0};
};

// GPU-driven rendering: matches VkDrawIndexedIndirectCommand layout.
struct GpuDrawIndexedIndirectCommand {
  uint32_t indexCount{0};
  uint32_t instanceCount{0};
  uint32_t firstIndex{0};
  int32_t  vertexOffset{0};
  uint32_t firstInstance{0};  // Encodes objectIndex.
};

struct CullPushConstants {
  uint32_t objectCount{0};
  uint32_t pad0{0};
  uint32_t pad1{0};
  uint32_t pad2{0};
};

struct HiZPushConstants {
  uint32_t srcWidth{0};
  uint32_t srcHeight{0};
  uint32_t dstMipLevel{0};
  uint32_t pad0{0};
};

}  // namespace container::gpu

