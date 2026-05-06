#include "Container/renderer/core/RendererFrontend.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Container/app/AppConfig.h"
#include "Container/ecs/World.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimFrameDrawRoutingPlanner.h"
#include "Container/renderer/effects/BloomManager.h"
#include "Container/renderer/scene/CameraController.h"
#include "Container/renderer/resources/CommandBufferManager.h"
#include "Container/renderer/debug/DebugUiPresenter.h"
#include "Container/renderer/lighting/EnvironmentManager.h"
#include "Container/renderer/effects/ExposureManager.h"
#include "Container/renderer/core/FrameConcurrencyPolicy.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/deferred/DeferredRasterFrameGraphContext.h"
#include "Container/renderer/pipeline/GraphicsPipelineBuilder.h"
#include "Container/renderer/pipeline/PipelineRegistry.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/effects/OitManager.h"
#include "Container/renderer/core/RenderPassGpuProfiler.h"
#include "Container/renderer/picking/RenderSurfaceInteractionController.h"
#include "Container/renderer/core/RenderExtraction.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/RendererTelemetry.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/scene/SceneProviderSynchronizer.h"
#include "Container/renderer/shadow/ShadowCullManager.h"
#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/scene/MeshSceneProviderBuilder.h"
#include "Container/scene/SceneProvider.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/DebugMessengerExt.h"
#include "Container/utility/FrameSyncManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/Logger.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"
#include "stb_image_write.h"

namespace container::renderer {

using container::gpu::CameraData;
using container::gpu::LightingData;

namespace {

using TelemetryClock = std::chrono::steady_clock;

float elapsedMilliseconds(
    TelemetryClock::time_point start,
    TelemetryClock::time_point end = TelemetryClock::now()) {
  return std::chrono::duration<float, std::milli>(end - start).count();
}

uint32_t saturatingU32(size_t value) {
  return static_cast<uint32_t>(
      std::min<size_t>(value, std::numeric_limits<uint32_t>::max()));
}

const FrameResourceBinding* deferredRasterRuntimeBinding(
    const FrameResourceManager* manager, uint32_t imageIndex,
    std::string_view name) {
  if (manager == nullptr || imageIndex >= manager->frameCount()) {
    return nullptr;
  }
  return manager->resourceRegistry().findBinding(
      TechniqueResourceKey{.technique = RenderTechniqueId::DeferredRaster,
                           .name = std::string(name)},
      imageIndex);
}

const FrameImageBinding* deferredRasterRuntimeImageBinding(
    const FrameResourceManager* manager, uint32_t imageIndex,
    std::string_view name) {
  const FrameResourceBinding* binding =
      deferredRasterRuntimeBinding(manager, imageIndex, name);
  return binding != nullptr && binding->kind == FrameResourceKind::Image
             ? &binding->image
             : nullptr;
}

VkImage deferredRasterRuntimeImage(const FrameResourceManager* manager,
                                   uint32_t imageIndex,
                                   std::string_view name) {
  const FrameImageBinding* binding =
      deferredRasterRuntimeImageBinding(manager, imageIndex, name);
  return binding != nullptr ? binding->image : VK_NULL_HANDLE;
}

const FrameBufferBinding* deferredRasterRuntimeBufferBinding(
    const FrameResourceManager* manager, uint32_t imageIndex,
    std::string_view name) {
  const FrameResourceBinding* binding =
      deferredRasterRuntimeBinding(manager, imageIndex, name);
  return binding != nullptr && binding->kind == FrameResourceKind::Buffer
             ? &binding->buffer
             : nullptr;
}

uint32_t deferredRasterRuntimeOitNodeCapacity(
    const FrameResourceManager* manager, uint32_t imageIndex) {
  const FrameBufferBinding* binding = deferredRasterRuntimeBufferBinding(
      manager, imageIndex, "oit-node-buffer");
  if (binding == nullptr || binding->size == 0) {
    return 0;
  }
  const VkDeviceSize capacity = binding->size / sizeof(OitNode);
  return static_cast<uint32_t>(
      std::min<VkDeviceSize>(capacity, std::numeric_limits<uint32_t>::max()));
}

std::string lowerAscii(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

bool isAuxiliaryRenderModelPath(std::string_view path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos) {
    return false;
  }

  const std::string extension = lowerAscii(path.substr(dot));
  return extension == ".bim" || extension == ".ifc" || extension == ".ifcx" ||
         extension == ".usd" || extension == ".usda" || extension == ".usdc" ||
         extension == ".usdz";
}

glm::vec3 arrayToVec3(const std::array<float, 3> &value) {
  return {value[0], value[1], value[2]};
}

container::gpu::ExposureSettings
exposureSettingsFromConfig(const container::app::AppConfig &config) {
  container::gpu::ExposureSettings settings{};
  if (config.hasManualExposureOverride) {
    settings.mode = container::gpu::kExposureModeManual;
    settings.manualExposure = std::max(config.manualExposure, 0.0f);
  }
  return settings;
}

void applyCameraOverride(container::scene::BaseCamera *camera,
                         const container::app::AppConfig &config) {
  if (!camera || !config.hasCameraOverride) {
    return;
  }

  const glm::vec3 position = arrayToVec3(config.cameraPosition);
  const glm::vec3 target = arrayToVec3(config.cameraTarget);
  glm::vec3 forward = target - position;
  const float length = glm::length(forward);
  if (length <= 0.0001f) {
    return;
  }
  forward /= length;

  const float pitchDegrees =
      glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
  const float yawDegrees = glm::degrees(std::atan2(-forward.z, forward.x));
  camera->setPosition(position);
  camera->setYawPitch(yawDegrees, pitchDegrees);
  if (auto *perspective =
          dynamic_cast<container::scene::PerspectiveCamera *>(camera)) {
    perspective->setFieldOfView(
        std::clamp(config.cameraVerticalFovDegrees, 1.0f, 179.0f));
  }
}

bool isSupportedScreenshotFormat(VkFormat format) {
  switch (format) {
  case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_R8G8B8A8_UNORM:
    return true;
  default:
    return false;
  }
}

std::vector<unsigned char> convertSwapchainBytesToRgba(const unsigned char *src,
                                                       size_t pixelCount,
                                                       VkFormat format) {
  std::vector<unsigned char> rgba(pixelCount * 4u);
  const bool bgra =
      format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
  for (size_t i = 0; i < pixelCount; ++i) {
    const unsigned char c0 = src[i * 4u + 0u];
    const unsigned char c1 = src[i * 4u + 1u];
    const unsigned char c2 = src[i * 4u + 2u];
    const unsigned char c3 = src[i * 4u + 3u];
    rgba[i * 4u + 0u] = bgra ? c2 : c0;
    rgba[i * 4u + 1u] = c1;
    rgba[i * 4u + 2u] = bgra ? c0 : c2;
    rgba[i * 4u + 3u] = c3;
  }
  return rgba;
}

bool decodeDepthReadbackValue(VkFormat format, const void *data,
                              float &outDepth) {
  if (data == nullptr) {
    return false;
  }

  if (format == VK_FORMAT_D32_SFLOAT ||
      format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
    float depth = 0.0f;
    std::memcpy(&depth, data, sizeof(depth));
    if (!std::isfinite(depth)) {
      return false;
    }
    outDepth = std::clamp(depth, 0.0f, 1.0f);
    return true;
  }

  if (format == VK_FORMAT_D24_UNORM_S8_UINT) {
    uint32_t packed = 0u;
    std::memcpy(&packed, data, sizeof(packed));
    outDepth = static_cast<float>(packed & 0x00ffffffu) /
               static_cast<float>(0x00ffffffu);
    return true;
  }

  return false;
}

bool depthHitVisible(float hitDepth, float sampledDepth) {
  constexpr float kDepthVisibilityTolerance = 0.0005f;
  // Reverse-Z stores nearer surfaces as larger values.
  return sampledDepth <= hitDepth + kDepthVisibilityTolerance;
}

bool hasTransparentCommands(const std::vector<DrawCommand> &draws,
                            const std::vector<DrawCommand> &singleSided,
                            const std::vector<DrawCommand> &windingFlipped,
                            const std::vector<DrawCommand> &doubleSided) {
  return !draws.empty() || !singleSided.empty() || !windingFlipped.empty() ||
         !doubleSided.empty();
}

bool hasTransparentGeometry(const BimGeometryDrawLists &draws) {
  return hasTransparentCommands(draws.transparentDrawCommands,
                                draws.transparentSingleSidedDrawCommands,
                                draws.transparentWindingFlippedDrawCommands,
                                draws.transparentDoubleSidedDrawCommands);
}

bool hasAnyGeometry(const BimGeometryDrawLists& draws) {
  return !draws.opaqueDrawCommands.empty() ||
         !draws.opaqueSingleSidedDrawCommands.empty() ||
         !draws.opaqueWindingFlippedDrawCommands.empty() ||
         !draws.opaqueDoubleSidedDrawCommands.empty() ||
         hasTransparentGeometry(draws);
}

bool hasTransparentGeometry(const BimDrawLists &draws, bool includePoints,
                            bool includeCurves) {
  if (hasTransparentCommands(draws.transparentDrawCommands,
                             draws.transparentSingleSidedDrawCommands,
                             draws.transparentWindingFlippedDrawCommands,
                             draws.transparentDoubleSidedDrawCommands)) {
    return true;
  }
  return (includePoints && (hasTransparentGeometry(draws.points) ||
                            hasTransparentGeometry(draws.nativePoints))) ||
         (includeCurves && (hasTransparentGeometry(draws.curves) ||
                             hasTransparentGeometry(draws.nativeCurves)));
}

bool hasTransparentBimSurfaceGeometry(const BimDrawLists& draws,
                                      bool includePoints,
                                      bool includeCurves) {
  if (hasTransparentCommands(draws.transparentDrawCommands,
                             draws.transparentSingleSidedDrawCommands,
                             draws.transparentWindingFlippedDrawCommands,
                             draws.transparentDoubleSidedDrawCommands)) {
    return true;
  }
  return (includePoints && hasTransparentGeometry(draws.points)) ||
         (includeCurves && hasTransparentGeometry(draws.curves));
}

bool hasTransparentBimGeometry(const BimManager& bimManager,
                               bool includePoints,
                               bool includeCurves) {
  if (hasTransparentCommands(
          bimManager.transparentDrawCommands(),
          bimManager.transparentSingleSidedDrawCommands(),
          bimManager.transparentWindingFlippedDrawCommands(),
          bimManager.transparentDoubleSidedDrawCommands())) {
    return true;
  }
  return (includePoints &&
          (hasTransparentGeometry(bimManager.pointDrawLists()) ||
           hasTransparentGeometry(bimManager.nativePointDrawLists()))) ||
          (includeCurves &&
           (hasTransparentGeometry(bimManager.curveDrawLists()) ||
            hasTransparentGeometry(bimManager.nativeCurveDrawLists())));
}

bool hasTransparentBimSurfaceGeometry(const BimManager& bimManager,
                                      bool includePoints,
                                      bool includeCurves) {
  if (hasTransparentCommands(
          bimManager.transparentDrawCommands(),
          bimManager.transparentSingleSidedDrawCommands(),
          bimManager.transparentWindingFlippedDrawCommands(),
          bimManager.transparentDoubleSidedDrawCommands())) {
    return true;
  }
  return (includePoints && hasTransparentGeometry(bimManager.pointDrawLists())) ||
         (includeCurves && hasTransparentGeometry(bimManager.curveDrawLists()));
}

BimFrameGpuVisibilityInputs bimFrameGpuVisibilityInputs(
    const BimManager &bimManager, const BimDrawFilter &filter) {
  const BimVisibilityFilterStats &stats = bimManager.visibilityFilterStats();
  return {.filterActive = filter.active(),
          .gpuResident = stats.gpuResident,
          .computeReady = stats.computeReady,
          .objectCount = stats.objectCount,
          .visibilityMaskReady =
              bimManager.visibilityMaskBuffer().buffer != VK_NULL_HANDLE};
}

BimFrameMeshDrawLists bimFrameMeshDrawLists(const BimManager &bimManager) {
  return {.opaqueDrawCommands = &bimManager.opaqueDrawCommands(),
          .opaqueSingleSidedDrawCommands =
              &bimManager.opaqueSingleSidedDrawCommands(),
          .opaqueWindingFlippedDrawCommands =
              &bimManager.opaqueWindingFlippedDrawCommands(),
          .opaqueDoubleSidedDrawCommands =
              &bimManager.opaqueDoubleSidedDrawCommands(),
          .transparentDrawCommands = &bimManager.transparentDrawCommands(),
          .transparentSingleSidedDrawCommands =
              &bimManager.transparentSingleSidedDrawCommands(),
          .transparentWindingFlippedDrawCommands =
              &bimManager.transparentWindingFlippedDrawCommands(),
          .transparentDoubleSidedDrawCommands =
              &bimManager.transparentDoubleSidedDrawCommands()};
}

BimFrameDrawRoutingInputs bimFrameDrawRoutingInputs(
    const BimManager &bimManager, const BimDrawFilter &filter,
    const container::ui::BimLayerVisibilityState &layers,
    const BimDrawLists *cpuFilteredDraws) {
  return {.gpuVisibility = bimFrameGpuVisibilityInputs(bimManager, filter),
          .pointCloudVisible = layers.pointCloudVisible,
          .curvesVisible = layers.curvesVisible,
          .unfilteredMeshDraws = bimFrameMeshDrawLists(bimManager),
          .unfilteredPointDraws = &bimManager.pointDrawLists(),
          .unfilteredCurveDraws = &bimManager.curveDrawLists(),
          .unfilteredNativePointDraws = &bimManager.nativePointDrawLists(),
          .unfilteredNativeCurveDraws = &bimManager.nativeCurveDrawLists(),
          .cpuFilteredDraws = cpuFilteredDraws};
}

void assignFrameDrawLists(FrameDrawLists &target,
                          const BimFrameMeshDrawLists &source) {
  target.opaqueDrawCommands = source.opaqueDrawCommands;
  target.opaqueSingleSidedDrawCommands = source.opaqueSingleSidedDrawCommands;
  target.opaqueWindingFlippedDrawCommands =
      source.opaqueWindingFlippedDrawCommands;
  target.opaqueDoubleSidedDrawCommands = source.opaqueDoubleSidedDrawCommands;
  target.transparentDrawCommands = source.transparentDrawCommands;
  target.transparentSingleSidedDrawCommands =
      source.transparentSingleSidedDrawCommands;
  target.transparentWindingFlippedDrawCommands =
      source.transparentWindingFlippedDrawCommands;
  target.transparentDoubleSidedDrawCommands =
      source.transparentDoubleSidedDrawCommands;
}

void assignFrameDrawLists(FrameDrawLists& target,
                          const BimGeometryDrawLists& source) {
  target.opaqueDrawCommands = &source.opaqueDrawCommands;
  target.opaqueSingleSidedDrawCommands = &source.opaqueSingleSidedDrawCommands;
  target.opaqueWindingFlippedDrawCommands =
      &source.opaqueWindingFlippedDrawCommands;
  target.opaqueDoubleSidedDrawCommands = &source.opaqueDoubleSidedDrawCommands;
  target.transparentDrawCommands = &source.transparentDrawCommands;
  target.transparentSingleSidedDrawCommands =
      &source.transparentSingleSidedDrawCommands;
  target.transparentWindingFlippedDrawCommands =
      &source.transparentWindingFlippedDrawCommands;
  target.transparentDoubleSidedDrawCommands =
      &source.transparentDoubleSidedDrawCommands;
}

bool isFiniteVec3(const glm::vec3 &value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

glm::vec3 normalizedOr(const glm::vec3 &value, const glm::vec3 &fallback) {
  const float length = glm::length(value);
  if (!std::isfinite(length) || length <= 0.0001f) {
    return fallback;
  }
  return value / length;
}

glm::vec4
sectionPlaneEquation(const container::ui::SectionPlaneState &sectionPlane) {
  const glm::vec3 normal =
      normalizedOr(sectionPlane.normal, {0.0f, 1.0f, 0.0f});
  return {normal, -sectionPlane.offset};
}

std::array<glm::vec4, 6>
makeBoxClipPlanes(const container::ui::BimBoxClipUiState& boxClip) {
  const glm::vec3 minBounds = glm::min(boxClip.min, boxClip.max);
  const glm::vec3 maxBounds = glm::max(boxClip.min, boxClip.max);
  return {{
      {1.0f, 0.0f, 0.0f, -minBounds.x},
      {-1.0f, 0.0f, 0.0f, maxBounds.x},
      {0.0f, 1.0f, 0.0f, -minBounds.y},
      {0.0f, -1.0f, 0.0f, maxBounds.y},
      {0.0f, 0.0f, 1.0f, -minBounds.z},
      {0.0f, 0.0f, -1.0f, maxBounds.z},
  }};
}

bool currentSectionPlaneEquation(const container::ui::GuiManager *guiManager,
                                 glm::vec4 &outPlane) {
  if (guiManager == nullptr) {
    return false;
  }
  const auto &sectionPlane = guiManager->sectionPlaneState();
  if (!sectionPlane.enabled) {
    return false;
  }
  outPlane = sectionPlaneEquation(sectionPlane);
  return true;
}

void includeBoundingSphere(const glm::vec4 &sphere, glm::vec3 &boundsMin,
                           glm::vec3 &boundsMax, bool &hasBounds) {
  const glm::vec3 center{sphere.x, sphere.y, sphere.z};
  const float radius = std::max(sphere.w, 0.0f);
  if (!isFiniteVec3(center) || !std::isfinite(radius)) {
    return;
  }

  const glm::vec3 extent{radius};
  if (!hasBounds) {
    boundsMin = center - extent;
    boundsMax = center + extent;
    hasBounds = true;
    return;
  }

  boundsMin = glm::min(boundsMin, center - extent);
  boundsMax = glm::max(boundsMax, center + extent);
}

bool accumulateDrawCommandBounds(
    const std::vector<DrawCommand> &commands,
    const std::vector<container::gpu::ObjectData> &objectData,
    glm::vec3 &boundsMin, glm::vec3 &boundsMax) {
  bool hasBounds = false;
  for (const DrawCommand &command : commands) {
    const uint32_t instanceCount = std::max(command.instanceCount, 1u);
    for (uint32_t instance = 0; instance < instanceCount; ++instance) {
      const uint32_t objectIndex = command.objectIndex + instance;
      if (objectIndex >= objectData.size()) {
        continue;
      }
      includeBoundingSphere(objectData[objectIndex].boundingSphere, boundsMin,
                            boundsMax, hasBounds);
    }
  }
  return hasBounds;
}

float transformGizmoScale(const glm::vec3 &origin, float boundsRadius,
                          const CameraData &cameraData) {
  const glm::vec3 cameraPosition{cameraData.cameraWorldPosition};
  const float distance = glm::length(cameraPosition - origin);
  const float screenScale = std::isfinite(distance) ? distance * 0.075f : 1.0f;
  const float radiusScale = std::max(boundsRadius * 0.45f, 0.18f);
  const float maxScale = std::max(boundsRadius * 1.75f, 0.35f);
  return std::clamp(std::max(screenScale, radiusScale), 0.18f, maxScale);
}

std::optional<glm::vec2> projectToFramebuffer(const CameraData &cameraData,
                                              VkExtent2D viewportExtent,
                                              const glm::vec3 &worldPosition) {
  if (viewportExtent.width == 0u || viewportExtent.height == 0u) {
    return std::nullopt;
  }

  const glm::vec4 clip =
      cameraData.viewProj * glm::vec4(worldPosition, 1.0f);
  if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
      !std::isfinite(clip.w) || clip.w <= 0.0001f) {
    return std::nullopt;
  }

  const glm::vec2 ndc{clip.x / clip.w, clip.y / clip.w};
  return glm::vec2{
      (ndc.x * 0.5f + 0.5f) * static_cast<float>(viewportExtent.width),
      (1.0f - (ndc.y * 0.5f + 0.5f)) *
          static_cast<float>(viewportExtent.height),
  };
}

float distanceSquaredToSegment(const glm::vec2 &point, const glm::vec2 &start,
                               const glm::vec2 &end) {
  const glm::vec2 segment = end - start;
  const float lengthSquared = glm::dot(segment, segment);
  if (lengthSquared <= 0.0001f) {
    const glm::vec2 delta = point - start;
    return glm::dot(delta, delta);
  }

  const float t =
      std::clamp(glm::dot(point - start, segment) / lengthSquared, 0.0f, 1.0f);
  const glm::vec2 closest = start + segment * t;
  const glm::vec2 delta = point - closest;
  return glm::dot(delta, delta);
}

float snapRelativeFloat(float base, float value, float step) {
  if (step <= 0.0f) {
    return value;
  }
  return base + std::round((value - base) / step) * step;
}

glm::vec3 snapRelativeVec3(const glm::vec3 &base, const glm::vec3 &value,
                           float step) {
  return {
      snapRelativeFloat(base.x, value.x, step),
      snapRelativeFloat(base.y, value.y, step),
      snapRelativeFloat(base.z, value.z, step),
  };
}

enum class GpuPickTargetKind {
  None,
  Scene,
  Bim,
};

struct GpuPickTarget {
  GpuPickTargetKind kind{GpuPickTargetKind::None};
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
};

GpuPickTarget decodeGpuPickId(uint32_t pickId) {
  if (pickId == container::gpu::kPickIdNone) {
    return {};
  }

  const bool isBim = (pickId & container::gpu::kPickIdBimMask) != 0u;
  const uint32_t encodedObject = pickId & container::gpu::kPickIdObjectMask;
  if (encodedObject == 0u) {
    return {};
  }

  return GpuPickTarget{
      .kind = isBim ? GpuPickTargetKind::Bim : GpuPickTargetKind::Scene,
      .objectIndex = encodedObject - 1u,
  };
}

std::optional<glm::vec3> unprojectDepthAtCursor(
    const container::gpu::CameraData &cameraData, VkExtent2D viewportExtent,
    double cursorX, double cursorY, float depth) {
  if (viewportExtent.width == 0u || viewportExtent.height == 0u ||
      cursorX < 0.0 || cursorY < 0.0 ||
      cursorX >= static_cast<double>(viewportExtent.width) ||
      cursorY >= static_cast<double>(viewportExtent.height) ||
      !std::isfinite(depth)) {
    return std::nullopt;
  }

  const float ndcX =
      static_cast<float>((cursorX / static_cast<double>(viewportExtent.width)) *
                             2.0 -
                         1.0);
  const float ndcY =
      static_cast<float>(1.0 -
                         (cursorY / static_cast<double>(viewportExtent.height)) *
                             2.0);
  glm::vec4 world = cameraData.inverseViewProj *
                    glm::vec4(ndcX, ndcY, std::clamp(depth, 0.0f, 1.0f), 1.0f);
  if (world.w == 0.0f) {
    return std::nullopt;
  }
  world /= world.w;
  const glm::vec3 point{world};
  return isFiniteVec3(point) ? std::optional<glm::vec3>{point} : std::nullopt;
}

container::ui::GBufferViewMode
currentDisplayMode(const container::ui::GuiManager *guiManager) {
  return guiManager ? guiManager->gBufferViewMode()
                    : container::ui::GBufferViewMode::Overview;
}

bool displayModeRecordsShadowAtlas(container::ui::GBufferViewMode mode) {
  return mode == container::ui::GBufferViewMode::Lit;
}

bool displayModeRecordsTileCull(container::ui::GBufferViewMode mode) {
  return mode == container::ui::GBufferViewMode::Lit ||
         mode == container::ui::GBufferViewMode::TileLightHeatMap;
}

bool displayModeRecordsGtao(container::ui::GBufferViewMode mode) {
  return mode == container::ui::GBufferViewMode::Lit;
}

std::string bimSelectionLabel(const BimElementMetadata &metadata) {
  std::string label =
      "Selected BIM object " + std::to_string(metadata.objectIndex);
  if (!metadata.type.empty() && metadata.type != "Unknown") {
    label += " (" + metadata.type + ")";
  }
  if (!metadata.guid.empty()) {
    label += " [" + metadata.guid + "]";
  }
  return label;
}

std::string bimGeometryKindLabel(BimGeometryKind kind) {
  switch (kind) {
  case BimGeometryKind::Points:
    return "points";
  case BimGeometryKind::Curves:
    return "curves";
  case BimGeometryKind::Mesh:
  default:
    return "mesh";
  }
}

struct FrameFeatureReadiness {
  bool shadowAtlas{false};
  bool gtao{false};
  bool tileCull{false};
};

bool hasShadowAtlasResources(const ShadowManager *shadowManager) {
  if (shadowManager == nullptr ||
      shadowManager->shadowAtlasArrayView() == VK_NULL_HANDLE ||
      shadowManager->shadowSampler() == VK_NULL_HANDLE) {
    return false;
  }

  return std::ranges::any_of(shadowManager->shadowUbos(), [](const auto &ubo) {
    return ubo.buffer != VK_NULL_HANDLE;
  });
}

bool hasGtaoResources(const EnvironmentManager *environmentManager) {
  return environmentManager != nullptr && environmentManager->isGtaoReady() &&
         environmentManager->isAoEnabled() &&
         environmentManager->aoTextureView() != VK_NULL_HANDLE &&
         environmentManager->aoSampler() != VK_NULL_HANDLE;
}

bool hasTileCullResources(const LightingManager *lightingManager) {
  return lightingManager != nullptr &&
         lightingManager->isTiledLightingReady() &&
         lightingManager->tileGridBuffer() != VK_NULL_HANDLE &&
         lightingManager->tileGridBufferSize() > 0;
}

bool graphPassScheduled(const FrameRecorder *frameRecorder, RenderPassId id) {
  if (frameRecorder == nullptr) {
    return false;
  }
  const auto *status = frameRecorder->graph().executionStatus(id);
  return status != nullptr && status->active;
}

bool anyShadowCascadeScheduled(const FrameRecorder *frameRecorder) {
  return std::ranges::any_of(shadowCascadePassIds(), [frameRecorder](auto id) {
    return graphPassScheduled(frameRecorder, id);
  });
}

FrameFeatureReadiness
evaluateFrameFeatureReadiness(container::ui::GBufferViewMode displayMode,
                              const FrameRecorder *frameRecorder,
                              const ShadowManager *shadowManager,
                              const EnvironmentManager *environmentManager,
                              const LightingManager *lightingManager) {
  FrameFeatureReadiness readiness{};
  readiness.shadowAtlas = displayModeRecordsShadowAtlas(displayMode) &&
                          anyShadowCascadeScheduled(frameRecorder) &&
                          hasShadowAtlasResources(shadowManager);
  readiness.gtao = displayModeRecordsGtao(displayMode) &&
                   graphPassScheduled(frameRecorder, RenderPassId::GTAO) &&
                   hasGtaoResources(environmentManager);
  readiness.tileCull =
      displayModeRecordsTileCull(displayMode) &&
      graphPassScheduled(frameRecorder, RenderPassId::TileCull) &&
      hasTileCullResources(lightingManager);
  return readiness;
}

bool sameVec4(const glm::vec4 &lhs, const glm::vec4 &rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

bool sameMat4(const glm::mat4 &lhs, const glm::mat4 &rhs) {
  for (int column = 0; column < 4; ++column) {
    if (!sameVec4(lhs[column], rhs[column])) {
      return false;
    }
  }
  return true;
}

bool sameCameraData(const container::gpu::CameraData &lhs,
                    const container::gpu::CameraData &rhs) {
  return sameMat4(lhs.viewProj, rhs.viewProj) &&
         sameMat4(lhs.inverseViewProj, rhs.inverseViewProj) &&
         sameVec4(lhs.cameraWorldPosition, rhs.cameraWorldPosition) &&
         sameVec4(lhs.cameraForward, rhs.cameraForward);
}

container::scene::SceneProviderBounds sceneProviderBoundsFromModelBounds(
    const container::scene::ModelBounds& bounds) {
  return {
      .min = bounds.min,
      .max = bounds.max,
      .valid = bounds.valid,
  };
}

container::scene::SceneProviderBounds sceneProviderBoundsFromBim(
    const BimManager& bimManager) {
  container::scene::SceneProviderBounds bounds{};
  for (const BimElementMetadata& metadata : bimManager.elementMetadata()) {
    if (!metadata.bounds.valid) {
      continue;
    }
    if (!bounds.valid) {
      bounds.min = metadata.bounds.min;
      bounds.max = metadata.bounds.max;
      bounds.valid = true;
      continue;
    }
    bounds.min.x = std::min(bounds.min.x, metadata.bounds.min.x);
    bounds.min.y = std::min(bounds.min.y, metadata.bounds.min.y);
    bounds.min.z = std::min(bounds.min.z, metadata.bounds.min.z);
    bounds.max.x = std::max(bounds.max.x, metadata.bounds.max.x);
    bounds.max.y = std::max(bounds.max.y, metadata.bounds.max.y);
    bounds.max.z = std::max(bounds.max.z, metadata.bounds.max.z);
  }
  return bounds;
}

std::vector<container::scene::MeshSceneProviderPrimitive>
meshSceneProviderPrimitivesFromSceneManager(
    const container::scene::SceneManager& sceneManager) {
  std::vector<container::scene::MeshSceneProviderPrimitive> primitives;
  primitives.reserve(sceneManager.primitiveRanges().size());

  for (const auto& primitive : sceneManager.primitiveRanges()) {
    primitives.push_back(container::scene::MeshSceneProviderPrimitive{
        .firstIndex = primitive.firstIndex,
        .indexCount = primitive.indexCount,
        .materialIndex = primitive.materialIndex,
        .doubleSided = primitive.disableBackfaceCulling,
    });
  }

  return primitives;
}

std::vector<container::scene::MeshSceneMaterialProperties>
meshSceneMaterialPropertiesFromSceneManager(
    const container::scene::SceneManager& sceneManager) {
  std::vector<container::scene::MeshSceneMaterialProperties> materials;
  materials.reserve(sceneManager.materialCount());
  for (std::size_t materialIndex = 0; materialIndex < sceneManager.materialCount();
       ++materialIndex) {
    const auto properties =
        sceneManager.materialRenderProperties(static_cast<uint32_t>(materialIndex));
    materials.push_back(container::scene::MeshSceneMaterialProperties{
        .transparent = properties.transparent,
        .doubleSided = properties.doubleSided,
    });
  }
  return materials;
}

container::scene::MeshSceneAsset meshSceneAssetFromSceneManager(
    const container::scene::SceneManager& sceneManager,
    std::size_t instanceCount,
    container::scene::SceneProviderBounds bounds) {
  const std::vector<container::scene::MeshSceneProviderPrimitive> primitives =
      meshSceneProviderPrimitivesFromSceneManager(sceneManager);
  const std::vector<container::scene::MeshSceneMaterialProperties> materials =
      meshSceneMaterialPropertiesFromSceneManager(sceneManager);
  return container::scene::buildMeshSceneAsset({
      .primitives =
          std::span<const container::scene::MeshSceneProviderPrimitive>{
              primitives.data(), primitives.size()},
      .materials =
          std::span<const container::scene::MeshSceneMaterialProperties>{
              materials.data(), materials.size()},
      .materialCount = sceneManager.materialCount(),
      .instanceCount = instanceCount,
      .bounds = bounds,
  });
}

std::string sceneProviderDisplayName(std::string_view modelPath,
                                     std::string_view fallback) {
  if (modelPath.empty()) {
    return std::string(fallback);
  }
  const size_t slash = modelPath.find_last_of("/\\");
  const std::string_view fileName =
      slash == std::string_view::npos ? modelPath : modelPath.substr(slash + 1);
  return fileName.empty() ? std::string(fallback) : std::string(fileName);
}

} // namespace

RendererFrontend::RendererFrontend(RendererFrontendCreateInfo info)
    : svc_{*info.ctx,
           *info.pipelineManager,
           *info.allocationManager,
           *info.swapChainManager,
           *info.commandBufferManager,
           *info.config,
           info.nativeWindow,
           *info.inputManager} {}

RendererFrontend::~RendererFrontend() { shutdown(); }

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void RendererFrontend::initialize() {
  // Initialization order mirrors runtime dependencies: render passes define
  // attachment compatibility, scene/lighting managers create descriptor
  // layouts, and only then can graphics pipelines be built.
  subs_.renderPassManager =
      std::make_unique<RenderPassManager>(svc_.ctx.deviceWrapper);
  subs_.oitManager = std::make_unique<OitManager>(svc_.ctx.deviceWrapper);
  createRenderPasses();

  subs_.sceneManager = std::make_unique<container::scene::SceneManager>(
      svc_.allocationManager, svc_.pipelineManager, svc_.ctx.deviceWrapper,
      svc_.config);
  subs_.sceneManager->initialize(
      svc_.config.modelPath, svc_.config.importScale,
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  sceneState_.indexType = subs_.sceneManager->indexType();
  activePrimaryModelPath_ = svc_.config.modelPath;
  activePrimaryImportScale_ = svc_.config.importScale;

  if (!svc_.config.bimModelPath.empty()) {
    subs_.bimManager = std::make_unique<BimManager>(
        svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
    subs_.bimManager->createMeshletResidencyResources(
        container::util::executableDirectory());
    subs_.bimManager->loadModel(svc_.config.bimModelPath,
                                svc_.config.bimImportScale,
                                *subs_.sceneManager);
    activeAuxiliaryModelPath_ = svc_.config.bimModelPath;
    activeAuxiliaryImportScale_ = svc_.config.bimImportScale;
  }

  subs_.sceneController = std::make_unique<SceneController>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      sceneGraph_, *subs_.sceneManager, nullptr, svc_.config);
  subs_.lightingManager = std::make_unique<LightingManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      subs_.sceneManager.get(), sceneGraph_, subs_.sceneController->world());
  {
    auto lightingSettings = subs_.lightingManager->lightingSettings();
    if (svc_.config.hasEnvironmentIntensityOverride) {
      lightingSettings.environmentIntensity = svc_.config.environmentIntensity;
    }
    if (svc_.config.hasDirectionalIntensityOverride) {
      lightingSettings.directionalIntensity = svc_.config.directionalIntensity;
    }
    subs_.lightingManager->setLightingSettings(lightingSettings);
  }
  subs_.shadowManager = std::make_unique<ShadowManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.shadowManager->createResources(
      resources_.gBufferFormats.depthStencil,
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.shadowManager->createFramebuffers(resources_.renderPasses.shadow);
  subs_.shadowCullManager = std::make_unique<ShadowCullManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.shadowCullManager->createResources(
      container::util::executableDirectory(),
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.environmentManager = std::make_unique<EnvironmentManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.commandBufferManager.pool());
  subs_.environmentManager->createResources(
      container::util::executableDirectory());
  {
    const auto exeDir = container::util::executableDirectory();
    const std::filesystem::path hdrPath =
        exeDir / container::app::kDefaultEnvironmentHdrRelativePath;
    if (std::filesystem::exists(hdrPath)) {
      subs_.environmentManager->loadHdrEnvironment(exeDir, hdrPath);
    }
  }
  subs_.gpuCullManager = std::make_unique<GpuCullManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.gpuCullManager->createResources(container::util::executableDirectory());
  subs_.bloomManager = std::make_unique<BloomManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.commandBufferManager.pool());
  subs_.bloomManager->createResources(container::util::executableDirectory());
  subs_.exposureManager = std::make_unique<ExposureManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.exposureManager->createResources(
      container::util::executableDirectory());
  subs_.frameResourceManager = std::make_unique<FrameResourceManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.swapChainManager, svc_.commandBufferManager.pool());
  subs_.frameResourceManager->createDescriptorSetLayouts();
  subs_.frameResourceManager->createGBufferSampler();
  subs_.lightingManager->createDescriptorResources(
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.lightingManager->createTiledResources(
      container::util::executableDirectory(), svc_.swapChainManager.extent());
  createGraphicsPipelines();
  svc_.swapChainManager.createFramebuffers(resources_.renderPasses.postProcess);

  createCamera();

  if (svc_.config.enableGui) {
    subs_.guiManager = std::make_unique<container::ui::GuiManager>();
    subs_.guiManager->initialize(
        svc_.ctx.instance, svc_.ctx.deviceWrapper->device(),
        svc_.ctx.deviceWrapper->physicalDevice(),
        svc_.ctx.deviceWrapper->graphicsQueue(),
        svc_.ctx.deviceWrapper->queueFamilyIndices().graphicsFamily.value(),
        resources_.renderPasses.postProcess,
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()),
        svc_.nativeWindow, svc_.config.modelPath, svc_.config.importScale);
    subs_.guiManager->setWireframeCapabilities(
        svc_.ctx.wireframeSupported, svc_.ctx.wireframeRasterModeSupported,
        svc_.ctx.wireframeWideLinesSupported);
    if (subs_.environmentManager) {
      subs_.guiManager->setEnvironmentStatus(
          subs_.environmentManager->environmentStatus());
    }
    if (subs_.sceneController)
      subs_.sceneController->setGuiManager(subs_.guiManager.get());
  }

