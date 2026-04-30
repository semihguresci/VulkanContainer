#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace container::gpu {

struct CameraData {
  alignas(16) glm::mat4 viewProj{1.0f};
  alignas(16) glm::mat4 inverseViewProj{1.0f};
  alignas(16) glm::vec4 cameraWorldPosition{0.0f, 0.0f, 0.0f, 1.0f};
  // Unit world-space forward vector. Lighting uses this for cascade selection
  // without reconstructing camera basis vectors per shaded pixel.
  alignas(16) glm::vec4 cameraForward{0.0f, 0.0f, -1.0f, 0.0f};
};

struct ObjectData {
  alignas(16) glm::mat4 model{1.0f};
  // Inverse-transpose normal transform stored as 3 columns. The implicit final
  // row of a mat4 is unused for normal vectors, so this saves 16 bytes/object.
  alignas(16) glm::vec4 normalMatrix0{1.0f, 0.0f, 0.0f, 0.0f};
  alignas(16) glm::vec4 normalMatrix1{0.0f, 1.0f, 0.0f, 0.0f};
  alignas(16) glm::vec4 normalMatrix2{0.0f, 0.0f, 1.0f, 0.0f};
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

inline constexpr uint32_t kMaxDeferredPointLights = 12;
inline constexpr uint32_t kMaxClusteredLights     = 8192;
inline constexpr uint32_t kTileSize               = 16;   // pixels
inline constexpr uint32_t kClusterDepthSlices     = 16;
inline constexpr uint32_t kMaxLightsPerTile       = 128;
inline constexpr uint32_t kShadowCascadeCount = 4;
inline constexpr uint32_t kShadowMapResolution = 2048;

struct LightingSettings {
  uint32_t preset{0};
  float density{1.0f};
  float radiusScale{1.0f};
  float intensityScale{1.0f};
  float directionalIntensity{2.0f};
};

struct LightCullingStats {
  uint32_t submittedLights{0};
  uint32_t activeClusters{0};
  uint32_t totalClusters{0};
  uint32_t maxLightsPerCluster{0};
  uint32_t droppedLightReferences{0};
  float clusterCullMs{0.0f};
  float clusteredLightingMs{0.0f};
};

struct ShadowCascadeData {
  alignas(16) glm::mat4 viewProj{1.0f};
  alignas(4)  float     splitDepth{0.0f};
  alignas(4)  float     padding[3]{};
};

struct ShadowData {
  ShadowCascadeData cascades[kShadowCascadeCount];
};

struct ShadowCascadeCullData {
  alignas(16) glm::mat4 viewProj{1.0f};
  alignas(16) glm::mat4 lightView{1.0f};
  alignas(16) glm::vec4 receiverMinBounds{0.0f};
  alignas(16) glm::vec4 receiverMaxBounds{0.0f};
  alignas(16) glm::vec4 casterMinBounds{0.0f};
  alignas(16) glm::vec4 casterMaxBounds{0.0f};
};

struct ShadowCullData {
  ShadowCascadeCullData cascades[kShadowCascadeCount];
};

struct ShadowCullPushConstants {
  uint32_t drawCount{0};
  uint32_t cascadeIndex{0};
  uint32_t outputOffset{0};
  uint32_t objectCount{0};
};

struct ShadowCullCountData {
  std::array<uint32_t, kShadowCascadeCount> visibleCounts{};
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
  uint32_t depthSliceCount{kClusterDepthSlices};
  uint32_t oitEnabled{0};
};

struct LightingData {
  alignas(16) glm::vec4 directionalDirection{0.0f, 0.0f, 1.0f, 0.0f};
  alignas(16) glm::vec4 directionalColorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
  alignas(4) uint32_t pointLightCount{0};
  alignas(4) std::array<uint32_t, 3> featureFlags{};
  alignas(4) uint32_t prefilteredMipCount{1};
  alignas(4) std::array<uint32_t, 3> padding{};
};

struct TileLightGrid {
  uint32_t offset{0};
  uint32_t count{0};
};

struct TileCullPushConstants {
  uint32_t tileCountX{0};
  uint32_t tileCountY{0};
  uint32_t depthSliceCount{kClusterDepthSlices};
  uint32_t totalLights{0};
  float cameraNear{0.1f};
  float cameraFar{100.0f};
};

struct TiledLightingPushConstants {
  uint32_t tileCountX{0};
  uint32_t tileCountY{0};
  uint32_t depthSliceCount{kClusterDepthSlices};
  float cameraNear{0.1f};
  float cameraFar{100.0f};
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

// ---------------------------------------------------------------------------
// Compile-time layout verification.
//
// These asserts guard the host <-> shader struct layout contract. The shader
// counterparts live in `shaders/lighting_structs.slang` (LightingBuffer,
// ShadowBuffer, CameraBuffer, PointLightData, ShadowCascadeData) and in the
// per-shader `ObjectBuffer` declarations (gbuffer.slang, depth_prepass.slang,
// shadow_depth.slang, forward_transparent.slang, etc.).
//
// If any field is added/removed or an `alignas` is changed, the shader side
// must be updated in lockstep and these numbers re-derived. The sizes below
// are the std140-compatible layouts used when these structs are uploaded to
// uniform or storage buffers.
// ---------------------------------------------------------------------------

static_assert(sizeof(CameraData) == 160,
              "CameraData size mismatch with shaders/lighting_structs.slang "
              "CameraBuffer. Update shader layout in lockstep.");
static_assert(alignof(CameraData) == 16, "CameraData must be 16-byte aligned.");
static_assert(offsetof(CameraData, viewProj) == 0, "CameraData.viewProj offset");
static_assert(offsetof(CameraData, inverseViewProj) == 64,
              "CameraData.inverseViewProj offset");
static_assert(offsetof(CameraData, cameraWorldPosition) == 128,
              "CameraData.cameraWorldPosition offset");
static_assert(offsetof(CameraData, cameraForward) == 144,
              "CameraData.cameraForward offset");

static_assert(sizeof(PointLightData) == 32,
              "PointLightData size mismatch with shader PointLightData.");
static_assert(alignof(PointLightData) == 16,
              "PointLightData must be 16-byte aligned.");
static_assert(offsetof(PointLightData, positionRadius) == 0,
              "PointLightData.positionRadius offset");
static_assert(offsetof(PointLightData, colorIntensity) == 16,
              "PointLightData.colorIntensity offset");

static_assert(sizeof(LightingData) == 64,
              "LightingData size mismatch with shaders/lighting_structs.slang "
              "LightingBuffer. Update shader layout in lockstep.");
static_assert(alignof(LightingData) == 16,
              "LightingData must be 16-byte aligned.");
static_assert(offsetof(LightingData, directionalDirection) == 0,
              "LightingData.directionalDirection offset");
static_assert(offsetof(LightingData, directionalColorIntensity) == 16,
              "LightingData.directionalColorIntensity offset");
static_assert(offsetof(LightingData, pointLightCount) == 32,
              "LightingData.pointLightCount offset");
static_assert(offsetof(LightingData, featureFlags) == 36,
              "LightingData.featureFlags offset");
static_assert(offsetof(LightingData, prefilteredMipCount) == 48,
              "LightingData.prefilteredMipCount offset");
static_assert(sizeof(ShadowCascadeData) == 80,
              "ShadowCascadeData size mismatch with shader ShadowCascadeData.");
static_assert(alignof(ShadowCascadeData) == 16,
              "ShadowCascadeData must be 16-byte aligned.");
static_assert(offsetof(ShadowCascadeData, viewProj) == 0,
              "ShadowCascadeData.viewProj offset");
static_assert(offsetof(ShadowCascadeData, splitDepth) == 64,
              "ShadowCascadeData.splitDepth offset");

static_assert(sizeof(ShadowData) == 80 * kShadowCascadeCount,
              "ShadowData size mismatch with shaders/lighting_structs.slang "
              "ShadowBuffer. Update shader layout in lockstep.");
static_assert(alignof(ShadowData) == 16, "ShadowData must be 16-byte aligned.");

static_assert(sizeof(ShadowCascadeCullData) == 192,
              "ShadowCascadeCullData size mismatch with shaders/lighting_structs.slang "
              "ShadowCascadeCullData. Update shader layout in lockstep.");
static_assert(alignof(ShadowCascadeCullData) == 16,
              "ShadowCascadeCullData must be 16-byte aligned.");
static_assert(offsetof(ShadowCascadeCullData, viewProj) == 0,
              "ShadowCascadeCullData.viewProj offset");
static_assert(offsetof(ShadowCascadeCullData, lightView) == 64,
              "ShadowCascadeCullData.lightView offset");
static_assert(offsetof(ShadowCascadeCullData, receiverMinBounds) == 128,
              "ShadowCascadeCullData.receiverMinBounds offset");
static_assert(offsetof(ShadowCascadeCullData, receiverMaxBounds) == 144,
              "ShadowCascadeCullData.receiverMaxBounds offset");
static_assert(offsetof(ShadowCascadeCullData, casterMinBounds) == 160,
              "ShadowCascadeCullData.casterMinBounds offset");
static_assert(offsetof(ShadowCascadeCullData, casterMaxBounds) == 176,
              "ShadowCascadeCullData.casterMaxBounds offset");

static_assert(sizeof(ShadowCullData) == sizeof(ShadowCascadeCullData) * kShadowCascadeCount,
              "ShadowCullData size mismatch with shaders/lighting_structs.slang "
              "ShadowCullData. Update shader layout in lockstep.");
static_assert(alignof(ShadowCullData) == 16,
              "ShadowCullData must be 16-byte aligned.");
static_assert(sizeof(ShadowCullPushConstants) == 16,
              "ShadowCullPushConstants must remain 16 bytes.");
static_assert(sizeof(ShadowCullCountData) == sizeof(uint32_t) * kShadowCascadeCount,
              "ShadowCullCountData stores one visible count per shadow cascade.");

static_assert(sizeof(ObjectData) == 208,
              "ObjectData size mismatch with shader ObjectBuffer (see "
              "gbuffer.slang, depth_prepass.slang, shadow_depth.slang, etc.). "
              "Update all shader ObjectBuffer declarations in lockstep.");
static_assert(alignof(ObjectData) == 16,
              "ObjectData must be 16-byte aligned.");
static_assert(offsetof(ObjectData, model) == 0, "ObjectData.model offset");
static_assert(offsetof(ObjectData, normalMatrix0) == 64,
              "ObjectData.normalMatrix0 offset");
static_assert(offsetof(ObjectData, normalMatrix1) == 80,
              "ObjectData.normalMatrix1 offset");
static_assert(offsetof(ObjectData, normalMatrix2) == 96,
              "ObjectData.normalMatrix2 offset");
static_assert(offsetof(ObjectData, color) == 112, "ObjectData.color offset");
static_assert(offsetof(ObjectData, emissiveColor) == 128,
              "ObjectData.emissiveColor offset");
static_assert(offsetof(ObjectData, emissiveStrength) == 140,
              "ObjectData.emissiveStrength offset");
static_assert(offsetof(ObjectData, metallicRoughness) == 144,
              "ObjectData.metallicRoughness offset");
static_assert(offsetof(ObjectData, alphaCutoff) == 152,
              "ObjectData.alphaCutoff offset");
static_assert(offsetof(ObjectData, normalTextureScale) == 156,
              "ObjectData.normalTextureScale offset");
static_assert(offsetof(ObjectData, occlusionStrength) == 160,
              "ObjectData.occlusionStrength offset");
static_assert(offsetof(ObjectData, baseColorTextureIndex) == 164,
              "ObjectData.baseColorTextureIndex offset");
static_assert(offsetof(ObjectData, normalTextureIndex) == 168,
              "ObjectData.normalTextureIndex offset");
static_assert(offsetof(ObjectData, occlusionTextureIndex) == 172,
              "ObjectData.occlusionTextureIndex offset");
static_assert(offsetof(ObjectData, emissiveTextureIndex) == 176,
              "ObjectData.emissiveTextureIndex offset");
static_assert(offsetof(ObjectData, metallicRoughnessTextureIndex) == 180,
              "ObjectData.metallicRoughnessTextureIndex offset");
static_assert(offsetof(ObjectData, flags) == 184, "ObjectData.flags offset");
static_assert(offsetof(ObjectData, boundingSphere) == 192,
              "ObjectData.boundingSphere offset");

}  // namespace container::gpu