  subs_.frameRecorder = std::make_unique<FrameRecorder>();
  subs_.deferredRasterFrameGraphContext =
      std::make_unique<DeferredRasterFrameGraphContext>(
          DeferredRasterFrameGraphServices{
              .graph = &subs_.frameRecorder->graph(),
              .swapChainManager = &svc_.swapChainManager,
              .oitManager = subs_.oitManager.get(),
              .lightingManager = subs_.lightingManager.get(),
              .environmentManager = subs_.environmentManager.get(),
              .sceneController = subs_.sceneController.get(),
              .gpuCullManager = subs_.gpuCullManager.get(),
              .bloomManager = subs_.bloomManager.get(),
              .exposureManager = subs_.exposureManager.get(),
              .camera = subs_.cameraController
                            ? subs_.cameraController->camera()
                            : nullptr,
              .guiManager = subs_.guiManager.get()});
  subs_.frameResourceRegistry = std::make_unique<FrameResourceRegistry>();
  subs_.frameRuntimeResourceRegistry =
      std::make_unique<FrameResourceRegistry>();
  subs_.pipelineRegistry = std::make_unique<PipelineRegistry>();
  subs_.sceneProviderSynchronizer =
      std::make_unique<SceneProviderSynchronizer>();
  subs_.sceneProviderRegistry =
      std::make_unique<container::scene::SceneProviderRegistry>();
  syncSceneProviders();
  auto techniqueRegistry = createDefaultRenderTechniqueRegistry();
  subs_.techniqueRegistry =
      std::make_unique<RenderTechniqueRegistry>(std::move(techniqueRegistry));
  const RenderTechniqueId requestedTechnique =
      renderTechniqueIdFromName(svc_.config.renderTechnique)
          .value_or(RenderTechniqueId::DeferredRaster);
  RenderSystemContext techniqueContext{
      .frameRecorder = subs_.frameRecorder.get(),
      .deferredRaster = subs_.deferredRasterFrameGraphContext.get(),
      .deviceCapabilities = &subs_.deviceCapabilities,
      .sceneProviders = subs_.sceneProviderRegistry.get(),
      .frameResources = subs_.frameResourceRegistry.get(),
      .pipelines = subs_.pipelineRegistry.get(),
  };
  const RenderTechniqueSelection techniqueSelection =
      subs_.techniqueRegistry->select(requestedTechnique, techniqueContext);
  subs_.activeTechnique = techniqueSelection.technique;
  if (subs_.activeTechnique == nullptr) {
    throw std::runtime_error(
        "No available render technique; deferred raster fallback unavailable");
  }
  subs_.activeTechnique->registerTechniqueContracts(techniqueContext);
  subs_.activeTechnique->buildFrameGraph(techniqueContext);
  if (techniqueSelection.usedFallback) {
    container::log::ContainerLogger::instance().renderer()->warn(
        "Requested render technique '{}' unavailable ({}); using '{}'.",
        svc_.config.renderTechnique, techniqueSelection.unavailableReason,
        subs_.activeTechnique->name());
  } else {
    container::log::ContainerLogger::instance().renderer()->info(
        "Active render technique: {}", subs_.activeTechnique->name());
  }

  svc_.commandBufferManager.allocate(svc_.swapChainManager.imageCount());
  svc_.commandBufferManager.configureSecondaryBuffers(
      static_cast<uint32_t>(shadowCascadePassIds().size()), 1);
  subs_.frameSyncManager = std::make_unique<container::gpu::FrameSyncManager>(
      svc_.ctx.deviceWrapper->device(), svc_.config.maxFramesInFlight);
  subs_.frameSyncManager->initialize(
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  frame_.imagesInFlight.assign(svc_.swapChainManager.imageCount(),
                               VK_NULL_HANDLE);
  subs_.renderPassGpuProfiler = std::make_unique<RenderPassGpuProfiler>();
  subs_.renderPassGpuProfiler->initialize(
      svc_.ctx.deviceWrapper->device(), svc_.ctx.instance,
      svc_.ctx.deviceWrapper->physicalDevice(),
      svc_.ctx.deviceWrapper->queueFamilyIndices().graphicsFamily.value(),
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  container::log::ContainerLogger::instance().renderer()->info(
      "GPU pass profiler: {}", subs_.renderPassGpuProfiler->backendStatus());
  subs_.rendererTelemetry = std::make_unique<RendererTelemetry>();

  initializeScene();
}

bool RendererFrontend::drawFrame(bool &framebufferResized) {
  const auto frameStart = TelemetryClock::now();
  const auto concurrencyPolicy = FrameConcurrencyPolicy::serializedGpuResources(
      "shared object buffer plus GPU cull, tile cull, GTAO, bloom, "
      "exposure, and readback resources are not yet per-frame");
  auto *telemetry = subs_.rendererTelemetry.get();
  if (telemetry) {
    telemetry->beginFrame(frame_.submittedFrameCount, frame_.currentFrame,
                          svc_.config.maxFramesInFlight,
                          concurrencyPolicy.mode() ==
                              FrameConcurrencyMode::SerializedGpuResources,
                          concurrencyPolicy.reason());
  }

  auto phaseStart = TelemetryClock::now();
  concurrencyPolicy.waitBeforeAcquire(*subs_.frameSyncManager,
                                      frame_.currentFrame);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::WaitForFrame,
                           elapsedMilliseconds(phaseStart));
    if (subs_.renderPassGpuProfiler) {
      const auto gpuTimings = subs_.renderPassGpuProfiler->collectLatest();
      telemetry->setPassGpuTimings(gpuTimings,
                                   subs_.renderPassGpuProfiler->timingSource());
      telemetry->setGpuProfilerStatus(RendererGpuProfilerTelemetry{
          .source = subs_.renderPassGpuProfiler->timingSource(),
          .available = subs_.renderPassGpuProfiler->isReady(),
          .resultLatencyFrames =
              subs_.renderPassGpuProfiler->resultLatencyFrames(),
          .status = std::string(subs_.renderPassGpuProfiler->backendStatus()),
      });
    }
  }

  // Collect culling statistics from the previous frame (now safe after fence).
  phaseStart = TelemetryClock::now();
  if (subs_.gpuCullManager)
    subs_.gpuCullManager->collectStats();
  if (subs_.lightingManager)
    subs_.lightingManager->collectStats();
  if (subs_.exposureManager) {
    subs_.exposureManager->collectReadback(
        subs_.guiManager ? subs_.guiManager->exposureSettings()
                         : container::gpu::ExposureSettings{});
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::Readbacks,
                           elapsedMilliseconds(phaseStart));
    if (subs_.gpuCullManager) {
      telemetry->setCullingStats(subs_.gpuCullManager->cullStats());
    }
    if (subs_.lightingManager) {
      telemetry->setLightCullingStats(
          subs_.lightingManager->lightCullingStats());
    }
  }

  uint32_t imageIndex = 0;
  phaseStart = TelemetryClock::now();
  VkResult result = vkAcquireNextImageKHR(
      svc_.ctx.deviceWrapper->device(), svc_.swapChainManager.swapChain(),
      UINT64_MAX, subs_.frameSyncManager->imageAvailable(frame_.currentFrame),
      VK_NULL_HANDLE, &imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::AcquireImage,
                           elapsedMilliseconds(phaseStart));
  }

  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    phaseStart = TelemetryClock::now();
    handleResize();
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
      telemetry->setCpuPhase(RendererTelemetryPhase::Frame,
                             elapsedMilliseconds(frameStart));
      telemetry->endFrame();
    }
    ++frame_.submittedFrameCount;
    return true;
  } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    throw std::runtime_error("failed to acquire swap chain image!");
  }
  if (telemetry) {
    telemetry->setImageIndex(imageIndex);
  }

  phaseStart = TelemetryClock::now();
  if (frame_.imagesInFlight[imageIndex]) {
    vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1,
                    &frame_.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::ImageFenceWait,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  growExactOitNodePoolIfNeeded(imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                           elapsedMilliseconds(phaseStart));
  }

  subs_.frameSyncManager->resetFence(frame_.currentFrame);
  frame_.imagesInFlight[imageIndex] =
      subs_.frameSyncManager->fence(frame_.currentFrame);

  phaseStart = TelemetryClock::now();
  presentSceneControls();
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::Gui,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  updateObjectBuffer();
  applyBimSemanticColorMode();
  updateCameraBuffer(imageIndex);

  if (subs_.lightingManager && subs_.cameraController) {
    subs_.lightingManager->updateLightingDataForActiveCamera();
  }

  if (subs_.shadowManager && subs_.lightingManager && subs_.cameraController) {
    const auto &ld = subs_.lightingManager->lightingData();
    const float aspect =
        static_cast<float>(svc_.swapChainManager.extent().width) /
        static_cast<float>(svc_.swapChainManager.extent().height);
    const container::gpu::ShadowSettings shadowSettings =
        subs_.guiManager ? subs_.guiManager->shadowSettings()
                         : container::gpu::ShadowSettings{};
    subs_.shadowManager->update(subs_.cameraController->camera(), aspect,
                                glm::vec3(ld.directionalDirection),
                                shadowSettings, imageIndex);
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::SceneUpdate,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  updateFrameDescriptorSets(imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::DescriptorUpdate,
                           elapsedMilliseconds(phaseStart));
  }

  const bool screenshotThisFrame = screenshot_.pending;
  if (screenshotThisFrame) {
    phaseStart = TelemetryClock::now();
    ensureScreenshotReadbackBuffer(svc_.swapChainManager.extent(),
                                   svc_.swapChainManager.imageFormat());
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
    }
  }

  phaseStart = TelemetryClock::now();
  vkResetCommandBuffer(svc_.commandBufferManager.buffer(imageIndex), 0);
  recordCommandBuffer(svc_.commandBufferManager.buffer(imageIndex), imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::CommandRecord,
                           elapsedMilliseconds(phaseStart));
    if (subs_.frameRecorder) {
      telemetry->setRenderGraph(subs_.frameRecorder->graph());
    }
  }

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  VkPerformanceQuerySubmitInfoKHR performanceSubmitInfo{};
  if (subs_.renderPassGpuProfiler &&
      subs_.renderPassGpuProfiler->usesPerformanceQueries()) {
    performanceSubmitInfo.sType =
        VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR;
    performanceSubmitInfo.counterPassIndex = 0u;
    submitInfo.pNext = &performanceSubmitInfo;
  }

  VkSemaphore waitSemaphores[] = {
      subs_.frameSyncManager->imageAvailable(frame_.currentFrame)};
  VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;

  VkCommandBuffer cmdHandle = svc_.commandBufferManager.buffer(imageIndex);
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmdHandle;

  VkSemaphore signalSemaphores[] = {
      subs_.frameSyncManager->renderFinishedForImage(imageIndex)};
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;

  phaseStart = TelemetryClock::now();
  if (vkQueueSubmit(svc_.ctx.deviceWrapper->graphicsQueue(), 1, &submitInfo,
                    subs_.frameSyncManager->fence(frame_.currentFrame)) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to submit draw command buffer!");
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::QueueSubmit,
                           elapsedMilliseconds(phaseStart));
  }

  if (screenshotThisFrame) {
    phaseStart = TelemetryClock::now();
    const VkFence fence = subs_.frameSyncManager->fence(frame_.currentFrame);
    if (vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1, &fence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
      throw std::runtime_error("failed to wait for screenshot frame");
    }
    writePendingScreenshotPng();
    if (telemetry) {
      telemetry->setCpuPhase(RendererTelemetryPhase::Screenshot,
                             elapsedMilliseconds(phaseStart));
    }
  }

  phaseStart = TelemetryClock::now();
  result = svc_.swapChainManager.present(
      svc_.ctx.deviceWrapper->presentQueue(), imageIndex,
      subs_.frameSyncManager->renderFinishedForImage(imageIndex));
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::Present,
                           elapsedMilliseconds(phaseStart));
  }

  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
      framebufferResized) {
    framebufferResized = false;
    phaseStart = TelemetryClock::now();
    handleResize();
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
    }
  } else if (result != VK_SUCCESS) {
    throw std::runtime_error("failed to present swap chain image!");
  } else {
    markDepthVisibilityFrameComplete(imageIndex);
  }

  if (telemetry) {
    RendererWorkloadTelemetry workload{};
    if (subs_.sceneController) {
      workload.objectCount =
          saturatingU32(subs_.sceneController->objectData().size());
      workload.opaqueDrawCount =
          saturatingU32(subs_.sceneController->opaqueDrawCommands().size());
      workload.transparentDrawCount = saturatingU32(
          subs_.sceneController->transparentDrawCommands().size());
      workload.totalDrawCount =
          saturatingU32(static_cast<size_t>(workload.opaqueDrawCount) +
                        static_cast<size_t>(workload.transparentDrawCount));
    }
    if (subs_.lightingManager) {
      workload.submittedLights =
          subs_.lightingManager->lightCullingStats().submittedLights;
    }
    telemetry->setWorkload(workload);

    RendererResourceTelemetry resources{};
    const VkExtent2D extent = svc_.swapChainManager.extent();
    resources.swapchainWidth = extent.width;
    resources.swapchainHeight = extent.height;
    resources.swapchainImageCount =
        saturatingU32(svc_.swapChainManager.imageCount());
    resources.cameraBufferCount = saturatingU32(buffers_.cameras.size());
    resources.objectBufferCapacity = saturatingU32(buffers_.objectCapacity);
    resources.oitNodeCapacity = deferredRasterRuntimeOitNodeCapacity(
        subs_.frameResourceManager.get(), imageIndex);
    telemetry->setResources(resources);
    telemetry->setCpuPhase(RendererTelemetryPhase::Frame,
                           elapsedMilliseconds(frameStart));
    telemetry->endFrame();
  }

  frame_.currentFrame =
      (frame_.currentFrame + 1) % svc_.config.maxFramesInFlight;
  ++frame_.submittedFrameCount;
  return true;
}

void RendererFrontend::handleResize() {
  if (subs_.rendererTelemetry) {
    subs_.rendererTelemetry->noteDeviceWaitIdle();
    subs_.rendererTelemetry->noteSwapchainRecreate();
  }
  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());

  destroyGBufferResources();
  depthVisibility_.valid = false;
  depthVisibility_.renderFence = VK_NULL_HANDLE;
  svc_.swapChainManager.recreate(resources_.renderPasses.postProcess);
  ensureCameraBuffers();
  if (subs_.lightingManager) {
    subs_.lightingManager->createDescriptorResources(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  if (subs_.shadowManager) {
    subs_.shadowManager->recreatePerFrameResources(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  if (subs_.shadowCullManager) {
    subs_.shadowCullManager->recreatePerFrameResources(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  createFrameResources();
  if (subs_.environmentManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.environmentManager->recreateGtaoTextures(ext.width, ext.height);
  }
  if (subs_.bloomManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.bloomManager->createTextures(ext.width, ext.height);
  }
  svc_.commandBufferManager.reallocate(svc_.swapChainManager.imageCount());

  const uint32_t imageCount =
      static_cast<uint32_t>(svc_.swapChainManager.imageCount());
  if (subs_.guiManager)
    subs_.guiManager->updateSwapchainImageCount(imageCount);

  subs_.frameSyncManager->recreateRenderFinishedSemaphores(
      svc_.swapChainManager.imageCount());
  frame_.imagesInFlight.assign(svc_.swapChainManager.imageCount(),
                               VK_NULL_HANDLE);
  if (subs_.renderPassGpuProfiler) {
    subs_.renderPassGpuProfiler->recreate(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }

  for (uint32_t imageIndex = 0;
       imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
       ++imageIndex) {
    updateCameraBuffer(imageIndex);
  }
  updateObjectBuffer();
  if (subs_.sceneManager) {
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.object);
    if (subs_.bimManager && subs_.bimManager->hasScene()) {
      subs_.sceneManager->updateAuxiliaryDescriptorSets(
          buffers_.cameras, subs_.bimManager->objectAllocatedBuffer());
    }
  }
  updateFrameDescriptorSets();
}

void RendererFrontend::processInput(float deltaTime) {
  if (!subs_.cameraController) {
    svc_.inputManager.endFrame();
    return;
  }

  // Input updates the CPU camera object only. GPU camera buffers are uploaded
  // in drawFrame() after all in-flight work has completed, so culling, depth,
  // and G-buffer passes cannot race a previous frame still reading them.
  syncCameraSelectionPivotOverride();
  subs_.cameraController->updateViewAnimation(deltaTime);
  interactionController_.process(RenderSurfaceInteractionController::Context{
      .inputManager = svc_.inputManager,
      .cameraController = *subs_.cameraController,
      .debugState = debugState_,
      .deltaTime = deltaTime,
      .guiCapturingMouse =
          subs_.guiManager ? subs_.guiManager->isCapturingMouse() : false,
      .guiCapturingKeyboard =
          subs_.guiManager ? subs_.guiManager->isCapturingKeyboard() : false,
      .selectedMeshNode = sceneState_.selectedMeshNode,
      .hasSelection =
          sceneState_.selectedMeshNode !=
              container::scene::SceneGraph::kInvalidNode ||
          selectedBimObjectIndex_ != std::numeric_limits<uint32_t>::max(),
      .selectAtCursor =
          [this](double cursorX, double cursorY) {
            selectMeshNodeAtCursor(cursorX, cursorY);
          },
      .hoverAtCursor =
          [this](double cursorX, double cursorY) {
            hoverMeshNodeAtCursor(cursorX, cursorY);
          },
      .clearHover = [this]() { clearHoveredMeshNode(); },
      .clearSelection = [this]() { clearSelectedMeshNode(); },
      .pickTransformGizmoAxisAtCursor =
          [this](double cursorX, double cursorY) {
            return pickTransformGizmoAxisAtCursor(cursorX, cursorY);
          },
      .transformSelectedByDrag =
          [this](container::ui::ViewportTool tool,
                 container::ui::TransformSpace space,
                 container::ui::TransformAxis axis, bool snapEnabled,
                 double deltaX, double deltaY) {
            transformSelectedNodeByDrag(tool, space, axis, snapEnabled, deltaX,
                                        deltaY);
          },
      .setStatusMessage =
          [this](std::string message) {
            if (subs_.guiManager) {
              subs_.guiManager->setStatusMessage(std::move(message));
            }
          },
      .onCullingUnfrozen =
          [this]() {
            if (subs_.gpuCullManager) {
              subs_.gpuCullManager->unfreezeCulling();
            }
          },
  });
  if (interactionController_.state().gesture !=
      container::ui::ViewportGesture::TransformDrag) {
    transformDragSession_.active = false;
  }
}

void RendererFrontend::selectMeshNodeAtCursor(double cursorX, double cursorY) {
  if (!subs_.sceneController) {
    return;
  }

  auto selectBimObject = [&](uint32_t objectIndex,
                             std::optional<glm::vec3> anchorPoint =
                                 std::nullopt) {
    sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
    selectedBimObjectIndex_ = objectIndex;
    selectionNavigationAnchor_ = {};
    if (anchorPoint && isFiniteVec3(*anchorPoint) && subs_.bimManager) {
      const BimElementBounds bounds =
          subs_.bimManager->elementBoundsForObject(objectIndex);
      selectionNavigationAnchor_ = SelectionNavigationAnchor{
          .valid = true,
          .point = *anchorPoint,
          .radius = bounds.valid ? std::max(bounds.radius, 0.25f) : 1.0f,
          .sceneNode = container::scene::SceneGraph::kInvalidNode,
          .bimObject = objectIndex,
      };
    }
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    selectedBimNativePointDrawCommands_.clear();
    selectedBimNativeCurveDrawCommands_.clear();
    if (subs_.guiManager) {
      if (subs_.bimManager) {
        if (const auto *metadata =
                subs_.bimManager->metadataForObject(objectIndex)) {
          subs_.guiManager->setStatusMessage(bimSelectionLabel(*metadata));
          return;
        }
      }
      subs_.guiManager->setStatusMessage("Selected BIM object " +
                                         std::to_string(objectIndex));
    }
  };

  auto selectSceneNode = [&](uint32_t nodeIndex,
                             std::optional<glm::vec3> anchorPoint =
                                 std::nullopt) {
    sceneState_.selectedMeshNode = nodeIndex;
    selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
    selectionNavigationAnchor_ = {};
    if (anchorPoint && isFiniteVec3(*anchorPoint)) {
      const SceneNodeWorldBounds bounds =
          subs_.sceneController->nodeWorldBounds(nodeIndex);
      selectionNavigationAnchor_ = SelectionNavigationAnchor{
          .valid = true,
          .point = *anchorPoint,
          .radius = bounds.valid ? std::max(bounds.radius, 0.25f) : 1.0f,
          .sceneNode = nodeIndex,
          .bimObject = std::numeric_limits<uint32_t>::max(),
      };
    }
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    selectedBimNativePointDrawCommands_.clear();
    selectedBimNativeCurveDrawCommands_.clear();
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage("Selected node " +
                                         std::to_string(nodeIndex));
    }
  };

  const BimDrawFilter bimFilter = currentBimDrawFilter();
  glm::vec4 activeSectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  const bool sectionPlaneEnabled =
      currentSectionPlaneEquation(subs_.guiManager.get(), activeSectionPlane);
  uint32_t gpuPickId = container::gpu::kPickIdNone;
  const bool hasGpuPick = samplePickIdAtCursor(cursorX, cursorY, gpuPickId);
  if (hasGpuPick) {
    std::optional<glm::vec3> gpuPickWorldPoint;
    float gpuPickDepth = 0.0f;
    if (samplePickDepthAtCursor(cursorX, cursorY, gpuPickDepth) ||
        sampleDepthAtCursor(cursorX, cursorY, gpuPickDepth)) {
      gpuPickWorldPoint =
          unprojectDepthAtCursor(depthVisibility_.cameraData,
                                 depthVisibility_.extent, cursorX, cursorY,
                                 gpuPickDepth);
    }

    const GpuPickTarget target = decodeGpuPickId(gpuPickId);
    if (target.kind == GpuPickTargetKind::Bim && subs_.bimManager &&
        target.objectIndex < subs_.bimManager->objectData().size() &&
        subs_.bimManager->objectMatchesFilter(target.objectIndex, bimFilter) &&
        bimObjectVisibleByLayer(target.objectIndex)) {
      selectBimObject(target.objectIndex, gpuPickWorldPoint);
      return;
    }
    if (target.kind == GpuPickTargetKind::Scene) {
      const uint32_t nodeIndex =
          subs_.sceneController->nodeIndexForObject(target.objectIndex);
      if (nodeIndex != container::scene::SceneGraph::kInvalidNode) {
        selectSceneNode(nodeIndex, gpuPickWorldPoint);
        return;
      }
    }

    clearSelectedMeshNode();
    return;
  }

  SceneNodePickHit sceneHit = subs_.sceneController->pickRenderableNodeHit(
      buffers_.cameraData, svc_.swapChainManager.extent(), cursorX, cursorY,
      sectionPlaneEnabled, activeSectionPlane);
  BimPickHit bimHit =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->pickRenderableObject(
                buffers_.cameraData, svc_.swapChainManager.extent(), cursorX,
                cursorY, sectionPlaneEnabled, activeSectionPlane)
          : BimPickHit{};

  float sampledDepth = 0.0f;
  if (sampleDepthAtCursor(cursorX, cursorY, sampledDepth)) {
    if (sceneHit.hit && !depthHitVisible(sceneHit.depth, sampledDepth)) {
      sceneHit.hit = false;
    }
    if (bimHit.hit && !depthHitVisible(bimHit.depth, sampledDepth)) {
      bimHit.hit = false;
    }
  }
  if (bimHit.hit && subs_.bimManager &&
      (!subs_.bimManager->objectMatchesFilter(bimHit.objectIndex, bimFilter) ||
       !bimObjectVisibleByLayer(bimHit.objectIndex))) {
    bimHit.hit = false;
  }

  if (!sceneHit.hit && !bimHit.hit) {
    clearSelectedMeshNode();
    return;
  }

  if (bimHit.hit && (!sceneHit.hit || bimHit.distance < sceneHit.distance)) {
    selectBimObject(bimHit.objectIndex,
                    bimHit.hasWorldPosition
                        ? std::optional<glm::vec3>{bimHit.worldPosition}
                        : std::nullopt);
    return;
  }

  selectSceneNode(sceneHit.nodeIndex,
                  sceneHit.hasWorldPosition
                      ? std::optional<glm::vec3>{sceneHit.worldPosition}
                      : std::nullopt);
}

void RendererFrontend::hoverMeshNodeAtCursor(double cursorX, double cursorY) {
  if (!subs_.sceneController) {
    clearHoveredMeshNode();
    return;
  }

  const uint64_t objectRevision = subs_.sceneController->objectDataRevision();
  const uint64_t bimObjectRevision =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->objectDataRevision()
          : 0u;
  const BimDrawFilter bimFilter = currentBimDrawFilter();
  const auto currentBimLayers =
      subs_.guiManager ? subs_.guiManager->bimLayerVisibilityState()
                       : container::ui::BimLayerVisibilityState{};
  glm::vec4 activeSectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  const bool sectionPlaneEnabled =
      currentSectionPlaneEquation(subs_.guiManager.get(), activeSectionPlane);
  if (hoverPickCache_.valid && hoverPickCache_.cursorX == cursorX &&
      hoverPickCache_.cursorY == cursorY &&
      hoverPickCache_.selectedMeshNode == sceneState_.selectedMeshNode &&
      hoverPickCache_.selectedBimObjectIndex == selectedBimObjectIndex_ &&
      hoverPickCache_.objectDataRevision == objectRevision &&
      hoverPickCache_.bimObjectDataRevision == bimObjectRevision &&
      hoverPickCache_.bimTypeFilterEnabled == bimFilter.typeFilterEnabled &&
      hoverPickCache_.bimFilterType == bimFilter.type &&
      hoverPickCache_.bimStoreyFilterEnabled ==
          bimFilter.storeyFilterEnabled &&
      hoverPickCache_.bimFilterStorey == bimFilter.storey &&
      hoverPickCache_.bimMaterialFilterEnabled ==
          bimFilter.materialFilterEnabled &&
      hoverPickCache_.bimFilterMaterial == bimFilter.material &&
      hoverPickCache_.bimDisciplineFilterEnabled ==
          bimFilter.disciplineFilterEnabled &&
      hoverPickCache_.bimFilterDiscipline == bimFilter.discipline &&
      hoverPickCache_.bimPhaseFilterEnabled == bimFilter.phaseFilterEnabled &&
      hoverPickCache_.bimFilterPhase == bimFilter.phase &&
      hoverPickCache_.bimFireRatingFilterEnabled ==
          bimFilter.fireRatingFilterEnabled &&
      hoverPickCache_.bimFilterFireRating == bimFilter.fireRating &&
      hoverPickCache_.bimLoadBearingFilterEnabled ==
          bimFilter.loadBearingFilterEnabled &&
      hoverPickCache_.bimFilterLoadBearing == bimFilter.loadBearing &&
      hoverPickCache_.bimStatusFilterEnabled ==
          bimFilter.statusFilterEnabled &&
      hoverPickCache_.bimFilterStatus == bimFilter.status &&
      hoverPickCache_.bimDrawBudgetEnabled == bimFilter.drawBudgetEnabled &&
      hoverPickCache_.bimDrawBudgetMaxObjects ==
          bimFilter.drawBudgetMaxObjects &&
      hoverPickCache_.bimIsolateSelection == bimFilter.isolateSelection &&
      hoverPickCache_.bimHideSelection == bimFilter.hideSelection &&
      hoverPickCache_.bimPointCloudVisible ==
          currentBimLayers.pointCloudVisible &&
      hoverPickCache_.bimCurvesVisible == currentBimLayers.curvesVisible &&
      hoverPickCache_.sectionPlaneEnabled == sectionPlaneEnabled &&
      (!sectionPlaneEnabled ||
       sameVec4(hoverPickCache_.sectionPlane, activeSectionPlane)) &&
      sameCameraData(hoverPickCache_.cameraData, buffers_.cameraData)) {
    return;
  }

  hoverPickCache_ = HoverPickCache{
      .valid = true,
      .cursorX = cursorX,
      .cursorY = cursorY,
      .selectedMeshNode = sceneState_.selectedMeshNode,
      .selectedBimObjectIndex = selectedBimObjectIndex_,
      .objectDataRevision = objectRevision,
      .bimObjectDataRevision = bimObjectRevision,
      .bimTypeFilterEnabled = bimFilter.typeFilterEnabled,
      .bimFilterType = bimFilter.type,
      .bimStoreyFilterEnabled = bimFilter.storeyFilterEnabled,
      .bimFilterStorey = bimFilter.storey,
      .bimMaterialFilterEnabled = bimFilter.materialFilterEnabled,
      .bimFilterMaterial = bimFilter.material,
      .bimDisciplineFilterEnabled = bimFilter.disciplineFilterEnabled,
      .bimFilterDiscipline = bimFilter.discipline,
      .bimPhaseFilterEnabled = bimFilter.phaseFilterEnabled,
      .bimFilterPhase = bimFilter.phase,
      .bimFireRatingFilterEnabled = bimFilter.fireRatingFilterEnabled,
      .bimFilterFireRating = bimFilter.fireRating,
      .bimLoadBearingFilterEnabled = bimFilter.loadBearingFilterEnabled,
      .bimFilterLoadBearing = bimFilter.loadBearing,
      .bimStatusFilterEnabled = bimFilter.statusFilterEnabled,
      .bimFilterStatus = bimFilter.status,
      .bimDrawBudgetEnabled = bimFilter.drawBudgetEnabled,
      .bimDrawBudgetMaxObjects = bimFilter.drawBudgetMaxObjects,
      .bimIsolateSelection = bimFilter.isolateSelection,
      .bimHideSelection = bimFilter.hideSelection,
      .bimPointCloudVisible = currentBimLayers.pointCloudVisible,
      .bimCurvesVisible = currentBimLayers.curvesVisible,
      .sectionPlaneEnabled = sectionPlaneEnabled,
      .sectionPlane = activeSectionPlane,
      .cameraData = buffers_.cameraData,
  };

  uint32_t hoveredNode = container::scene::SceneGraph::kInvalidNode;
  uint32_t hoveredBimObject = std::numeric_limits<uint32_t>::max();

  uint32_t gpuPickId = container::gpu::kPickIdNone;
  const bool hasGpuPick = samplePickIdAtCursor(cursorX, cursorY, gpuPickId);
  if (hasGpuPick) {
    const GpuPickTarget target = decodeGpuPickId(gpuPickId);
    if (target.kind == GpuPickTargetKind::Bim && subs_.bimManager &&
        target.objectIndex < subs_.bimManager->objectData().size() &&
        subs_.bimManager->objectMatchesFilter(target.objectIndex, bimFilter) &&
        bimObjectVisibleByLayer(target.objectIndex)) {
      hoveredBimObject = target.objectIndex;
    } else if (target.kind == GpuPickTargetKind::Scene) {
      hoveredNode =
          subs_.sceneController->nodeIndexForObject(target.objectIndex);
    }
  } else {
    const SceneNodePickHit sceneHit =
        subs_.sceneController->pickRenderableNodeHit(
            buffers_.cameraData, svc_.swapChainManager.extent(), cursorX,
            cursorY, sectionPlaneEnabled, activeSectionPlane);
    BimPickHit bimHit =
        (subs_.bimManager && subs_.bimManager->hasScene())
            ? subs_.bimManager->pickRenderableObject(
                  buffers_.cameraData, svc_.swapChainManager.extent(), cursorX,
                  cursorY, sectionPlaneEnabled, activeSectionPlane)
            : BimPickHit{};
  if (bimHit.hit && subs_.bimManager &&
      (!subs_.bimManager->objectMatchesFilter(bimHit.objectIndex, bimFilter) ||
       !bimObjectVisibleByLayer(bimHit.objectIndex))) {
    bimHit.hit = false;
  }

    if (bimHit.hit && (!sceneHit.hit || bimHit.distance < sceneHit.distance)) {
      hoveredBimObject = bimHit.objectIndex;
    } else if (sceneHit.hit) {
      hoveredNode = sceneHit.nodeIndex;
    }
  }

  if (hoveredNode == sceneState_.selectedMeshNode) {
    hoveredNode = container::scene::SceneGraph::kInvalidNode;
  }
  if (hoveredBimObject != std::numeric_limits<uint32_t>::max() &&
      selectedBimObjectIndex_ != std::numeric_limits<uint32_t>::max() &&
      subs_.bimManager) {
    const auto* selectedMetadata =
        subs_.bimManager->metadataForObject(selectedBimObjectIndex_);
    const auto* hoveredMetadata =
        subs_.bimManager->metadataForObject(hoveredBimObject);
    if (selectedMetadata != nullptr && hoveredMetadata != nullptr &&
        sameBimProductIdentity(*selectedMetadata, *hoveredMetadata)) {
      hoveredBimObject = std::numeric_limits<uint32_t>::max();
    }
  }
  if (hoveredMeshNode_ == hoveredNode &&
      hoveredBimObjectIndex_ == hoveredBimObject) {
    return;
  }

  hoveredMeshNode_ = hoveredNode;
  hoveredBimObjectIndex_ = hoveredBimObject;
  hoveredDrawCommands_.clear();
  hoveredBimDrawCommands_.clear();
  hoveredBimNativePointDrawCommands_.clear();
  hoveredBimNativeCurveDrawCommands_.clear();
}

void RendererFrontend::clearHoveredMeshNode() {
  hoverPickCache_.valid = false;
  if (hoveredMeshNode_ == container::scene::SceneGraph::kInvalidNode &&
      hoveredBimObjectIndex_ == std::numeric_limits<uint32_t>::max()) {
    return;
  }

  hoveredMeshNode_ = container::scene::SceneGraph::kInvalidNode;
  hoveredBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
  hoveredDrawCommands_.clear();
  hoveredBimDrawCommands_.clear();
  hoveredBimNativePointDrawCommands_.clear();
  hoveredBimNativeCurveDrawCommands_.clear();
}

void RendererFrontend::clearSelectedMeshNode() {
  selectionNavigationAnchor_ = {};
  if (sceneState_.selectedMeshNode ==
          container::scene::SceneGraph::kInvalidNode &&
      selectedBimObjectIndex_ == std::numeric_limits<uint32_t>::max()) {
    return;
  }

  sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
  selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
  clearHoveredMeshNode();
  selectedDrawCommands_.clear();
  selectedBimDrawCommands_.clear();
  selectedBimNativePointDrawCommands_.clear();
  selectedBimNativeCurveDrawCommands_.clear();
  if (subs_.guiManager) {
    subs_.guiManager->setStatusMessage("Selection cleared");
  }
}

void RendererFrontend::transformSelectedNodeByDrag(
    container::ui::ViewportTool tool, container::ui::TransformSpace space,
    container::ui::TransformAxis axis, bool snapEnabled, double deltaX,
    double deltaY) {
  if (!subs_.cameraController ||
      sceneState_.selectedMeshNode ==
          container::scene::SceneGraph::kInvalidNode) {
    return;
  }

  const uint32_t selectedNode = sceneState_.selectedMeshNode;
  auto currentControls = subs_.cameraController->nodeTransformControls(
      sceneState_.selectedMeshNode);
  if (!transformDragSession_.active ||
      transformDragSession_.nodeIndex != selectedNode ||
      transformDragSession_.tool != tool ||
      transformDragSession_.space != space ||
      transformDragSession_.axis != axis ||
      transformDragSession_.snapEnabled != snapEnabled) {
    const FrameTransformGizmoState gizmo = buildTransformGizmoState();
    transformDragSession_ = TransformDragSession{
        .active = true,
        .nodeIndex = selectedNode,
        .tool = tool,
        .space = space,
        .axis = axis,
        .snapEnabled = snapEnabled,
        .startControls = currentControls,
        .origin = gizmo.visible ? gizmo.origin : currentControls.position,
        .gizmoScale = std::max(gizmo.scale, 0.0001f),
        .axisX = normalizedOr(gizmo.axisX, {1.0f, 0.0f, 0.0f}),
        .axisY = normalizedOr(gizmo.axisY, {0.0f, 1.0f, 0.0f}),
        .axisZ = normalizedOr(gizmo.axisZ, {0.0f, 0.0f, 1.0f}),
    };
  }

  transformDragSession_.accumulatedDeltaX += deltaX;
  transformDragSession_.accumulatedDeltaY += deltaY;

  auto controls = transformDragSession_.startControls;
  const double dragDeltaX = transformDragSession_.accumulatedDeltaX;
  const double dragDeltaY = transformDragSession_.accumulatedDeltaY;

  auto transformAxisVector = [&]() {
    switch (axis) {
    case container::ui::TransformAxis::X:
      return transformDragSession_.axisX;
    case container::ui::TransformAxis::Y:
      return transformDragSession_.axisY;
    case container::ui::TransformAxis::Z:
      return transformDragSession_.axisZ;
    case container::ui::TransformAxis::Free:
      break;
    }
    return glm::vec3{1.0f, 0.0f, 0.0f};
  };

  auto projectedAxisDragAmount = [&](const glm::vec3 &axisVector,
                                     float fallbackScale) {
    const VkExtent2D extent = svc_.swapChainManager.extent();
    const auto startScreen = projectToFramebuffer(
        buffers_.cameraData, extent, transformDragSession_.origin);
    const auto endScreen = projectToFramebuffer(
        buffers_.cameraData, extent,
        transformDragSession_.origin +
            axisVector * transformDragSession_.gizmoScale);
    if (!startScreen || !endScreen) {
      return static_cast<float>(dragDeltaX - dragDeltaY) * fallbackScale;
    }

    const glm::vec2 screenAxis = *endScreen - *startScreen;
    const float screenAxisLength = glm::length(screenAxis);
    if (!std::isfinite(screenAxisLength) || screenAxisLength <= 0.0001f) {
      return static_cast<float>(dragDeltaX - dragDeltaY) * fallbackScale;
    }

    const glm::vec2 mouseScreenDelta{static_cast<float>(dragDeltaX),
                                     static_cast<float>(-dragDeltaY)};
    const float screenPixels = glm::dot(mouseScreenDelta,
                                        screenAxis / screenAxisLength);
    return screenPixels *
           (transformDragSession_.gizmoScale / screenAxisLength);
  };

  switch (tool) {
  case container::ui::ViewportTool::Select:
    return;
  case container::ui::ViewportTool::Translate: {
    const float scaleHint =
        std::max(0.05f, glm::length(transformDragSession_.startControls.scale) *
                            0.33333334f);
    const float dragScale = scaleHint * 0.01f;
    if (axis != container::ui::TransformAxis::Free) {
      const glm::vec3 axisVector = transformAxisVector();
      controls.position +=
          axisVector * projectedAxisDragAmount(axisVector, dragScale);
      break;
    }

    glm::vec3 horizontal{1.0f, 0.0f, 0.0f};
    glm::vec3 vertical{0.0f, 1.0f, 0.0f};

    if (auto *camera = subs_.cameraController->camera()) {
      const glm::vec3 front = camera->frontVector();
      const glm::vec3 up = camera->upVector(front);
      horizontal = camera->rightVector(front, up);
      vertical = up;
    }

    controls.position += horizontal * static_cast<float>(dragDeltaX) * dragScale;
    controls.position += vertical * static_cast<float>(dragDeltaY) * dragScale;
    break;
  }
  case container::ui::ViewportTool::Rotate: {
    const float amount = static_cast<float>(dragDeltaX - dragDeltaY) * 0.25f;
    switch (axis) {
    case container::ui::TransformAxis::X:
      controls.rotationDegrees.x += amount;
      break;
    case container::ui::TransformAxis::Y:
      controls.rotationDegrees.y += amount;
      break;
    case container::ui::TransformAxis::Z:
      controls.rotationDegrees.z += amount;
      break;
    case container::ui::TransformAxis::Free:
      controls.rotationDegrees.y += static_cast<float>(dragDeltaX) * 0.25f;
      controls.rotationDegrees.x += static_cast<float>(dragDeltaY) * 0.25f;
      break;
    }
    break;
  }
  case container::ui::ViewportTool::Scale: {
    const float factor = std::clamp(
        std::exp(static_cast<float>(dragDeltaX - dragDeltaY) * 0.005f), 0.25f,
        4.0f);
    if (axis == container::ui::TransformAxis::Free) {
      controls.scale = glm::clamp(controls.scale * factor, glm::vec3(0.001f),
                                  glm::vec3(1000.0f));
    } else {
      auto applyScaleAxis = [&](float &component) {
        component = std::clamp(component * factor, 0.001f, 1000.0f);
      };
      switch (axis) {
      case container::ui::TransformAxis::X:
        applyScaleAxis(controls.scale.x);
        break;
      case container::ui::TransformAxis::Y:
        applyScaleAxis(controls.scale.y);
        break;
      case container::ui::TransformAxis::Z:
        applyScaleAxis(controls.scale.z);
        break;
      case container::ui::TransformAxis::Free:
        break;
      }
    }
    break;
  }
  }

  if (snapEnabled) {
    switch (tool) {
    case container::ui::ViewportTool::Translate:
      controls.position = snapRelativeVec3(
          transformDragSession_.startControls.position, controls.position,
          0.25f);
      break;
    case container::ui::ViewportTool::Rotate:
      controls.rotationDegrees = snapRelativeVec3(
          transformDragSession_.startControls.rotationDegrees,
          controls.rotationDegrees, 15.0f);
      break;
    case container::ui::ViewportTool::Scale:
      controls.scale = glm::clamp(
          snapRelativeVec3(transformDragSession_.startControls.scale,
                           controls.scale, 0.1f),
          glm::vec3(0.001f), glm::vec3(1000.0f));
      break;
    case container::ui::ViewportTool::Select:
      break;
    }
  }

  subs_.cameraController->applyNodeTransform(sceneState_.selectedMeshNode,
                                             sceneState_.rootNode, controls);
  if (sceneState_.selectedMeshNode == sceneState_.rootNode &&
      subs_.lightingManager) {
    subs_.lightingManager->updateLightingData();
  }
  updateObjectBuffer();
}

std::optional<container::ui::TransformAxis>
RendererFrontend::pickTransformGizmoAxisAtCursor(double cursorX,
                                                 double cursorY) const {
  if (sceneState_.selectedMeshNode ==
      container::scene::SceneGraph::kInvalidNode) {
    return std::nullopt;
  }

  const FrameTransformGizmoState gizmo = buildTransformGizmoState();
  if (!gizmo.visible ||
      gizmo.tool == container::ui::ViewportTool::Select) {
    return std::nullopt;
  }

  const VkExtent2D extent = svc_.swapChainManager.extent();
  const glm::vec2 cursor{static_cast<float>(cursorX),
                         static_cast<float>(cursorY)};
  const auto originScreen =
      projectToFramebuffer(buffers_.cameraData, extent, gizmo.origin);
  if (!originScreen) {
    return std::nullopt;
  }

  constexpr float kCenterHitRadiusPx = 12.0f;
  constexpr float kAxisHitRadiusPx = 9.0f;
  constexpr float kEndpointHitRadiusPx = 15.0f;
  constexpr float kScaleEndpointHitRadiusPx = 18.0f;
  constexpr float kRotateRingHitRadiusPx = 8.5f;

  const glm::vec2 centerDelta = cursor - *originScreen;
  if (glm::dot(centerDelta, centerDelta) <=
      kCenterHitRadiusPx * kCenterHitRadiusPx) {
    return container::ui::TransformAxis::Free;
  }

  std::optional<container::ui::TransformAxis> bestAxis;
  float bestDistance2 = std::numeric_limits<float>::max();
  auto consider = [&](container::ui::TransformAxis axis, float distance2,
                      float thresholdPx) {
    const float threshold2 = thresholdPx * thresholdPx;
    if (distance2 <= threshold2 && distance2 < bestDistance2) {
      bestDistance2 = distance2;
      bestAxis = axis;
    }
  };

  auto axisDirection = [&](container::ui::TransformAxis axis) {
    switch (axis) {
    case container::ui::TransformAxis::X:
      return normalizedOr(gizmo.axisX, {1.0f, 0.0f, 0.0f});
    case container::ui::TransformAxis::Y:
      return normalizedOr(gizmo.axisY, {0.0f, 1.0f, 0.0f});
    case container::ui::TransformAxis::Z:
      return normalizedOr(gizmo.axisZ, {0.0f, 0.0f, 1.0f});
    case container::ui::TransformAxis::Free:
      break;
    }
    return glm::vec3{1.0f, 0.0f, 0.0f};
  };

  auto axisSideBasis = [&](container::ui::TransformAxis axis,
                           glm::vec3 &sideA, glm::vec3 &sideB) {
    if (axis == container::ui::TransformAxis::X) {
      sideA = axisDirection(container::ui::TransformAxis::Y);
      sideB = axisDirection(container::ui::TransformAxis::Z);
    } else if (axis == container::ui::TransformAxis::Y) {
      sideA = axisDirection(container::ui::TransformAxis::X);
      sideB = axisDirection(container::ui::TransformAxis::Z);
    } else {
      sideA = axisDirection(container::ui::TransformAxis::X);
      sideB = axisDirection(container::ui::TransformAxis::Y);
    }
  };

  auto considerSegment = [&](container::ui::TransformAxis axis,
                             const glm::vec3 &start,
                             const glm::vec3 &end,
                             float thresholdPx) {
    const auto startScreen =
        projectToFramebuffer(buffers_.cameraData, extent, start);
    const auto endScreen = projectToFramebuffer(buffers_.cameraData, extent, end);
    if (!startScreen || !endScreen) {
      return;
    }
    consider(axis, distanceSquaredToSegment(cursor, *startScreen, *endScreen),
             thresholdPx);
  };

  auto considerPoint = [&](container::ui::TransformAxis axis,
                           const glm::vec3 &point, float thresholdPx) {
    const auto pointScreen =
        projectToFramebuffer(buffers_.cameraData, extent, point);
    if (!pointScreen) {
      return;
    }
    const glm::vec2 delta = cursor - *pointScreen;
    consider(axis, glm::dot(delta, delta), thresholdPx);
  };

  static constexpr std::array kAxes{
      container::ui::TransformAxis::X,
      container::ui::TransformAxis::Y,
      container::ui::TransformAxis::Z,
  };

  if (gizmo.tool == container::ui::ViewportTool::Rotate) {
    constexpr uint32_t kRingSegments = 64u;
    constexpr float kPi = 3.14159265358979323846f;
    for (const container::ui::TransformAxis axis : kAxes) {
      glm::vec3 sideA{0.0f};
      glm::vec3 sideB{0.0f};
      axisSideBasis(axis, sideA, sideB);
      for (uint32_t segment = 0u; segment < kRingSegments; ++segment) {
        const float angle0 =
            static_cast<float>(segment) *
            (2.0f * kPi / static_cast<float>(kRingSegments));
        const float angle1 =
            static_cast<float>(segment + 1u) *
            (2.0f * kPi / static_cast<float>(kRingSegments));
        const glm::vec3 start =
            gizmo.origin +
            (std::cos(angle0) * sideA + std::sin(angle0) * sideB) *
                gizmo.scale;
        const glm::vec3 end =
            gizmo.origin +
            (std::cos(angle1) * sideA + std::sin(angle1) * sideB) *
                gizmo.scale;
        considerSegment(axis, start, end, kRotateRingHitRadiusPx);
      }
    }
    return bestAxis;
  }

  for (const container::ui::TransformAxis axis : kAxes) {
    const glm::vec3 direction = axisDirection(axis);
    if (gizmo.tool == container::ui::ViewportTool::Scale) {
      const glm::vec3 handleCenter =
          gizmo.origin + direction * gizmo.scale * 0.92f;
      considerSegment(axis, gizmo.origin, handleCenter, kAxisHitRadiusPx);
      considerPoint(axis, handleCenter, kScaleEndpointHitRadiusPx);
    } else {
      const glm::vec3 handleTip = gizmo.origin + direction * gizmo.scale;
      considerSegment(axis, gizmo.origin, handleTip, kAxisHitRadiusPx);
      considerPoint(axis, handleTip, kEndpointHitRadiusPx);
    }
  }

  return bestAxis;
}

void RendererFrontend::requestScreenshot(std::filesystem::path outputPath) {
  if (outputPath.empty()) {
    throw std::runtime_error("screenshot output path is empty");
  }
  if (!svc_.swapChainManager.supportsTransferSrc()) {
    throw std::runtime_error(
        "swapchain does not support VK_IMAGE_USAGE_TRANSFER_SRC_BIT");
  }
  if (!isSupportedScreenshotFormat(svc_.swapChainManager.imageFormat())) {
    throw std::runtime_error("unsupported swapchain screenshot format");
  }

  screenshot_.outputPath = std::move(outputPath);
  screenshot_.pending = true;
}

bool RendererFrontend::reloadSceneModel(const std::string &path,
                                        float importScale) {
  if (!subs_.sceneController || !subs_.sceneManager)
    return false;

  const auto cameraBuffer = buffers_.cameras.empty()
                                ? container::gpu::AllocatedBuffer{}
                                : buffers_.cameras.front();
  auto reloadPrimary = [&](const std::string &modelPath, float scale) {
    return subs_.sceneController->reloadSceneModel(
        modelPath, scale, buffers_.object, buffers_.objectCapacity,
        cameraBuffer, sceneState_.indexType, sceneState_.rootNode,
        sceneState_.selectedMeshNode, sceneState_.cubeNode);
  };
  auto refreshSceneState = [&](bool resetCamera) {
    syncSceneStateFromController();
    if (subs_.lightingManager) {
      subs_.lightingManager->setRootNode(sceneState_.rootNode);
      subs_.lightingManager->updateLightingData();
      subs_.lightingManager->createLightVolumeGeometry();
    }
    if (resetCamera) {
      if (subs_.cameraController)
        subs_.cameraController->resetCameraForScene();
      for (uint32_t imageIndex = 0;
           imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
           ++imageIndex) {
        updateCameraBuffer(imageIndex);
      }
    }
    updateObjectBuffer();
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.object);
    if (subs_.bimManager && subs_.bimManager->hasScene()) {
      subs_.sceneManager->updateAuxiliaryDescriptorSets(
          buffers_.cameras, subs_.bimManager->objectAllocatedBuffer());
    }
    syncSceneProviders();
  };

  if (isAuxiliaryRenderModelPath(path)) {
    const std::string previousPrimaryPath = activePrimaryModelPath_;
    const float previousPrimaryScale = activePrimaryImportScale_;
    const std::string previousAuxiliaryPath = activeAuxiliaryModelPath_;
    const float previousAuxiliaryScale = activeAuxiliaryImportScale_;

    (void)reloadPrimary("", 1.0f);
    if (!subs_.bimManager) {
      subs_.bimManager = std::make_unique<BimManager>(
          svc_.ctx.deviceWrapper, svc_.allocationManager,
          svc_.pipelineManager);
      subs_.bimManager->createMeshletResidencyResources(
          container::util::executableDirectory());
    }

    try {
      subs_.bimManager->loadModel(path, importScale, *subs_.sceneManager);
    } catch (const std::exception &) {
      subs_.bimManager->clear();
      (void)reloadPrimary(previousPrimaryPath, previousPrimaryScale);
      if (!previousAuxiliaryPath.empty()) {
        try {
          subs_.bimManager->loadModel(previousAuxiliaryPath,
                                      previousAuxiliaryScale,
                                      *subs_.sceneManager);
        } catch (const std::exception &) {
          subs_.bimManager->clear();
          activeAuxiliaryModelPath_.clear();
        }
      }
      refreshSceneState(true);
      if (subs_.guiManager) {
        subs_.guiManager->setStatusMessage("Failed to load model: " + path);
      }
      return false;
    }

    activePrimaryModelPath_.clear();
    activePrimaryImportScale_ = 1.0f;
    activeAuxiliaryModelPath_ = path;
    activeAuxiliaryImportScale_ = importScale;
    refreshSceneState(true);
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage("Loaded model: " + path);
    }
    return subs_.bimManager->hasScene();
  }

  const bool result = reloadPrimary(path, importScale);
  if (result) {
    activePrimaryModelPath_ = path;
    activePrimaryImportScale_ = importScale;
    activeAuxiliaryModelPath_.clear();
    activeAuxiliaryImportScale_ = 1.0f;

    if (subs_.bimManager) {
      subs_.bimManager->clear();
    }
    if (!svc_.config.bimModelPath.empty()) {
      if (!subs_.bimManager) {
        subs_.bimManager = std::make_unique<BimManager>(
            svc_.ctx.deviceWrapper, svc_.allocationManager,
            svc_.pipelineManager);
        subs_.bimManager->createMeshletResidencyResources(
            container::util::executableDirectory());
      }
      subs_.bimManager->loadModel(svc_.config.bimModelPath,
                                  svc_.config.bimImportScale,
                                  *subs_.sceneManager);
      activeAuxiliaryModelPath_ = svc_.config.bimModelPath;
      activeAuxiliaryImportScale_ = svc_.config.bimImportScale;
    }
  }

  refreshSceneState(result);
  return result;
}

void RendererFrontend::shutdown() {
  if (!svc_.ctx.deviceWrapper)
    return;

  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());

  subs_.frameSyncManager.reset();
  subs_.deferredRasterFrameGraphContext.reset();
  subs_.frameRecorder.reset();
  subs_.renderPassGpuProfiler.reset();
  subs_.activeTechnique = nullptr;
  subs_.techniqueRegistry.reset();
  subs_.pipelineRegistry.reset();
  subs_.frameRuntimeResourceRegistry.reset();
  subs_.frameResourceRegistry.reset();

  destroyGBufferResources();
  subs_.frameResourceManager.reset();

  if (subs_.guiManager) {
    subs_.guiManager->shutdown(svc_.ctx.deviceWrapper->device());
    subs_.guiManager.reset();
  }
  subs_.rendererTelemetry.reset();

  subs_.renderPassManager.reset();
  resources_.renderPasses = {};

  subs_.bimManager.reset();
  subs_.sceneController.reset();
  subs_.sceneManager.reset();
  subs_.lightingManager.reset();
  subs_.shadowCullManager.reset();
  subs_.shadowManager.reset();
  subs_.environmentManager.reset();
  subs_.bloomManager.reset();
  subs_.exposureManager.reset();
  subs_.oitManager.reset();
  subs_.cameraController.reset();
  subs_.pipelineBuilder.reset();

  for (auto &cameraBuffer : buffers_.cameras) {
    if (cameraBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(cameraBuffer);
    }
  }
  buffers_.cameras.clear();
  if (buffers_.object.buffer != VK_NULL_HANDLE)
    svc_.allocationManager.destroyBuffer(buffers_.object);
  if (screenshot_.readbackBuffer.buffer != VK_NULL_HANDLE) {
    svc_.allocationManager.destroyBuffer(screenshot_.readbackBuffer);
  }
  screenshot_.readbackSize = 0;
  if (depthVisibility_.readbackBuffer.buffer != VK_NULL_HANDLE) {
    svc_.allocationManager.destroyBuffer(depthVisibility_.readbackBuffer);
  }
  depthVisibility_.readbackSize = 0;
  depthVisibility_.valid = false;
  depthVisibility_.renderFence = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Init helpers
// ---------------------------------------------------------------------------

void RendererFrontend::createRenderPasses() {
  resources_.gBufferFormats.depthStencil =
      subs_.renderPassManager->findDepthStencilFormat();
  subs_.renderPassManager->create(
      svc_.swapChainManager.imageFormat(),
      resources_.gBufferFormats.depthStencil,
      resources_.gBufferFormats.sceneColor, resources_.gBufferFormats.albedo,
      resources_.gBufferFormats.normal, resources_.gBufferFormats.material,
      resources_.gBufferFormats.emissive, resources_.gBufferFormats.specular,
      resources_.gBufferFormats.pickId);
  resources_.renderPasses = subs_.renderPassManager->passes();
}

void RendererFrontend::createGraphicsPipelines() {
  if (!subs_.pipelineBuilder) {
    subs_.pipelineBuilder = std::make_unique<GraphicsPipelineBuilder>(
        svc_.ctx.deviceWrapper, svc_.pipelineManager);
  }
  const PipelineDescriptorLayouts descLayouts{
      subs_.sceneManager->descriptorSetLayout(),
      subs_.frameResourceManager->lightingLayout(),
      subs_.lightingManager->lightDescriptorSetLayout(),
      // Some tests or fallback configurations may not create tiled lighting
      // resources. Reuse the regular light layout so pipeline layout creation
      // stays well-formed even when the tiled path is unavailable.
      subs_.lightingManager->isTiledLightingReady()
          ? subs_.lightingManager->tiledDescriptorSetLayout()
          : subs_.lightingManager->lightDescriptorSetLayout(),
      subs_.shadowManager->descriptorSetLayout(),
      subs_.frameResourceManager->postProcessLayout(),
      subs_.frameResourceManager->oitLayout()};
  const PipelineRenderPasses rp{resources_.renderPasses.depthPrepass,
                                resources_.renderPasses.bimDepthPrepass,
                                resources_.renderPasses.gBuffer,
                                resources_.renderPasses.bimGBuffer,
                                resources_.renderPasses.transparentPick,
                                resources_.renderPasses.shadow,
                                resources_.renderPasses.lighting,
                                resources_.renderPasses.transformGizmos,
                                resources_.renderPasses.postProcess};
  resources_.builtPipelines = subs_.pipelineBuilder->build(
      container::util::executableDirectory(), descLayouts, rp);
  if (!resources_.builtPipelines.pipelines.layoutRegistry) {
    resources_.builtPipelines.pipelines.layoutRegistry =
        resources_.builtPipelines.layouts.layoutRegistry
            ? resources_.builtPipelines.layouts.layoutRegistry
            : buildGraphicsPipelineLayoutRegistry(
                  resources_.builtPipelines.layouts);
  }
  resources_.builtPipelines.layouts.layoutRegistry =
      resources_.builtPipelines.pipelines.layoutRegistry;
}

void RendererFrontend::createCamera() {
  subs_.cameraController = std::make_unique<CameraController>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.swapChainManager,
      sceneGraph_, subs_.sceneManager.get(), subs_.sceneController.get(),
      subs_.sceneController->world(), svc_.inputManager);
  subs_.cameraController->createCamera();
  applyCameraOverride(subs_.cameraController->camera(), svc_.config);
}

void RendererFrontend::syncCameraSelectionPivotOverride() {
  if (!subs_.cameraController) {
    return;
  }

  constexpr uint32_t invalidObjectIndex = std::numeric_limits<uint32_t>::max();
  const bool anchorMatchesScene =
      selectionNavigationAnchor_.valid &&
      sceneState_.selectedMeshNode != container::scene::SceneGraph::kInvalidNode &&
      selectionNavigationAnchor_.sceneNode == sceneState_.selectedMeshNode;
  const bool anchorMatchesBim =
      selectionNavigationAnchor_.valid &&
      selectedBimObjectIndex_ != invalidObjectIndex &&
      selectionNavigationAnchor_.bimObject == selectedBimObjectIndex_;
  if (anchorMatchesScene || anchorMatchesBim) {
    subs_.cameraController->setSelectionPivotOverride(
        CameraController::NavigationPivot{
            .center = selectionNavigationAnchor_.point,
            .radius = selectionNavigationAnchor_.radius,
            .valid = true});
    return;
  }

  if (selectedBimObjectIndex_ == invalidObjectIndex || !subs_.bimManager ||
      !subs_.bimManager->hasScene()) {
    subs_.cameraController->clearSelectionPivotOverride();
    return;
  }

  const BimElementBounds bounds =
      subs_.bimManager->elementBoundsForObject(selectedBimObjectIndex_);
  if (!bounds.valid) {
    subs_.cameraController->clearSelectionPivotOverride();
    return;
  }

  subs_.cameraController->setSelectionPivotOverride(
      CameraController::NavigationPivot{.center = bounds.center,
                                        .radius = bounds.radius,
                                        .valid = true});
}

void RendererFrontend::initializeScene() {
  buildSceneGraph();
  createSceneBuffers();
  subs_.lightingManager->updateLightingData();
  subs_.lightingManager->createLightVolumeGeometry();
  createFrameResources();
  if (subs_.environmentManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.environmentManager->createGtaoResources(
        container::util::executableDirectory(), ext.width, ext.height);
  }
  if (subs_.bloomManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.bloomManager->createTextures(ext.width, ext.height);
  }
  createGeometryBuffers();
  subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.object);
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    subs_.sceneManager->updateAuxiliaryDescriptorSets(
        buffers_.cameras, subs_.bimManager->objectAllocatedBuffer());
  }
  updateFrameDescriptorSets();
  syncSceneProviders();

  container::log::ContainerLogger::instance().renderer()->info(
      "Initializing Vulkan renderer");
  container::log::ContainerLogger::instance().vulkan()->debug(
      "Debugging Vulkan initialization");
}

void RendererFrontend::buildSceneGraph() {
  if (!subs_.sceneController)
    return;
  subs_.sceneController->buildSceneGraph(
      sceneState_.rootNode, sceneState_.selectedMeshNode, sceneState_.cubeNode);
  selectionNavigationAnchor_ = {};
  if (subs_.lightingManager)
    subs_.lightingManager->setRootNode(sceneState_.rootNode);
}

void RendererFrontend::ensureCameraBuffers() {
  const size_t imageCount = svc_.swapChainManager.imageCount();
  if (buffers_.cameras.size() == imageCount)
    return;

  for (auto &cameraBuffer : buffers_.cameras) {
    if (cameraBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(cameraBuffer);
    }
  }

  buffers_.cameras.assign(imageCount, {});
  for (auto &cameraBuffer : buffers_.cameras) {
    cameraBuffer = svc_.allocationManager.createBuffer(
        sizeof(container::gpu::CameraData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
}

void RendererFrontend::createSceneBuffers() {
  ensureCameraBuffers();
  if (subs_.sceneController) {
    subs_.sceneController->createSceneBuffers(
        buffers_.cameras.front(), buffers_.object, buffers_.objectCapacity);
  }
  for (uint32_t imageIndex = 0;
       imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
       ++imageIndex) {
    updateCameraBuffer(imageIndex);
  }
  updateObjectBuffer();
  updateFrameDescriptorSets();
}

void RendererFrontend::createGeometryBuffers() {
  if (!subs_.sceneController)
    return;
  subs_.sceneController->createGeometryBuffers();
  syncSceneStateFromController();
}

void RendererFrontend::createFrameResources() {
  if (subs_.lightingManager) {
    subs_.lightingManager->resizeTiledResources(svc_.swapChainManager.extent());
  }
  subs_.frameResourceManager->create(
      resources_.gBufferFormats, resources_.renderPasses.depthPrepass,
      resources_.renderPasses.bimDepthPrepass, resources_.renderPasses.gBuffer,
      resources_.renderPasses.bimGBuffer,
      resources_.renderPasses.transparentPick, resources_.renderPasses.lighting,
      resources_.renderPasses.transformGizmos, buffers_.cameras,
      buffers_.object);
}

// ---------------------------------------------------------------------------
// Per-frame helpers
// ---------------------------------------------------------------------------

void RendererFrontend::updateCameraBuffer(uint32_t imageIndex) {
  if (!subs_.cameraController || imageIndex >= buffers_.cameras.size())
    return;
  subs_.cameraController->updateCameraBuffer(buffers_.cameraData,
                                             buffers_.cameras[imageIndex]);
}

void RendererFrontend::updateObjectBuffer() {
  if (!subs_.sceneController)
    return;
  const bool recreated = subs_.sceneController->updateObjectBuffer(
      buffers_.object, buffers_.objectCapacity,
      buffers_.cameras.empty() ? container::gpu::AllocatedBuffer{}
                               : buffers_.cameras.front());
  if (recreated && subs_.sceneManager)
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.object);
  if ((recreated || !buffers_.shadowObjectDescriptorReady) &&
      subs_.shadowCullManager) {
    subs_.shadowCullManager->updateObjectSsboDescriptor(
        buffers_.object.buffer,
        sizeof(container::gpu::ObjectData) * buffers_.objectCapacity);
    buffers_.shadowObjectDescriptorReady = true;
  }
  sceneState_.diagCubeObjectIndex =
      subs_.sceneController->diagCubeObjectIndex();
}

void RendererFrontend::applyBimSemanticColorMode() {
  if (!subs_.bimManager || !subs_.bimManager->hasScene() ||
      !subs_.guiManager) {
    return;
  }
  subs_.bimManager->setSemanticColorMode(
      subs_.guiManager->bimSemanticColorMode());
}

void RendererFrontend::updateFrameDescriptorSets(uint32_t imageIndex) {
  if (subs_.frameResourceManager) {
    const auto displayMode = currentDisplayMode(subs_.guiManager.get());
    const FrameFeatureReadiness featureReadiness =
        evaluateFrameFeatureReadiness(
            displayMode, subs_.frameRecorder.get(), subs_.shadowManager.get(),
            subs_.environmentManager.get(), subs_.lightingManager.get());

    if (subs_.lightingManager) {
      auto &lightingData = subs_.lightingManager->lightingData();
      lightingData.shadowEnabled = featureReadiness.shadowAtlas ? 1u : 0u;
      lightingData.gtaoEnabled = featureReadiness.gtao ? 1u : 0u;
      lightingData.tileCullEnabled = featureReadiness.tileCull ? 1u : 0u;
      lightingData.prefilteredMipCount =
          subs_.environmentManager
              ? subs_.environmentManager->prefilteredMipCount()
              : 1u;
      if (imageIndex == UINT32_MAX) {
        subs_.lightingManager->uploadLightingData();
      } else {
        subs_.lightingManager->uploadLightingData(imageIndex);
      }
    }

    VkImageView shadowView = VK_NULL_HANDLE;
    VkSampler shadowSampler = VK_NULL_HANDLE;
    std::span<const container::gpu::AllocatedBuffer> shadowUbos{};
    if (subs_.shadowManager) {
      shadowView = subs_.shadowManager->shadowAtlasArrayView();
      shadowSampler = subs_.shadowManager->shadowSampler();
      shadowUbos = subs_.shadowManager->shadowUbos();
    }
    VkImageView irradianceView = VK_NULL_HANDLE;
    VkImageView prefilteredView = VK_NULL_HANDLE;
    VkImageView brdfLutView = VK_NULL_HANDLE;
    VkSampler envSampler = VK_NULL_HANDLE;
    VkSampler brdfLutSampler = VK_NULL_HANDLE;
    VkImageView aoTextureView = VK_NULL_HANDLE;
    VkSampler aoSampler = VK_NULL_HANDLE;
    if (subs_.environmentManager && subs_.environmentManager->isReady()) {
      irradianceView = subs_.environmentManager->irradianceView();
      prefilteredView = subs_.environmentManager->prefilteredView();
      brdfLutView = subs_.environmentManager->brdfLutView();
      envSampler = subs_.environmentManager->envSampler();
      brdfLutSampler = subs_.environmentManager->brdfLutSampler();
    }
    if (featureReadiness.gtao && subs_.environmentManager) {
      aoTextureView = subs_.environmentManager->aoTextureView();
      aoSampler = subs_.environmentManager->aoSampler();
    }
    VkImageView bloomTextureView = VK_NULL_HANDLE;
    VkSampler bloomSampler = VK_NULL_HANDLE;
    if (subs_.bloomManager && subs_.bloomManager->isReady()) {
      bloomTextureView = subs_.bloomManager->bloomResultView();
      bloomSampler = subs_.bloomManager->bloomSampler();
    }
    VkBuffer tileGridBuffer = VK_NULL_HANDLE;
    VkDeviceSize tileGridBufferSize = 0;
    if (featureReadiness.tileCull && subs_.lightingManager) {
      tileGridBuffer = subs_.lightingManager->tileGridBuffer();
      tileGridBufferSize = subs_.lightingManager->tileGridBufferSize();
    }
    VkBuffer exposureStateBuffer = VK_NULL_HANDLE;
    VkDeviceSize exposureStateBufferSize = 0;
    if (subs_.exposureManager && subs_.exposureManager->isReady()) {
      exposureStateBuffer = subs_.exposureManager->exposureStateBuffer();
      exposureStateBufferSize =
          subs_.exposureManager->exposureStateBufferSize();
    }
    subs_.frameResourceManager->updateDescriptorSets(
        buffers_.cameras, buffers_.object, shadowView, shadowSampler,
        shadowUbos, irradianceView, prefilteredView, brdfLutView, envSampler,
        brdfLutSampler, aoTextureView, aoSampler, bloomTextureView,
        bloomSampler, tileGridBuffer, tileGridBufferSize, exposureStateBuffer,
        exposureStateBufferSize);
  }
}

void RendererFrontend::destroyGBufferResources() {
  if (subs_.frameResourceManager)
    subs_.frameResourceManager->destroy();
}

bool RendererFrontend::growExactOitNodePoolIfNeeded(uint32_t imageIndex) {
  if (!subs_.frameResourceManager)
    return false;
  const bool grew = subs_.frameResourceManager->growOitPoolIfNeeded(imageIndex);
  if (grew) {
    vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());
    createFrameResources();
    updateFrameDescriptorSets();
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage(
          "Expanded exact OIT node pool to " +
          std::to_string(subs_.frameResourceManager->oitNodeCapacityFloor()));
    }
  }
  return grew;
}

void RendererFrontend::ensureScreenshotReadbackBuffer(VkExtent2D extent,
                                                      VkFormat format) {
  if (extent.width == 0 || extent.height == 0) {
    throw std::runtime_error("cannot capture a zero-sized swapchain image");
  }
  if (!isSupportedScreenshotFormat(format)) {
    throw std::runtime_error("unsupported swapchain screenshot format");
  }

  const VkDeviceSize requiredSize = static_cast<VkDeviceSize>(extent.width) *
                                    static_cast<VkDeviceSize>(extent.height) *
                                    4u;
  if (screenshot_.readbackBuffer.buffer != VK_NULL_HANDLE &&
      screenshot_.readbackSize == requiredSize &&
      screenshot_.extent.width == extent.width &&
      screenshot_.extent.height == extent.height &&
      screenshot_.format == format) {
    return;
  }

  if (screenshot_.readbackBuffer.buffer != VK_NULL_HANDLE) {
    svc_.allocationManager.destroyBuffer(screenshot_.readbackBuffer);
  }
  screenshot_.readbackBuffer = svc_.allocationManager.createBuffer(
      requiredSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  screenshot_.readbackSize = requiredSize;
  screenshot_.extent = extent;
  screenshot_.format = format;
}

void RendererFrontend::writePendingScreenshotPng() {
  if (!screenshot_.pending) {
    return;
  }
  if (screenshot_.readbackBuffer.buffer == VK_NULL_HANDLE ||
      screenshot_.readbackBuffer.allocation == nullptr ||
      screenshot_.readbackSize == 0) {
    throw std::runtime_error("screenshot readback buffer is not initialized");
  }

  void *mapped = screenshot_.readbackBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     screenshot_.readbackBuffer.allocation,
                     &mapped) != VK_SUCCESS) {
      throw std::runtime_error("failed to map screenshot readback buffer");
    }
    mappedHere = true;
  }

  if (vmaInvalidateAllocation(
          svc_.allocationManager.memoryManager()->allocator(),
          screenshot_.readbackBuffer.allocation, 0,
          screenshot_.readbackSize) != VK_SUCCESS) {
    if (mappedHere) {
      vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     screenshot_.readbackBuffer.allocation);
    }
    throw std::runtime_error("failed to invalidate screenshot readback buffer");
  }

  const auto *src = static_cast<const unsigned char *>(mapped);
  const size_t pixelCount = static_cast<size_t>(screenshot_.extent.width) *
                            static_cast<size_t>(screenshot_.extent.height);
  std::vector<unsigned char> rgba =
      convertSwapchainBytesToRgba(src, pixelCount, screenshot_.format);

  if (mappedHere) {
    vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                   screenshot_.readbackBuffer.allocation);
  }

  if (!screenshot_.outputPath.parent_path().empty()) {
    std::filesystem::create_directories(screenshot_.outputPath.parent_path());
  }
  const std::string outputPath =
      container::util::pathToUtf8(screenshot_.outputPath);
  const int ok = stbi_write_png(
      outputPath.c_str(), static_cast<int>(screenshot_.extent.width),
      static_cast<int>(screenshot_.extent.height), 4, rgba.data(),
      static_cast<int>(screenshot_.extent.width * 4));
  screenshot_.pending = false;
  if (ok == 0) {
    throw std::runtime_error("failed to write screenshot PNG: " + outputPath);
  }
}

void RendererFrontend::ensureDepthVisibilityReadbackBuffer() {
  constexpr VkDeviceSize kDepthReadbackSize = sizeof(uint32_t);
  if (depthVisibility_.readbackBuffer.buffer != VK_NULL_HANDLE &&
      depthVisibility_.readbackSize == kDepthReadbackSize) {
    return;
  }

  if (depthVisibility_.readbackBuffer.buffer != VK_NULL_HANDLE) {
    svc_.allocationManager.destroyBuffer(depthVisibility_.readbackBuffer);
  }

  depthVisibility_.readbackBuffer = svc_.allocationManager.createBuffer(
      kDepthReadbackSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  depthVisibility_.readbackSize = kDepthReadbackSize;
}

void RendererFrontend::markDepthVisibilityFrameComplete(uint32_t imageIndex) {
  depthVisibility_.valid = false;
  depthVisibility_.renderFence = VK_NULL_HANDLE;
  if (!subs_.frameResourceManager ||
      imageIndex >= subs_.frameResourceManager->frameCount()) {
    return;
  }

  const VkImage depthStencilImage = deferredRasterRuntimeImage(
      subs_.frameResourceManager.get(), imageIndex, "depth-stencil");
  if (depthStencilImage == VK_NULL_HANDLE ||
      frame_.imagesInFlight.size() <= imageIndex) {
    return;
  }

  depthVisibility_.imageIndex = imageIndex;
  depthVisibility_.extent = svc_.swapChainManager.extent();
  depthVisibility_.format = resources_.gBufferFormats.depthStencil;
  depthVisibility_.cameraData = buffers_.cameraData;
  depthVisibility_.objectDataRevision =
      subs_.sceneController ? subs_.sceneController->objectDataRevision() : 0u;
  depthVisibility_.bimObjectDataRevision =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->objectDataRevision()
          : 0u;
  depthVisibility_.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  depthVisibility_.sectionPlaneEnabled = currentSectionPlaneEquation(
      subs_.guiManager.get(), depthVisibility_.sectionPlane);
  const BimDrawFilter bimFilter = currentBimDrawFilter();
  depthVisibility_.bimTypeFilterEnabled = bimFilter.typeFilterEnabled;
  depthVisibility_.bimFilterType = bimFilter.type;
  depthVisibility_.bimStoreyFilterEnabled = bimFilter.storeyFilterEnabled;
  depthVisibility_.bimFilterStorey = bimFilter.storey;
  depthVisibility_.bimMaterialFilterEnabled = bimFilter.materialFilterEnabled;
  depthVisibility_.bimFilterMaterial = bimFilter.material;
  depthVisibility_.bimDisciplineFilterEnabled =
      bimFilter.disciplineFilterEnabled;
  depthVisibility_.bimFilterDiscipline = bimFilter.discipline;
  depthVisibility_.bimPhaseFilterEnabled = bimFilter.phaseFilterEnabled;
  depthVisibility_.bimFilterPhase = bimFilter.phase;
  depthVisibility_.bimFireRatingFilterEnabled =
      bimFilter.fireRatingFilterEnabled;
  depthVisibility_.bimFilterFireRating = bimFilter.fireRating;
  depthVisibility_.bimLoadBearingFilterEnabled =
      bimFilter.loadBearingFilterEnabled;
  depthVisibility_.bimFilterLoadBearing = bimFilter.loadBearing;
  depthVisibility_.bimStatusFilterEnabled = bimFilter.statusFilterEnabled;
  depthVisibility_.bimFilterStatus = bimFilter.status;
  depthVisibility_.bimDrawBudgetEnabled = bimFilter.drawBudgetEnabled;
  depthVisibility_.bimDrawBudgetMaxObjects = bimFilter.drawBudgetMaxObjects;
  depthVisibility_.bimIsolateSelection = bimFilter.isolateSelection;
  depthVisibility_.bimHideSelection = bimFilter.hideSelection;
  if (subs_.guiManager) {
    const auto& layers = subs_.guiManager->bimLayerVisibilityState();
    depthVisibility_.bimPointCloudVisible = layers.pointCloudVisible;
    depthVisibility_.bimCurvesVisible = layers.curvesVisible;
  } else {
    depthVisibility_.bimPointCloudVisible = true;
    depthVisibility_.bimCurvesVisible = true;
  }
  depthVisibility_.transparentPickDepthValid = false;
  if (subs_.sceneController &&
      hasTransparentCommands(
          subs_.sceneController->transparentDrawCommands(),
          subs_.sceneController->transparentSingleSidedDrawCommands(),
          subs_.sceneController->transparentWindingFlippedDrawCommands(),
          subs_.sceneController->transparentDoubleSidedDrawCommands())) {
    depthVisibility_.transparentPickDepthValid = true;
  }
  if (!depthVisibility_.transparentPickDepthValid && subs_.bimManager &&
      subs_.bimManager->hasScene()) {
    // Transparent-pick depth only records mesh and placeholder point/curve
    // surface paths. Native point/curve primitive passes render later in the
    // lighting pass, so they should not force a filteredDrawLists() lookup here.
    const bool transparentPickSurfaceGeometry =
        hasTransparentBimSurfaceGeometry(*subs_.bimManager,
                                         depthVisibility_.bimPointCloudVisible,
                                         depthVisibility_.bimCurvesVisible);
    if (bimFilter.active()) {
      const bool gpuOpaqueFiltering =
          bimFrameGpuVisibilityAvailable(
              bimFrameGpuVisibilityInputs(*subs_.bimManager, bimFilter));
      if (gpuOpaqueFiltering &&
          hasTransparentBimSurfaceGeometry(*subs_.bimManager, false, false)) {
        // GPU-compacted transparent mesh draws are filtered by the visibility
        // mask in the transparent-pick pass. Treat the clear/no-draw case as a
        // valid transparent depth surface when the active filter removes all
        // transparent objects.
        depthVisibility_.transparentPickDepthValid = true;
      }
      const bool cpuFilteredSurfaceGeometryRequired =
          transparentPickSurfaceGeometry &&
          (!gpuOpaqueFiltering ||
           (depthVisibility_.bimPointCloudVisible &&
            hasAnyGeometry(subs_.bimManager->pointDrawLists())) ||
           (depthVisibility_.bimCurvesVisible &&
            hasAnyGeometry(subs_.bimManager->curveDrawLists())));
      if (!depthVisibility_.transparentPickDepthValid &&
          cpuFilteredSurfaceGeometryRequired) {
        const BimDrawLists &filteredDraws =
            subs_.bimManager->filteredDrawLists(bimFilter);
        depthVisibility_.transparentPickDepthValid =
            hasTransparentBimSurfaceGeometry(
                filteredDraws, depthVisibility_.bimPointCloudVisible,
                depthVisibility_.bimCurvesVisible);
      }
    } else {
      depthVisibility_.transparentPickDepthValid =
          transparentPickSurfaceGeometry;
    }
  }
  depthVisibility_.selectedBimObjectIndex = bimFilter.selectedObjectIndex;
  depthVisibility_.renderFence = frame_.imagesInFlight[imageIndex];
  depthVisibility_.valid = true;
}

bool RendererFrontend::depthVisibilityFrameMatchesCurrentState() const {
  if (!depthVisibility_.valid || !subs_.sceneController) {
    return false;
  }

  const VkExtent2D extent = svc_.swapChainManager.extent();
  if (depthVisibility_.extent.width != extent.width ||
      depthVisibility_.extent.height != extent.height ||
      depthVisibility_.extent.width == 0 ||
      depthVisibility_.extent.height == 0) {
    return false;
  }
  if (!sameCameraData(depthVisibility_.cameraData, buffers_.cameraData)) {
    return false;
  }
  if (depthVisibility_.objectDataRevision !=
      subs_.sceneController->objectDataRevision()) {
    return false;
  }
  glm::vec4 currentSectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  const bool currentSectionPlaneEnabled = currentSectionPlaneEquation(
      subs_.guiManager.get(), currentSectionPlane);
  if (depthVisibility_.sectionPlaneEnabled != currentSectionPlaneEnabled) {
    return false;
  }
  if (currentSectionPlaneEnabled &&
      !sameVec4(depthVisibility_.sectionPlane, currentSectionPlane)) {
    return false;
  }

  const BimDrawFilter currentBimFilter = currentBimDrawFilter();
  if (depthVisibility_.bimTypeFilterEnabled !=
          currentBimFilter.typeFilterEnabled ||
      depthVisibility_.bimFilterType != currentBimFilter.type ||
      depthVisibility_.bimStoreyFilterEnabled !=
          currentBimFilter.storeyFilterEnabled ||
      depthVisibility_.bimFilterStorey != currentBimFilter.storey ||
      depthVisibility_.bimMaterialFilterEnabled !=
          currentBimFilter.materialFilterEnabled ||
      depthVisibility_.bimFilterMaterial != currentBimFilter.material ||
      depthVisibility_.bimDisciplineFilterEnabled !=
          currentBimFilter.disciplineFilterEnabled ||
      depthVisibility_.bimFilterDiscipline != currentBimFilter.discipline ||
      depthVisibility_.bimPhaseFilterEnabled !=
          currentBimFilter.phaseFilterEnabled ||
      depthVisibility_.bimFilterPhase != currentBimFilter.phase ||
      depthVisibility_.bimFireRatingFilterEnabled !=
          currentBimFilter.fireRatingFilterEnabled ||
      depthVisibility_.bimFilterFireRating != currentBimFilter.fireRating ||
      depthVisibility_.bimLoadBearingFilterEnabled !=
          currentBimFilter.loadBearingFilterEnabled ||
      depthVisibility_.bimFilterLoadBearing != currentBimFilter.loadBearing ||
      depthVisibility_.bimStatusFilterEnabled !=
          currentBimFilter.statusFilterEnabled ||
      depthVisibility_.bimFilterStatus != currentBimFilter.status ||
      depthVisibility_.bimDrawBudgetEnabled !=
          currentBimFilter.drawBudgetEnabled ||
      depthVisibility_.bimDrawBudgetMaxObjects !=
          currentBimFilter.drawBudgetMaxObjects ||
      depthVisibility_.bimIsolateSelection !=
          currentBimFilter.isolateSelection ||
      depthVisibility_.bimHideSelection != currentBimFilter.hideSelection ||
      depthVisibility_.selectedBimObjectIndex !=
          currentBimFilter.selectedObjectIndex) {
    return false;
  }
  if (subs_.guiManager) {
    const auto& layers = subs_.guiManager->bimLayerVisibilityState();
    if (depthVisibility_.bimPointCloudVisible != layers.pointCloudVisible ||
        depthVisibility_.bimCurvesVisible != layers.curvesVisible) {
      return false;
    }
  } else if (!depthVisibility_.bimPointCloudVisible ||
             !depthVisibility_.bimCurvesVisible) {
    return false;
  }

  const uint64_t bimRevision =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->objectDataRevision()
          : 0u;
  return depthVisibility_.bimObjectDataRevision == bimRevision;
}

BimDrawFilter RendererFrontend::currentBimDrawFilter() const {
  BimDrawFilter filter{};
  if (!subs_.guiManager) {
    return filter;
  }
  const auto &guiFilter = subs_.guiManager->bimFilterState();
  filter.typeFilterEnabled = guiFilter.typeFilterEnabled;
  filter.type = guiFilter.type;
  filter.storeyFilterEnabled = guiFilter.storeyFilterEnabled;
  filter.storey = guiFilter.storey;
  filter.materialFilterEnabled = guiFilter.materialFilterEnabled;
  filter.material = guiFilter.material;
  filter.disciplineFilterEnabled = guiFilter.disciplineFilterEnabled;
  filter.discipline = guiFilter.discipline;
  filter.phaseFilterEnabled = guiFilter.phaseFilterEnabled;
  filter.phase = guiFilter.phase;
  filter.fireRatingFilterEnabled = guiFilter.fireRatingFilterEnabled;
  filter.fireRating = guiFilter.fireRating;
  filter.loadBearingFilterEnabled = guiFilter.loadBearingFilterEnabled;
  filter.loadBearing = guiFilter.loadBearing;
  filter.statusFilterEnabled = guiFilter.statusFilterEnabled;
  filter.status = guiFilter.status;
  filter.drawBudgetEnabled = guiFilter.drawBudgetEnabled;
  filter.drawBudgetMaxObjects = guiFilter.drawBudgetMaxObjects;
  filter.isolateSelection = guiFilter.isolateSelection;
  filter.hideSelection = guiFilter.hideSelection;
  filter.selectedObjectIndex = selectedBimObjectIndex_;
  return filter;
}

bool RendererFrontend::bimObjectVisibleByLayer(uint32_t objectIndex) const {
  if (!subs_.bimManager) {
    return false;
  }
  const BimElementMetadata* metadata =
      subs_.bimManager->metadataForObject(objectIndex);
  if (metadata == nullptr) {
    return false;
  }
  if (!subs_.guiManager) {
    return true;
  }
  const auto& layerState = subs_.guiManager->bimLayerVisibilityState();
  switch (metadata->geometryKind) {
  case BimGeometryKind::Points:
    return layerState.pointCloudVisible;
  case BimGeometryKind::Curves:
    return layerState.curvesVisible;
  case BimGeometryKind::Mesh:
  default:
    return true;
  }
}

container::ui::ViewpointSnapshotState
RendererFrontend::currentViewpointSnapshot() const {
  container::ui::ViewpointSnapshotState snapshot{};
  if (subs_.cameraController) {
    snapshot.camera = subs_.cameraController->cameraTransformControls();
  }
  snapshot.selectedMeshNode = sceneState_.selectedMeshNode;
  snapshot.selectedBimObjectIndex = selectedBimObjectIndex_;
  if (subs_.guiManager) {
    snapshot.bimFilter = subs_.guiManager->bimFilterState();
  }
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    snapshot.bimModelPath = subs_.bimManager->modelPath();
    if (const auto *metadata =
            subs_.bimManager->metadataForObject(selectedBimObjectIndex_)) {
      snapshot.selectedBimObjectIndex = metadata->objectIndex;
      snapshot.selectedBimGuid = metadata->guid;
      snapshot.selectedBimType = metadata->type;
      snapshot.selectedBimSourceId = metadata->sourceId;
    }
  }
  return snapshot;
}

bool RendererFrontend::restoreViewpointSnapshot(
    const container::ui::ViewpointSnapshotState &snapshot) {
  if (!subs_.cameraController) {
    return false;
  }

  subs_.cameraController->applyCameraTransform(
      snapshot.camera, buffers_.cameraData,
      buffers_.cameras.empty() ? container::gpu::AllocatedBuffer{}
                               : buffers_.cameras.front());
  for (uint32_t imageIndex = 1;
       imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
       ++imageIndex) {
    updateCameraBuffer(imageIndex);
  }

  auto clearSelectionState = [this]() {
    sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
    selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
    selectionNavigationAnchor_ = {};
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
  };

  const uint32_t invalidObjectIndex = std::numeric_limits<uint32_t>::max();
  uint32_t restoredBimObjectIndex = invalidObjectIndex;
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    const bool sameBimScene =
        snapshot.bimModelPath.empty() ||
        snapshot.bimModelPath == subs_.bimManager->modelPath();
    auto chooseIndexedObject =
        [this, &snapshot, invalidObjectIndex](
            std::span<const uint32_t> objectIndices) -> uint32_t {
      if (objectIndices.empty()) {
        return invalidObjectIndex;
      }
      if (std::ranges::find(objectIndices, snapshot.selectedBimObjectIndex) !=
          objectIndices.end()) {
        return snapshot.selectedBimObjectIndex;
      }
      for (uint32_t objectIndex : objectIndices) {
        const BimElementMetadata *metadata =
            subs_.bimManager->metadataForObject(objectIndex);
        if (snapshot.selectedBimType.empty() ||
            (metadata != nullptr &&
             metadata->type == snapshot.selectedBimType)) {
          return objectIndex;
        }
      }
      return objectIndices.front();
    };

    if (sameBimScene && !snapshot.selectedBimGuid.empty()) {
      restoredBimObjectIndex = chooseIndexedObject(
          subs_.bimManager->objectIndicesForGuid(snapshot.selectedBimGuid));
    }

    if (sameBimScene && restoredBimObjectIndex == invalidObjectIndex &&
        !snapshot.selectedBimSourceId.empty()) {
      restoredBimObjectIndex =
          chooseIndexedObject(subs_.bimManager->objectIndicesForSourceId(
              snapshot.selectedBimSourceId));
    }

    if (sameBimScene && restoredBimObjectIndex == invalidObjectIndex &&
        snapshot.selectedBimObjectIndex <
            subs_.bimManager->objectData().size()) {
      const auto *metadata =
          subs_.bimManager->metadataForObject(snapshot.selectedBimObjectIndex);
      const bool guidCompatible =
          snapshot.selectedBimGuid.empty() ||
          (metadata != nullptr && metadata->guid == snapshot.selectedBimGuid);
      const bool sourceIdCompatible =
          snapshot.selectedBimSourceId.empty() ||
          (metadata != nullptr &&
           metadata->sourceId == snapshot.selectedBimSourceId);
      const bool typeCompatible =
          snapshot.selectedBimType.empty() ||
          (metadata != nullptr && metadata->type == snapshot.selectedBimType);
      if (guidCompatible && sourceIdCompatible && typeCompatible) {
        restoredBimObjectIndex = snapshot.selectedBimObjectIndex;
      }
    }
  }

  if (restoredBimObjectIndex != invalidObjectIndex) {
    sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
    selectedBimObjectIndex_ = restoredBimObjectIndex;
    selectionNavigationAnchor_ = {};
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    return true;
  }

  if (snapshot.selectedMeshNode != container::scene::SceneGraph::kInvalidNode &&
      sceneGraph_.getNode(snapshot.selectedMeshNode) != nullptr) {
    sceneState_.selectedMeshNode = snapshot.selectedMeshNode;
    selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
    selectionNavigationAnchor_ = {};
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    return true;
  }

  clearSelectionState();
  return true;
}

bool RendererFrontend::sampleDepthAtCursor(double cursorX, double cursorY,
                                           float &outDepth) {
  return sampleDepthAtCursor(cursorX, cursorY, outDepth, false);
}

bool RendererFrontend::samplePickDepthAtCursor(double cursorX, double cursorY,
                                               float &outDepth) {
  return sampleDepthAtCursor(cursorX, cursorY, outDepth, true);
}

bool RendererFrontend::sampleDepthAtCursor(double cursorX, double cursorY,
                                           float &outDepth, bool pickDepth) {
  if (!depthVisibilityFrameMatchesCurrentState() ||
      !subs_.frameResourceManager ||
      depthVisibility_.imageIndex >= subs_.frameResourceManager->frameCount()) {
    return false;
  }
  if (pickDepth && !depthVisibility_.transparentPickDepthValid) {
    return false;
  }

  const VkImage depthImage = deferredRasterRuntimeImage(
      subs_.frameResourceManager.get(), depthVisibility_.imageIndex,
      pickDepth ? "pick-depth" : "depth-stencil");
  if (depthImage == VK_NULL_HANDLE || cursorX < 0.0 || cursorY < 0.0 ||
      cursorX >= static_cast<double>(depthVisibility_.extent.width) ||
      cursorY >= static_cast<double>(depthVisibility_.extent.height)) {
    return false;
  }

  if (depthVisibility_.renderFence != VK_NULL_HANDLE) {
    if (vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1,
                        &depthVisibility_.renderFence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
      return false;
    }
  }

  ensureDepthVisibilityReadbackBuffer();
  if (depthVisibility_.readbackBuffer.buffer == VK_NULL_HANDLE ||
      depthVisibility_.readbackBuffer.allocation == nullptr) {
    return false;
  }

  const uint32_t x =
      std::min<uint32_t>(static_cast<uint32_t>(std::floor(cursorX)),
                         depthVisibility_.extent.width - 1u);
  const uint32_t y =
      std::min<uint32_t>(static_cast<uint32_t>(std::floor(cursorY)),
                         depthVisibility_.extent.height - 1u);

  VkCommandBufferAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocInfo.commandPool = svc_.commandBufferManager.pool();
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  if (vkAllocateCommandBuffers(svc_.ctx.deviceWrapper->device(), &allocInfo,
                               &commandBuffer) != VK_SUCCESS) {
    return false;
  }

  auto freeCommandBuffer = [&]() {
    if (commandBuffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(svc_.ctx.deviceWrapper->device(),
                           svc_.commandBufferManager.pool(), 1, &commandBuffer);
      commandBuffer = VK_NULL_HANDLE;
    }
  };

  VkCommandBufferBeginInfo beginInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_READ_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = pickDepth
                             ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                             : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = depthImage;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy copyRegion{};
  copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  copyRegion.imageSubresource.layerCount = 1;
  copyRegion.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y),
                            0};
  copyRegion.imageExtent = {1u, 1u, 1u};
  vkCmdCopyImageToBuffer(commandBuffer, depthImage,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         depthVisibility_.readbackBuffer.buffer, 1,
                         &copyRegion);

  VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.buffer = depthVisibility_.readbackBuffer.buffer;
  hostRead.offset = 0;
  hostRead.size = depthVisibility_.readbackSize;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead,
                       0, nullptr);

  VkImageMemoryBarrier toReadOnly = toTransfer;
  toReadOnly.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toReadOnly.dstAccessMask =
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  toReadOnly.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toReadOnly.newLayout = pickDepth
                             ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                             : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toReadOnly);

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  if (vkQueueSubmit(svc_.ctx.deviceWrapper->graphicsQueue(), 1, &submitInfo,
                    VK_NULL_HANDLE) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }
  vkQueueWaitIdle(svc_.ctx.deviceWrapper->graphicsQueue());
  freeCommandBuffer();

  void *mapped = depthVisibility_.readbackBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     depthVisibility_.readbackBuffer.allocation,
                     &mapped) != VK_SUCCESS) {
      return false;
    }
    mappedHere = true;
  }

  const VkResult invalidateResult = vmaInvalidateAllocation(
      svc_.allocationManager.memoryManager()->allocator(),
      depthVisibility_.readbackBuffer.allocation, 0,
      depthVisibility_.readbackSize);
  const bool decoded =
      invalidateResult == VK_SUCCESS &&
      decodeDepthReadbackValue(depthVisibility_.format, mapped, outDepth);

  if (mappedHere) {
    vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                   depthVisibility_.readbackBuffer.allocation);
  }
  return decoded;
}

bool RendererFrontend::samplePickIdAtCursor(double cursorX, double cursorY,
                                            uint32_t &outPickId) {
  if (!depthVisibilityFrameMatchesCurrentState() ||
      !subs_.frameResourceManager ||
      depthVisibility_.imageIndex >= subs_.frameResourceManager->frameCount()) {
    return false;
  }

  const VkImage pickIdImage = deferredRasterRuntimeImage(
      subs_.frameResourceManager.get(), depthVisibility_.imageIndex,
      "pick-id");
  if (pickIdImage == VK_NULL_HANDLE || cursorX < 0.0 ||
      cursorY < 0.0 ||
      cursorX >= static_cast<double>(depthVisibility_.extent.width) ||
      cursorY >= static_cast<double>(depthVisibility_.extent.height)) {
    return false;
  }

  if (depthVisibility_.renderFence != VK_NULL_HANDLE) {
    if (vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1,
                        &depthVisibility_.renderFence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
      return false;
    }
  }

  ensureDepthVisibilityReadbackBuffer();
  if (depthVisibility_.readbackBuffer.buffer == VK_NULL_HANDLE ||
      depthVisibility_.readbackBuffer.allocation == nullptr) {
    return false;
  }

  const uint32_t x =
      std::min<uint32_t>(static_cast<uint32_t>(std::floor(cursorX)),
                         depthVisibility_.extent.width - 1u);
  const uint32_t y =
      std::min<uint32_t>(static_cast<uint32_t>(std::floor(cursorY)),
                         depthVisibility_.extent.height - 1u);

  VkCommandBufferAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocInfo.commandPool = svc_.commandBufferManager.pool();
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  if (vkAllocateCommandBuffers(svc_.ctx.deviceWrapper->device(), &allocInfo,
                               &commandBuffer) != VK_SUCCESS) {
    return false;
  }

  auto freeCommandBuffer = [&]() {
    if (commandBuffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(svc_.ctx.deviceWrapper->device(),
                           svc_.commandBufferManager.pool(), 1, &commandBuffer);
      commandBuffer = VK_NULL_HANDLE;
    }
  };

  VkCommandBufferBeginInfo beginInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.srcAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = pickIdImage;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy copyRegion{};
  copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copyRegion.imageSubresource.layerCount = 1;
  copyRegion.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y),
                            0};
  copyRegion.imageExtent = {1u, 1u, 1u};
  vkCmdCopyImageToBuffer(commandBuffer, pickIdImage,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         depthVisibility_.readbackBuffer.buffer, 1,
                         &copyRegion);

  VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.buffer = depthVisibility_.readbackBuffer.buffer;
  hostRead.offset = 0;
  hostRead.size = depthVisibility_.readbackSize;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead,
                       0, nullptr);

  VkImageMemoryBarrier toReadOnly = toTransfer;
  toReadOnly.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toReadOnly.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toReadOnly.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toReadOnly.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toReadOnly);

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  if (vkQueueSubmit(svc_.ctx.deviceWrapper->graphicsQueue(), 1, &submitInfo,
                    VK_NULL_HANDLE) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }
  vkQueueWaitIdle(svc_.ctx.deviceWrapper->graphicsQueue());
  freeCommandBuffer();

  void *mapped = depthVisibility_.readbackBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     depthVisibility_.readbackBuffer.allocation,
                     &mapped) != VK_SUCCESS) {
      return false;
    }
    mappedHere = true;
  }

  const VkResult invalidateResult = vmaInvalidateAllocation(
      svc_.allocationManager.memoryManager()->allocator(),
      depthVisibility_.readbackBuffer.allocation, 0,
      depthVisibility_.readbackSize);
  if (invalidateResult == VK_SUCCESS) {
    std::memcpy(&outPickId, mapped, sizeof(outPickId));
  }

  if (mappedHere) {
    vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                   depthVisibility_.readbackBuffer.allocation);
  }
  return invalidateResult == VK_SUCCESS;
}

void RendererFrontend::presentSceneControls() {
  if (!subs_.guiManager)
    return;

  if (subs_.gpuCullManager) {
    const auto stats = subs_.gpuCullManager->cullStats();
    subs_.guiManager->setCullStats(stats.totalInputCount,
                                   stats.frustumPassedCount,
                                   stats.occlusionPassedCount);
  }
  if (subs_.lightingManager) {
    subs_.guiManager->setLightCullingStats(
        subs_.lightingManager->lightCullingStats());
    subs_.guiManager->setLightingSettings(
        subs_.lightingManager->lightingSettings());
  }
  if (subs_.rendererTelemetry) {
    subs_.guiManager->setRendererTelemetry(subs_.rendererTelemetry->view());
  }

  // Sync freeze-culling: push debug state into GUI before rendering.
  subs_.guiManager->setFreezeCulling(debugState_.freezeCulling);

  // Sync bloom: push BloomManager settings into GUI before rendering.
  if (subs_.bloomManager) {
    subs_.guiManager->setBloomSettings(
        subs_.bloomManager->enabled(), subs_.bloomManager->threshold(),
        subs_.bloomManager->knee(), subs_.bloomManager->intensity(),
        subs_.bloomManager->filterRadius());
  }

  // Sync render pass toggles: push graph pass list into GUI before rendering.
  if (subs_.frameRecorder) {
    DebugUiPresenter::publishRenderPasses(*subs_.guiManager,
                                          subs_.frameRecorder->graph());
  }

  subs_.guiManager->startFrame();
  const std::vector<container::gpu::PointLightData> emptyPointLights;
  const auto &pointLights = subs_.lightingManager
                                ? subs_.lightingManager->pointLightsSsbo()
                                : emptyPointLights;
  container::ui::BimInspectionState bimInspection{};
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    const BimSceneStats stats = subs_.bimManager->sceneStats();
    const BimOptimizedModelMetadata& optimizedMetadata =
        subs_.bimManager->optimizedModelMetadata();
    const BimDrawBudgetLodStats drawBudgetLodStats =
        subs_.bimManager->drawBudgetLodStats(currentBimDrawFilter());
    const auto &elementTypes = subs_.bimManager->elementTypes();
    const auto &elementStoreys = subs_.bimManager->elementStoreys();
    const auto &elementMaterials = subs_.bimManager->elementMaterials();
    const auto &elementDisciplines = subs_.bimManager->elementDisciplines();
    const auto &elementPhases = subs_.bimManager->elementPhases();
    const auto &elementFireRatings = subs_.bimManager->elementFireRatings();
    const auto &elementLoadBearingValues =
        subs_.bimManager->elementLoadBearingValues();
    const auto &elementStatuses = subs_.bimManager->elementStatuses();
    const auto &elementStoreyRanges =
        subs_.bimManager->elementStoreyRanges();
    bimInspection.hasScene = true;
    bimInspection.modelPath = subs_.bimManager->modelPath();
    bimInspection.objectCount = stats.objectCount;
    bimInspection.meshObjectCount = stats.meshObjectCount;
    bimInspection.pointObjectCount = stats.pointObjectCount;
    bimInspection.curveObjectCount = stats.curveObjectCount;
    bimInspection.opaqueDrawCount = stats.opaqueDrawCount;
    bimInspection.transparentDrawCount = stats.transparentDrawCount;
    bimInspection.pointOpaqueDrawCount = stats.pointOpaqueDrawCount;
    bimInspection.pointTransparentDrawCount = stats.pointTransparentDrawCount;
    bimInspection.curveOpaqueDrawCount = stats.curveOpaqueDrawCount;
    bimInspection.curveTransparentDrawCount = stats.curveTransparentDrawCount;
    bimInspection.nativePointOpaqueDrawCount =
        stats.nativePointOpaqueDrawCount;
    bimInspection.nativePointTransparentDrawCount =
        stats.nativePointTransparentDrawCount;
    bimInspection.nativeCurveOpaqueDrawCount =
        stats.nativeCurveOpaqueDrawCount;
    bimInspection.nativeCurveTransparentDrawCount =
        stats.nativeCurveTransparentDrawCount;
    bimInspection.meshletClusterCount = stats.meshletClusterCount;
    bimInspection.meshletSourceClusterCount = stats.meshletSourceClusterCount;
    bimInspection.meshletEstimatedClusterCount =
        stats.meshletEstimatedClusterCount;
    bimInspection.meshletObjectReferenceCount =
        stats.meshletObjectReferenceCount;
    bimInspection.meshletGpuResidentObjectCount =
        stats.meshletGpuResidentObjectCount;
    bimInspection.meshletGpuResidentClusterCount =
        stats.meshletGpuResidentClusterCount;
    bimInspection.meshletGpuBufferBytes = stats.meshletGpuBufferBytes;
    bimInspection.meshletGpuComputeReady = stats.meshletGpuComputeReady;
    bimInspection.meshletGpuDispatchPending =
        stats.meshletGpuDispatchPending;
    bimInspection.meshletMaxLodLevel = stats.meshletMaxLodLevel;
    bimInspection.optimizedModelMetadataCacheable =
        stats.optimizedModelMetadataCacheable;
    bimInspection.optimizedModelMetadataCacheHit =
        stats.optimizedModelMetadataCacheHit;
    bimInspection.optimizedModelMetadataCacheStale =
        stats.optimizedModelMetadataCacheStale;
    bimInspection.optimizedModelMetadataCacheWritten =
        stats.optimizedModelMetadataCacheWriteSucceeded;
    bimInspection.optimizedModelMetadataCacheKey =
        optimizedMetadata.cacheKey;
    bimInspection.optimizedModelMetadataCachePath =
        optimizedMetadata.cachePath;
    bimInspection.optimizedModelMetadataCacheStatus =
        optimizedMetadata.cacheStatus;
    bimInspection.drawBudgetVisibleObjectCount =
        drawBudgetLodStats.visibleObjectCount;
    bimInspection.drawBudgetVisibleMeshObjectCount =
        drawBudgetLodStats.visibleMeshObjectCount;
    bimInspection.drawBudgetVisibleMeshletClusterCount =
        drawBudgetLodStats.visibleMeshletClusterReferences;
    bimInspection.drawBudgetVisibleMaxLodLevel =
        drawBudgetLodStats.visibleMaxLodLevel;
    bimInspection.floorPlanDrawCount = stats.floorPlanDrawCount;
    bimInspection.uniqueTypeCount = stats.uniqueTypeCount;
    bimInspection.uniqueStoreyCount = stats.uniqueStoreyCount;
    bimInspection.uniqueMaterialCount = stats.uniqueMaterialCount;
    bimInspection.uniqueDisciplineCount = stats.uniqueDisciplineCount;
    bimInspection.uniquePhaseCount = stats.uniquePhaseCount;
    bimInspection.uniqueFireRatingCount = stats.uniqueFireRatingCount;
    bimInspection.uniqueLoadBearingCount = stats.uniqueLoadBearingCount;
    bimInspection.uniqueStatusCount = stats.uniqueStatusCount;
    const BimModelUnitMetadata &unitMetadata =
        subs_.bimManager->modelUnitMetadata();
    bimInspection.hasSourceUnits = unitMetadata.hasSourceUnits;
    bimInspection.sourceUnits = unitMetadata.sourceUnits;
    bimInspection.hasMetersPerUnit = unitMetadata.hasMetersPerUnit;
    bimInspection.metersPerUnit = unitMetadata.metersPerUnit;
    bimInspection.hasImportScale = unitMetadata.hasImportScale;
    bimInspection.importScale = unitMetadata.importScale;
    bimInspection.hasEffectiveImportScale =
        unitMetadata.hasEffectiveImportScale;
    bimInspection.effectiveImportScale = unitMetadata.effectiveImportScale;
    const BimModelGeoreferenceMetadata& georeferenceMetadata =
        subs_.bimManager->modelGeoreferenceMetadata();
    bimInspection.hasSourceUpAxis = georeferenceMetadata.hasSourceUpAxis;
    bimInspection.sourceUpAxis = georeferenceMetadata.sourceUpAxis;
    bimInspection.hasCoordinateOffset =
        georeferenceMetadata.hasCoordinateOffset;
    bimInspection.coordinateOffset = georeferenceMetadata.coordinateOffset;
    bimInspection.coordinateOffsetSource =
        georeferenceMetadata.coordinateOffsetSource;
    bimInspection.crsName = georeferenceMetadata.crsName;
    bimInspection.crsAuthority = georeferenceMetadata.crsAuthority;
    bimInspection.crsCode = georeferenceMetadata.crsCode;
    bimInspection.mapConversionName = georeferenceMetadata.mapConversionName;
    bimInspection.elementTypes =
        std::span<const std::string>(elementTypes.data(), elementTypes.size());
    bimInspection.elementStoreys =
        std::span<const std::string>(elementStoreys.data(),
                                     elementStoreys.size());
    bimInspection.elementMaterials =
        std::span<const std::string>(elementMaterials.data(),
                                     elementMaterials.size());
    bimInspection.elementDisciplines =
        std::span<const std::string>(elementDisciplines.data(),
                                     elementDisciplines.size());
    bimInspection.elementPhases =
        std::span<const std::string>(elementPhases.data(),
                                     elementPhases.size());
    bimInspection.elementFireRatings =
        std::span<const std::string>(elementFireRatings.data(),
                                     elementFireRatings.size());
    bimInspection.elementLoadBearingValues =
        std::span<const std::string>(elementLoadBearingValues.data(),
                                     elementLoadBearingValues.size());
    bimInspection.elementStatuses =
        std::span<const std::string>(elementStatuses.data(),
                                     elementStatuses.size());
    bimInspection.elementStoreyRanges =
        std::span<const BimStoreyRange>(elementStoreyRanges.data(),
                                        elementStoreyRanges.size());
    if (const auto *metadata =
            subs_.bimManager->metadataForObject(selectedBimObjectIndex_)) {
      bimInspection.hasSelection = true;
      bimInspection.selectedObjectIndex = metadata->objectIndex;
      bimInspection.sourceElementIndex = metadata->sourceElementIndex;
      bimInspection.meshId = metadata->meshId;
      bimInspection.sourceMaterialIndex = metadata->sourceMaterialIndex;
      bimInspection.materialIndex = metadata->materialIndex;
      bimInspection.semanticTypeId = metadata->semanticTypeId;
      bimInspection.sourceColor = metadata->sourceColor;
      bimInspection.guid = metadata->guid;
      bimInspection.type = metadata->type;
      bimInspection.displayName = metadata->displayName;
      bimInspection.objectType = metadata->objectType;
      bimInspection.storeyName = metadata->storeyName;
      bimInspection.storeyId = metadata->storeyId;
      bimInspection.materialName = metadata->materialName;
      bimInspection.materialCategory = metadata->materialCategory;
      bimInspection.discipline = metadata->discipline;
      bimInspection.phase = metadata->phase;
      bimInspection.fireRating = metadata->fireRating;
      bimInspection.loadBearing = metadata->loadBearing;
      bimInspection.status = metadata->status;
      bimInspection.sourceId = metadata->sourceId;
      bimInspection.geometryKind = bimGeometryKindLabel(metadata->geometryKind);
      bimInspection.properties =
          std::span<const BimElementProperty>(metadata->properties.data(),
                                              metadata->properties.size());
      bimInspection.transparent = metadata->transparent;
      bimInspection.doubleSided = metadata->doubleSided;
      const BimElementBounds elementBounds =
          subs_.bimManager->elementBoundsForObject(selectedBimObjectIndex_);
      if (elementBounds.valid) {
        bimInspection.hasSelectionBounds = true;
        bimInspection.selectionBoundsMin = elementBounds.min;
        bimInspection.selectionBoundsMax = elementBounds.max;
        bimInspection.selectionBoundsCenter = elementBounds.center;
        bimInspection.selectionBoundsSize = elementBounds.size;
        bimInspection.selectionBoundsRadius = elementBounds.radius;
        bimInspection.selectionFloorElevation = elementBounds.floorElevation;
      }
    }
  }
  const container::ui::ViewpointSnapshotState currentViewpoint =
      currentViewpointSnapshot();
  const auto uploadCameraBuffers = [this]() {
    for (uint32_t imageIndex = 0;
         imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
         ++imageIndex) {
      updateCameraBuffer(imageIndex);
    }
  };
  subs_.guiManager->drawSceneControls(
      sceneGraph_,
      [this](const std::string &modelPath, float importScale) {
        return reloadSceneModel(modelPath, importScale);
      },
      [this](float importScale) {
        return reloadSceneModel(container::app::DefaultAppConfig().modelPath,
                                importScale);
      },
      subs_.cameraController ? subs_.cameraController->cameraTransformControls()
                             : container::ui::TransformControls{},
      [this, uploadCameraBuffers](
          const container::ui::TransformControls &controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyCameraTransform(
              controls, buffers_.cameraData,
              buffers_.cameras.empty() ? container::gpu::AllocatedBuffer{}
                                       : buffers_.cameras.front());
        uploadCameraBuffers();
      },
      subs_.cameraController
          ? subs_.cameraController->nodeTransformControls(sceneState_.rootNode)
          : container::ui::TransformControls{},
      [this](const container::ui::TransformControls &controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyNodeTransform(
              sceneState_.rootNode, sceneState_.rootNode, controls);
        if (subs_.lightingManager)
          subs_.lightingManager->updateLightingData();
        updateObjectBuffer();
      },
      subs_.lightingManager ? subs_.lightingManager->directionalLightPosition()
                            : glm::vec3{0.0f},
      subs_.lightingManager ? subs_.lightingManager->lightingData()
                            : container::gpu::LightingData{},
      pointLights, sceneState_.selectedMeshNode, bimInspection,
      currentViewpoint,
      [this](const container::ui::ViewpointSnapshotState &snapshot) {
        return restoreViewpointSnapshot(snapshot);
      },
      [this](uint32_t nodeIndex) {
        if (nodeIndex == container::scene::SceneGraph::kInvalidNode) {
          clearSelectedMeshNode();
          return;
        }
        if (subs_.cameraController) {
          subs_.cameraController->selectMeshNode(nodeIndex,
                                                 sceneState_.selectedMeshNode);
          selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
          selectionNavigationAnchor_ = {};
          clearHoveredMeshNode();
          selectedDrawCommands_.clear();
          selectedBimDrawCommands_.clear();
          if (subs_.guiManager) {
            subs_.guiManager->setStatusMessage(
                "Selected node " +
                std::to_string(sceneState_.selectedMeshNode));
          }
        }
      },
      subs_.cameraController ? subs_.cameraController->nodeTransformControls(
                                   sceneState_.selectedMeshNode)
                             : container::ui::TransformControls{},
      [this](uint32_t nodeIndex,
             const container::ui::TransformControls &controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyNodeTransform(
              nodeIndex, sceneState_.rootNode, controls);
        if (nodeIndex == sceneState_.rootNode && subs_.lightingManager)
          subs_.lightingManager->updateLightingData();
        updateObjectBuffer();
      });

  subs_.guiManager->drawViewportInteractionControls(
      interactionController_.state(),
      [this](container::ui::ViewportTool tool) {
        interactionController_.setTool(tool);
      },
      [this](container::ui::TransformSpace transformSpace) {
        interactionController_.setTransformSpace(transformSpace);
      },
      [this](container::ui::TransformAxis transformAxis) {
        interactionController_.setTransformAxis(transformAxis);
      },
      [this](container::ui::ViewportNavigationStyle navigationStyle) {
        interactionController_.setNavigationStyle(navigationStyle);
      },
      [this](bool snapEnabled) {
        interactionController_.setTransformSnapEnabled(snapEnabled);
      });

  syncCameraSelectionPivotOverride();
  container::ui::ViewportNavigationState navigationState{};
  if (subs_.cameraController) {
    navigationState.projectionMode =
        subs_.cameraController->isOrthographic()
            ? container::ui::CameraProjectionMode::Orthographic
            : container::ui::CameraProjectionMode::Perspective;
    if (const auto *camera = subs_.cameraController->camera()) {
      navigationState.cameraForward = camera->frontVector();
      navigationState.cameraUp =
          camera->upVector(navigationState.cameraForward);
      navigationState.cameraRight = camera->rightVector(
          navigationState.cameraForward, navigationState.cameraUp);
    }
  }
  subs_.guiManager->drawViewportNavigationOverlay(
      navigationState,
      [this, uploadCameraBuffers](container::ui::CameraViewPreset preset) {
        if (!subs_.cameraController) {
          return;
        }
        subs_.cameraController->setViewPreset(sceneState_.selectedMeshNode,
                                              preset);
        uploadCameraBuffers();
      },
      [this, uploadCameraBuffers](float deltaX, float deltaY) {
        if (!subs_.cameraController) {
          return;
        }
        subs_.cameraController->orbit(sceneState_.selectedMeshNode, deltaX,
                                      deltaY, 1.0f);
        uploadCameraBuffers();
      },
      [this, uploadCameraBuffers](float deltaX, float deltaY) {
        if (!subs_.cameraController) {
          return;
        }
        subs_.cameraController->pan(sceneState_.selectedMeshNode, deltaX,
                                    -deltaY, 2.0f);
        uploadCameraBuffers();
      },
      [this, uploadCameraBuffers]() {
        if (!subs_.cameraController) {
          return;
        }
        subs_.cameraController->toggleProjectionMode(
            sceneState_.selectedMeshNode);
        uploadCameraBuffers();
      });

  // Sync freeze-culling: pull GUI state back into debug state (checkbox may
  // have toggled it).
  const bool guiFreeze = subs_.guiManager->freezeCullingRequested();
  if (guiFreeze != debugState_.freezeCulling) {
    debugState_.freezeCulling = guiFreeze;
    if (!guiFreeze && subs_.gpuCullManager)
      subs_.gpuCullManager->unfreezeCulling();
  }

  // Sync bloom: pull GUI state back into BloomManager.
  if (subs_.bloomManager) {
    subs_.bloomManager->enabled() = subs_.guiManager->bloomEnabled();
    subs_.bloomManager->threshold() = subs_.guiManager->bloomThreshold();
    subs_.bloomManager->knee() = subs_.guiManager->bloomKnee();
    subs_.bloomManager->intensity() = subs_.guiManager->bloomIntensity();
    subs_.bloomManager->filterRadius() = subs_.guiManager->bloomRadius();
  }

  if (subs_.lightingManager) {
    const auto &guiLightingSettings = subs_.guiManager->lightingSettings();
    const auto &currentLightingSettings =
        subs_.lightingManager->lightingSettings();
    const bool lightingSettingsChanged =
        guiLightingSettings.preset != currentLightingSettings.preset ||
        guiLightingSettings.density != currentLightingSettings.density ||
        guiLightingSettings.radiusScale !=
            currentLightingSettings.radiusScale ||
        guiLightingSettings.intensityScale !=
            currentLightingSettings.intensityScale ||
        guiLightingSettings.directionalIntensity !=
            currentLightingSettings.directionalIntensity ||
        guiLightingSettings.environmentIntensity !=
            currentLightingSettings.environmentIntensity;
    if (lightingSettingsChanged) {
      subs_.lightingManager->setLightingSettings(guiLightingSettings);
      subs_.lightingManager->updateLightingData();
      updateFrameDescriptorSets();
    }
  }

  if (subs_.guiManager && subs_.environmentManager) {
    subs_.guiManager->setEnvironmentStatus(
        subs_.environmentManager->environmentStatus());
  }

  // Sync render pass toggles: pull GUI toggle states back into the render
  // graph.
  if (subs_.frameRecorder) {
    auto &graph = subs_.frameRecorder->graph();
    if (DebugUiPresenter::applyRenderPassToggles(*subs_.guiManager, graph)) {
      subs_.guiManager->setStatusMessage(
          "Protected passes stay enabled; dependent optional passes are "
          "disabled automatically.");
    }
  }
}

void RendererFrontend::recordCommandBuffer(VkCommandBuffer commandBuffer,
                                           uint32_t imageIndex) {
  if (!subs_.frameRecorder || !subs_.frameResourceManager) {
    throw std::runtime_error(
        "frameRecorder not initialized or image index out of range");
  }
  if (imageIndex >= subs_.frameResourceManager->frameCount()) {
    throw std::runtime_error(
        "frameRecorder not initialized or image index out of range");
  }
  auto p = buildFrameRecordParams(imageIndex);
  if (subs_.deferredRasterFrameGraphContext) {
    p.lifecycle = subs_.deferredRasterFrameGraphContext->lifecycleHooks();
  }
  subs_.frameRecorder->record(commandBuffer, p);
}

FrameTransformGizmoState RendererFrontend::buildTransformGizmoState() const {
  FrameTransformGizmoState gizmo{};
  const auto &interaction = interactionController_.state();
  gizmo.tool = interaction.tool;
  gizmo.transformSpace = interaction.transformSpace;
  gizmo.activeAxis =
      interaction.transformAxis != container::ui::TransformAxis::Free ||
              interaction.gesture == container::ui::ViewportGesture::TransformDrag
          ? interaction.transformAxis
          : interaction.hoverTransformAxis;

  if (interaction.tool == container::ui::ViewportTool::Select) {
    return gizmo;
  }

  glm::vec3 boundsMin{0.0f};
  glm::vec3 boundsMax{0.0f};
  bool hasBounds = false;
  bool useLocalAxes = false;
  const container::scene::SceneNode *selectedNode = nullptr;

  if (sceneState_.selectedMeshNode !=
          container::scene::SceneGraph::kInvalidNode &&
      subs_.sceneController) {
    const auto &objectData = subs_.sceneController->objectData();
    hasBounds = accumulateDrawCommandBounds(selectedDrawCommands_, objectData,
                                            boundsMin, boundsMax);
    selectedNode = sceneGraph_.getNode(sceneState_.selectedMeshNode);
    if (!hasBounds && selectedNode != nullptr) {
      const glm::vec3 origin{selectedNode->worldTransform[3]};
      boundsMin = origin - glm::vec3{0.5f};
      boundsMax = origin + glm::vec3{0.5f};
      hasBounds = true;
    }
    useLocalAxes =
        selectedNode != nullptr &&
        interaction.transformSpace == container::ui::TransformSpace::Local;
  } else if (selectedBimObjectIndex_ != std::numeric_limits<uint32_t>::max() &&
             subs_.bimManager && subs_.bimManager->hasScene()) {
    const auto &objectData = subs_.bimManager->objectData();
    std::vector<DrawCommand> selectedProductDraws;
    const uint32_t selectedObjectIndex = selectedBimObjectIndex_;
    subs_.bimManager->collectDrawCommandsForObject(selectedObjectIndex,
                                                   selectedProductDraws);
    if (!selectedProductDraws.empty()) {
      hasBounds = accumulateDrawCommandBounds(selectedProductDraws, objectData,
                                              boundsMin, boundsMax);
    } else if (selectedBimObjectIndex_ < objectData.size()) {
      includeBoundingSphere(objectData[selectedBimObjectIndex_].boundingSphere,
                            boundsMin, boundsMax, hasBounds);
    }
    // BIM meshes are selectable overlays today; transform drag still operates
    // on scene graph nodes, so world-space axes avoid implying hidden local
    // transform state.
    gizmo.transformSpace = container::ui::TransformSpace::World;
  }

  if (!hasBounds) {
    return gizmo;
  }

  gizmo.origin = (boundsMin + boundsMax) * 0.5f;
  gizmo.radius = std::max(glm::length(boundsMax - boundsMin) * 0.5f, 0.1f);
  gizmo.scale =
      transformGizmoScale(gizmo.origin, gizmo.radius, buffers_.cameraData);
  if (useLocalAxes && selectedNode != nullptr) {
    gizmo.axisX = normalizedOr(glm::vec3{selectedNode->worldTransform[0]},
                               {1.0f, 0.0f, 0.0f});
    gizmo.axisY = normalizedOr(glm::vec3{selectedNode->worldTransform[1]},
                               {0.0f, 1.0f, 0.0f});
    gizmo.axisZ = normalizedOr(glm::vec3{selectedNode->worldTransform[2]},
                               {0.0f, 0.0f, 1.0f});
  }
  gizmo.visible = true;
  return gizmo;
}

void RendererFrontend::publishFrameRuntimeResourceBindings(
    uint32_t imageIndex) {
  FrameResourceRegistry* runtime = subs_.frameRuntimeResourceRegistry.get();
  if (runtime == nullptr) {
    return;
  }

  runtime->clearBindings();

  auto copyBinding = [runtime](const FrameResourceBinding& binding) {
    switch (binding.kind) {
    case FrameResourceKind::Image:
      runtime->bindImage(binding.key.technique, binding.key.name,
                         binding.frameIndex, binding.image);
      break;
    case FrameResourceKind::Buffer:
      runtime->bindBuffer(binding.key.technique, binding.key.name,
                          binding.frameIndex, binding.buffer);
      break;
    case FrameResourceKind::Framebuffer:
      runtime->bindFramebuffer(binding.key.technique, binding.key.name,
                               binding.frameIndex, binding.framebuffer);
      break;
    case FrameResourceKind::DescriptorSet:
      runtime->bindDescriptorSet(binding.key.technique, binding.key.name,
                                 binding.frameIndex, binding.descriptor);
      break;
    case FrameResourceKind::Sampler:
      runtime->bindSampler(binding.key.technique, binding.key.name,
                           binding.frameIndex, binding.sampler);
      break;
    case FrameResourceKind::External:
      break;
    }
  };

  if (subs_.frameResourceManager != nullptr) {
    for (const FrameResourceBinding* binding :
         subs_.frameResourceManager->resourceRegistry().bindingsForFrame(
             RenderTechniqueId::DeferredRaster, imageIndex)) {
      if (binding != nullptr) {
        copyBinding(*binding);
      }
    }
  }

  auto bindDescriptorSet = [runtime, imageIndex](std::string name,
                                                 VkDescriptorSet set) {
    if (set == VK_NULL_HANDLE) {
      return;
    }
    runtime->bindDescriptorSet(RenderTechniqueId::DeferredRaster,
                               std::move(name), imageIndex,
                               FrameDescriptorBinding{.descriptorSet = set});
  };

  bindDescriptorSet("scene-descriptor-set",
                    subs_.sceneManager
                        ? subs_.sceneManager->descriptorSet(imageIndex)
                        : VK_NULL_HANDLE);
  bindDescriptorSet(
      "bim-scene-descriptor-set",
      (subs_.bimManager && subs_.bimManager->hasScene() && subs_.sceneManager)
          ? subs_.sceneManager->auxiliaryDescriptorSet(imageIndex)
          : VK_NULL_HANDLE);
  bindDescriptorSet("light-descriptor-set",
                    subs_.lightingManager
                        ? subs_.lightingManager->lightDescriptorSet(imageIndex)
                        : VK_NULL_HANDLE);
  bindDescriptorSet(
      "tiled-lighting-descriptor-set",
      (subs_.lightingManager && subs_.lightingManager->isTiledLightingReady())
          ? subs_.lightingManager->tiledDescriptorSet()
          : VK_NULL_HANDLE);
  bindDescriptorSet("shadow-descriptor-set",
                    subs_.shadowManager
                        ? subs_.shadowManager->descriptorSet(imageIndex)
                        : VK_NULL_HANDLE);

  if (subs_.frameResourceManager != nullptr &&
      subs_.frameResourceManager->gBufferSampler() != VK_NULL_HANDLE) {
    runtime->bindSampler(
        RenderTechniqueId::DeferredRaster, "g-buffer-sampler", imageIndex,
        FrameSamplerBinding{
            .sampler = subs_.frameResourceManager->gBufferSampler()});
  }

  const VkBuffer cameraBuffer =
      imageIndex < buffers_.cameras.size() ? buffers_.cameras[imageIndex].buffer
                                           : VK_NULL_HANDLE;
  if (cameraBuffer != VK_NULL_HANDLE) {
    runtime->bindBuffer(
        RenderTechniqueId::DeferredRaster, "camera-buffer", imageIndex,
        FrameBufferBinding{.buffer = cameraBuffer,
                           .size = sizeof(container::gpu::CameraData),
                           .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT});
  }
  if (buffers_.object.buffer != VK_NULL_HANDLE && buffers_.objectCapacity > 0) {
    runtime->bindBuffer(
        RenderTechniqueId::DeferredRaster, "scene-object-buffer", imageIndex,
        FrameBufferBinding{
            .buffer = buffers_.object.buffer,
            .size = buffers_.objectCapacity * sizeof(container::gpu::ObjectData),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
  }
}

FrameRecordParams
RendererFrontend::buildFrameRecordParams(uint32_t imageIndex) {
  publishFrameRuntimeResourceBindings(imageIndex);
  syncSceneProviders();

  FrameRecordParams p{};
  p.runtime.imageIndex = imageIndex;
  p.registries.resourceContracts = subs_.frameResourceRegistry.get();
  p.registries.pipelineRecipes = subs_.pipelineRegistry.get();
  p.registries.resourceBindings = subs_.frameRuntimeResourceRegistry.get();
  p.scene.vertexSlice = sceneState_.vertexSlice;
  p.scene.indexSlice = sceneState_.indexSlice;
  p.scene.indexType = sceneState_.indexType;
  p.draws.opaqueDrawCommands = &subs_.sceneController->opaqueDrawCommands();
  p.draws.transparentDrawCommands =
      &subs_.sceneController->transparentDrawCommands();
  p.draws.opaqueSingleSidedDrawCommands =
      &subs_.sceneController->opaqueSingleSidedDrawCommands();
  p.draws.opaqueWindingFlippedDrawCommands =
      &subs_.sceneController->opaqueWindingFlippedDrawCommands();
  p.draws.opaqueDoubleSidedDrawCommands =
      &subs_.sceneController->opaqueDoubleSidedDrawCommands();
  p.draws.transparentSingleSidedDrawCommands =
      &subs_.sceneController->transparentSingleSidedDrawCommands();
  p.draws.transparentWindingFlippedDrawCommands =
      &subs_.sceneController->transparentWindingFlippedDrawCommands();
  p.draws.transparentDoubleSidedDrawCommands =
      &subs_.sceneController->transparentDoubleSidedDrawCommands();
  subs_.sceneController->collectDrawCommandsForNode(hoveredMeshNode_,
                                                    hoveredDrawCommands_);
  p.draws.hoveredDrawCommands = &hoveredDrawCommands_;
  subs_.sceneController->collectDrawCommandsForNode(
      sceneState_.selectedMeshNode, selectedDrawCommands_);
  p.draws.selectedDrawCommands = &selectedDrawCommands_;
  p.scene.objectData = &subs_.sceneController->objectData();
  p.scene.objectDataRevision = subs_.sceneController->objectDataRevision();
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    const BimDrawFilter bimFilter = currentBimDrawFilter();
    BimMeshletResidencySettings residencySettings{};
    residencySettings.drawBudgetEnabled = bimFilter.drawBudgetEnabled;
    residencySettings.drawBudgetMaxObjects = bimFilter.drawBudgetMaxObjects;
    residencySettings.selectedObjectIndex = selectedBimObjectIndex_;
    residencySettings.viewportHeightPixels =
        static_cast<float>(std::max(svc_.swapChainManager.extent().height, 1u));
    if (subs_.guiManager != nullptr) {
      const auto& lodUi = subs_.guiManager->bimLodStreamingUiState();
      residencySettings.autoLod = lodUi.autoLod;
      residencySettings.pauseStreaming = lodUi.pauseStreamingRequest;
      residencySettings.keepSelectedResident = true;
      residencySettings.lodBias = lodUi.lodBias;
      residencySettings.screenErrorPixels = lodUi.screenErrorPixels;
      residencySettings.forceResident = false;
    }
    subs_.bimManager->updateMeshletResidencySettings(residencySettings);
    subs_.bimManager->updateVisibilityFilterSettings(bimFilter);
    const auto bimLayers =
        subs_.guiManager ? subs_.guiManager->bimLayerVisibilityState()
                         : container::ui::BimLayerVisibilityState{};
    BimFrameDrawRoutingInputs bimRoutingInputs =
        bimFrameDrawRoutingInputs(*subs_.bimManager, bimFilter, bimLayers,
                                  nullptr);
    BimFrameDrawRoutingPlan bimRouting =
        buildBimFrameDrawRoutingPlan(bimRoutingInputs);
    const BimDrawLists *filteredDraws = nullptr;
    if (bimRouting.cpuFilteredDrawsRequired) {
      filteredDraws = &subs_.bimManager->filteredDrawLists(bimFilter);
      bimRoutingInputs.cpuFilteredDraws = filteredDraws;
      bimRouting = buildBimFrameDrawRoutingPlan(bimRoutingInputs);
    }
    p.bim.scene.vertexSlice = subs_.bimManager->vertexSlice();
    p.bim.scene.indexSlice = subs_.bimManager->indexSlice();
    p.bim.scene.indexType = subs_.bimManager->indexType();
    p.bim.scene.objectData = &subs_.bimManager->objectData();
    p.bim.scene.objectDataRevision = subs_.bimManager->objectDataRevision();
    p.bim.scene.objectBuffer = subs_.bimManager->objectBuffer();
    p.bim.scene.objectBufferSize = subs_.bimManager->objectBufferSize();
    p.bim.semanticColorMode =
        static_cast<uint32_t>(subs_.bimManager->semanticColorMode());
    p.bim.opaqueMeshDrawsUseGpuVisibility =
        bimRouting.meshDrawsUseGpuVisibility;
    p.bim.transparentMeshDrawsUseGpuVisibility =
        bimRouting.transparentMeshDrawsUseGpuVisibility;
    p.bim.nativePrimitiveDrawsUseGpuVisibility =
        bimRouting.nativePrimitiveDrawsUseGpuVisibility;
    p.bim.nativePointDrawsUseGpuVisibility =
        bimRouting.nativePointDrawsUseGpuVisibility;
    p.bim.nativeCurveDrawsUseGpuVisibility =
        bimRouting.nativeCurveDrawsUseGpuVisibility;
    assignFrameDrawLists(p.bim.draws, bimRouting.meshDraws);
    if (bimLayers.pointCloudVisible) {
      if (bimRouting.pointPlaceholderDraws != nullptr) {
        assignFrameDrawLists(p.bim.pointDraws,
                             *bimRouting.pointPlaceholderDraws);
      }
      if (bimRouting.nativePointDraws != nullptr) {
        assignFrameDrawLists(p.bim.nativePointDraws,
                             *bimRouting.nativePointDraws);
      }
      p.bim.primitivePasses.pointCloud.enabled =
          bimRouting.pointPrimitivePassEnabled;
      p.bim.primitivePasses.pointCloud.depthTest = true;
    }
    if (bimLayers.curvesVisible) {
      if (bimRouting.curvePlaceholderDraws != nullptr) {
        assignFrameDrawLists(p.bim.curveDraws,
                             *bimRouting.curvePlaceholderDraws);
      }
      if (bimRouting.nativeCurveDraws != nullptr) {
        assignFrameDrawLists(p.bim.nativeCurveDraws,
                             *bimRouting.nativeCurveDraws);
      }
      p.bim.primitivePasses.curves.enabled =
          bimRouting.curvePrimitivePassEnabled;
      p.bim.primitivePasses.curves.depthTest = true;
    }
    hoveredBimDrawCommands_.clear();
    hoveredBimNativePointDrawCommands_.clear();
    hoveredBimNativeCurveDrawCommands_.clear();
    if (bimObjectVisibleByLayer(hoveredBimObjectIndex_)) {
      subs_.bimManager->collectDrawCommandsForObject(hoveredBimObjectIndex_,
                                                     hoveredBimDrawCommands_);
      if (bimLayers.pointCloudVisible) {
        subs_.bimManager->collectNativePointDrawCommandsForObject(
            hoveredBimObjectIndex_, hoveredBimNativePointDrawCommands_);
      }
      if (bimLayers.curvesVisible) {
        subs_.bimManager->collectNativeCurveDrawCommandsForObject(
            hoveredBimObjectIndex_, hoveredBimNativeCurveDrawCommands_);
      }
    }
    p.bim.draws.hoveredDrawCommands = &hoveredBimDrawCommands_;
    p.bim.nativePointDraws.hoveredDrawCommands =
        &hoveredBimNativePointDrawCommands_;
    p.bim.nativeCurveDraws.hoveredDrawCommands =
        &hoveredBimNativeCurveDrawCommands_;
    selectedBimDrawCommands_.clear();
    selectedBimNativePointDrawCommands_.clear();
    selectedBimNativeCurveDrawCommands_.clear();
    if (!bimFilter.hideSelection &&
        bimObjectVisibleByLayer(selectedBimObjectIndex_)) {
      subs_.bimManager->collectDrawCommandsForObject(selectedBimObjectIndex_,
                                                     selectedBimDrawCommands_);
      if (bimLayers.pointCloudVisible) {
        subs_.bimManager->collectNativePointDrawCommandsForObject(
            selectedBimObjectIndex_, selectedBimNativePointDrawCommands_);
      }
      if (bimLayers.curvesVisible) {
        subs_.bimManager->collectNativeCurveDrawCommandsForObject(
            selectedBimObjectIndex_, selectedBimNativeCurveDrawCommands_);
      }
    }
    p.bim.draws.selectedDrawCommands = &selectedBimDrawCommands_;
    p.bim.nativePointDraws.selectedDrawCommands =
        &selectedBimNativePointDrawCommands_;
    p.bim.nativeCurveDraws.selectedDrawCommands =
        &selectedBimNativeCurveDrawCommands_;
    if (subs_.guiManager != nullptr) {
      const auto &floorPlan = subs_.guiManager->bimFloorPlanOverlayState();
      p.bim.floorPlanDrawCommands =
          floorPlan.elevationMode ==
                  container::ui::BimFloorPlanElevationMode::SourceElevation
              ? &subs_.bimManager->floorPlanSourceElevationDrawCommands()
              : &subs_.bimManager->floorPlanGroundDrawCommands();
      p.bim.floorPlan.enabled =
          floorPlan.enabled && p.bim.floorPlanDrawCommands != nullptr &&
          !p.bim.floorPlanDrawCommands->empty();
      p.bim.floorPlan.depthTest = floorPlan.depthTest;
      p.bim.floorPlan.color = floorPlan.color;
      p.bim.floorPlan.opacity = floorPlan.opacity;
      p.bim.floorPlan.lineWidth = floorPlan.lineWidth;
    }
  }
  p.transformGizmo = buildTransformGizmoState();
  p.registries.pipelineHandles =
      resources_.builtPipelines.pipelines.handleRegistry.get();
  p.registries.pipelineLayouts =
      resources_.builtPipelines.layouts.layoutRegistry.get();
  p.debug.debugDirectionalOnly = debugState_.directionalOnly;
  p.debug.debugVisualizePointLightStencil =
      debugState_.visualizePointLightStencil;
  p.debug.debugFreezeCulling = debugState_.freezeCulling;
  p.debug.wireframeRasterModeSupported = svc_.ctx.wireframeRasterModeSupported;
  p.debug.wireframeWideLinesSupported = svc_.ctx.wireframeWideLinesSupported;
  pushConstants_.bindless.sectionPlaneEnabled = 0u;
  pushConstants_.bindless.semanticColorMode = 0u;
  pushConstants_.bindless.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  bool sectionPlaneActive = false;
  glm::vec4 activeSectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  container::gpu::SceneClipState sceneClipState{};
  bool boxClipActive = false;
  std::array<glm::vec4, 6> activeBoxClipPlanes{};
  if (subs_.guiManager != nullptr) {
    const auto &sectionPlane = subs_.guiManager->sectionPlaneState();
    if (sectionPlane.enabled) {
      sectionPlaneActive = true;
      activeSectionPlane = sectionPlaneEquation(sectionPlane);
      pushConstants_.bindless.sectionPlaneEnabled = 1u;
      pushConstants_.bindless.sectionPlane = activeSectionPlane;
    }
    const auto& boxClip = subs_.guiManager->bimBoxClipState();
    if (boxClip.enabled) {
      boxClipActive = true;
      activeBoxClipPlanes = makeBoxClipPlanes(boxClip);
      sceneClipState.boxClipEnabled = 1u;
      sceneClipState.boxClipInvert = boxClip.invert ? 1u : 0u;
      sceneClipState.boxClipPlaneCount =
          static_cast<uint32_t>(activeBoxClipPlanes.size());
      for (size_t i = 0; i < activeBoxClipPlanes.size(); ++i) {
        sceneClipState.boxClipPlanes[i] = activeBoxClipPlanes[i];
      }
    }
  }
  if (subs_.sceneManager != nullptr) {
    subs_.sceneManager->updateSceneClipState(sceneClipState);
  }
  if (subs_.bimManager != nullptr && subs_.guiManager != nullptr) {
    const auto& capUi = subs_.guiManager->bimClipCapHatchingUiState();
    const bool capStyleEnabled =
        (sectionPlaneActive || boxClipActive) &&
        (capUi.capPreview || capUi.hatchingPreview);
    p.bim.sectionClipCaps.enabled = capStyleEnabled;
    p.bim.sectionClipCaps.fillEnabled = capUi.capPreview;
    p.bim.sectionClipCaps.hatchEnabled = capUi.hatchingPreview;
    p.bim.sectionClipCaps.hatchMode =
        capUi.hatchingPreview ? FrameSectionClipCapHatchMode::Diagonal
                              : FrameSectionClipCapHatchMode::None;
    p.bim.sectionClipCaps.fillColor =
        glm::vec4(capUi.capColor, capUi.capOpacity);
    p.bim.sectionClipCaps.hatchColor = glm::vec4(capUi.hatchColor, 0.95f);
    p.bim.sectionClipCaps.hatchSpacing =
        std::clamp(capUi.hatchSpacing, 0.05f, 5.0f);
    p.bim.sectionClipCaps.hatchLineWidth =
        std::clamp(capUi.hatchLineWidth, 1.0f, 8.0f);
    p.bim.sectionClipCaps.hatchAngleRadians =
        std::clamp(capUi.hatchAngleDegrees, 0.0f, 180.0f) *
        0.017453292519943295f;
    p.bim.sectionClipCaps.boxClip.enabled = boxClipActive;
    p.bim.sectionClipCaps.boxClip.invert =
        sceneClipState.boxClipInvert != 0u;
    p.bim.sectionClipCaps.boxClip.planeCount =
        sceneClipState.boxClipPlaneCount;
    p.bim.sectionClipCaps.boxClip.planes = activeBoxClipPlanes;
    if (capStyleEnabled) {
      BimSectionCapBuildOptions capOptions{};
      capOptions.sectionPlane = activeSectionPlane;
      if (boxClipActive) {
        capOptions.clipPlaneCount =
            static_cast<uint32_t>(activeBoxClipPlanes.size());
        capOptions.clipPlanes = activeBoxClipPlanes;
      }
      capOptions.hatchSpacing = p.bim.sectionClipCaps.hatchSpacing;
      capOptions.hatchAngleRadians =
          p.bim.sectionClipCaps.hatchAngleRadians;
      capOptions.capOffset = p.bim.sectionClipCaps.capOffset;
      if (subs_.bimManager->rebuildSectionClipCapGeometry(capOptions)) {
        const auto& capData = subs_.bimManager->sectionClipCapDrawData();
        p.bim.sectionClipCapGeometry.scene.vertexSlice = capData.vertexSlice;
        p.bim.sectionClipCapGeometry.scene.indexSlice = capData.indexSlice;
        p.bim.sectionClipCapGeometry.scene.indexType = capData.indexType;
        p.bim.sectionClipCapGeometry.fillDrawCommands =
            &capData.fillDrawCommands;
        p.bim.sectionClipCapGeometry.hatchDrawCommands =
            &capData.hatchDrawCommands;
      }
    } else {
      subs_.bimManager->clearSectionClipCapGeometry();
    }
  }
  p.pushConstants = pushConstants_.state();
  p.swapchain.swapChainFramebuffers = &svc_.swapChainManager.framebuffers();
  p.scene.diagCubeObjectIndex = sceneState_.diagCubeObjectIndex;
  const auto *activeCamera = subs_.sceneController
                                 ? subs_.sceneController->world().activeCamera()
                                 : nullptr;
  if (activeCamera) {
    p.camera.nearPlane = activeCamera->nearPlane;
    p.camera.farPlane = activeCamera->farPlane;
  } else if (subs_.cameraController) {
    const auto *perspCam =
        dynamic_cast<const container::scene::PerspectiveCamera *>(
            subs_.cameraController->camera());
    if (perspCam) {
      p.camera.nearPlane = perspCam->nearPlane();
      p.camera.farPlane = perspCam->farPlane();
    }
  }
  if (subs_.shadowManager) {
    p.shadows.renderPass = resources_.renderPasses.shadow;
    p.shadows.shadowFramebuffers = subs_.shadowManager->framebuffers().data();
    p.shadows.shadowData = &subs_.shadowManager->shadowData();
    p.shadows.shadowSettings = subs_.guiManager
                                   ? subs_.guiManager->shadowSettings()
                                   : container::gpu::ShadowSettings{};
    p.shadows.shadowManager = subs_.shadowManager.get();
  }
  if (subs_.shadowCullManager) {
    if (subs_.shadowManager) {
      subs_.shadowCullManager->updateShadowCullDescriptor(
          imageIndex, subs_.shadowManager->shadowCullUbo(imageIndex).buffer,
          sizeof(container::gpu::ShadowCullData));
    }
    p.shadows.shadowCullManager = subs_.shadowCullManager.get();
    p.shadows.useGpuShadowCull = subs_.shadowCullManager->isReady();
  }
  p.shadows.useShadowSecondaryCommandBuffers =
      svc_.commandBufferManager.secondaryWorkerCount() >=
      container::gpu::kShadowCascadeCount;
  for (uint32_t cascadeIndex = 0;
       cascadeIndex < container::gpu::kShadowCascadeCount; ++cascadeIndex) {
    p.shadows.shadowSecondaryCommandBuffers[cascadeIndex] =
        svc_.commandBufferManager.secondaryBuffer(imageIndex, cascadeIndex, 0);
  }
  p.services.gpuCullManager = subs_.gpuCullManager.get();
  p.services.bimManager = subs_.bimManager.get();
  p.services.bloomManager = subs_.bloomManager.get();
  p.services.telemetry = subs_.rendererTelemetry.get();
  p.services.gpuProfiler = subs_.renderPassGpuProfiler.get();
  p.postProcess.exposureSettings =
      subs_.guiManager ? subs_.guiManager->exposureSettings()
                       : exposureSettingsFromConfig(svc_.config);
  p.postProcess.renderPass = resources_.renderPasses.postProcess;
  if (subs_.sceneProviderRegistry) {
    p.sceneExtraction = extractProviderSceneFrameInputs(
        *subs_.sceneProviderRegistry);
  }
  p.scene.objectBuffer = buffers_.object.buffer;
  p.scene.objectBufferSize =
      sizeof(container::gpu::ObjectData) * buffers_.objectCapacity;
  if (screenshot_.pending) {
    p.screenshot.enabled = true;
    p.screenshot.swapChainImage = svc_.swapChainManager.image(imageIndex);
    p.screenshot.readbackBuffer = screenshot_.readbackBuffer.buffer;
    p.screenshot.extent = svc_.swapChainManager.extent();
  }
  return p;
}

// ---------------------------------------------------------------------------
// Scene helpers
// ---------------------------------------------------------------------------

void RendererFrontend::syncSceneProviders() {
  if (!subs_.sceneProviderRegistry || !subs_.sceneProviderSynchronizer) {
    return;
  }

  SceneProviderSyncInput syncInput{};

  if (subs_.sceneManager) {
    const auto meshBounds =
        sceneProviderBoundsFromModelBounds(subs_.sceneManager->modelBounds());
    const std::size_t meshInstanceCount =
        subs_.sceneController ? subs_.sceneController->objectData().size() : 0u;
    const container::scene::MeshSceneAsset meshAsset =
        meshSceneAssetFromSceneManager(*subs_.sceneManager, meshInstanceCount,
                                       meshBounds);
    const uint64_t objectRevision =
        subs_.sceneController ? subs_.sceneController->objectDataRevision()
                              : 0u;
    syncInput.mesh = MeshSceneProviderSyncInput{
        .available = meshAsset.primitiveCount > 0 || meshAsset.bounds.valid,
        .primitiveCount = meshAsset.primitiveCount,
        .materialCount = meshAsset.materialCount,
        .instanceCount = meshAsset.instanceCount,
        .triangleBatches = meshAsset.triangleBatches,
        .bounds = meshAsset.bounds,
        .geometryRevision = objectRevision,
        .instanceRevision = objectRevision,
        .displayName =
            sceneProviderDisplayName(activePrimaryModelPath_,
                                     "Primary mesh scene"),
    };
  }

  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    const BimSceneStats stats = subs_.bimManager->sceneStats();
    const uint64_t objectRevision = subs_.bimManager->objectDataRevision();
    syncInput.bim = BimSceneProviderSyncInput{
        .available = true,
        .elementCount = stats.objectCount,
        .meshPrimitiveCount = stats.opaqueDrawCount +
                              stats.transparentDrawCount,
        .meshOpaqueBatchCount = stats.opaqueDrawCount,
        .meshTransparentBatchCount = stats.transparentDrawCount,
        .nativePointRangeCount = stats.nativePointOpaqueDrawCount +
                                 stats.nativePointTransparentDrawCount,
        .nativeCurveRangeCount = stats.nativeCurveOpaqueDrawCount +
                                 stats.nativeCurveTransparentDrawCount,
        .nativePointOpaqueRangeCount = stats.nativePointOpaqueDrawCount,
        .nativePointTransparentRangeCount =
            stats.nativePointTransparentDrawCount,
        .nativeCurveOpaqueRangeCount = stats.nativeCurveOpaqueDrawCount,
        .nativeCurveTransparentRangeCount =
            stats.nativeCurveTransparentDrawCount,
        .triangleBatches = subs_.bimManager->sceneProviderTriangleBatches(),
        .bounds = sceneProviderBoundsFromBim(*subs_.bimManager),
        .geometryRevision = objectRevision,
        .instanceRevision = objectRevision,
        .displayName =
            sceneProviderDisplayName(activeAuxiliaryModelPath_, "BIM scene"),
    };
  }

  subs_.sceneProviderSynchronizer->sync(*subs_.sceneProviderRegistry,
                                        syncInput);
}

void RendererFrontend::syncSceneStateFromController() {
  if (!subs_.sceneController)
    return;
  sceneState_.vertexSlice = subs_.sceneController->vertexSlice();
  sceneState_.indexSlice = subs_.sceneController->indexSlice();
}

} // namespace container::renderer
