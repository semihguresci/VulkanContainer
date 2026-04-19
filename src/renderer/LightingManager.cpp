#include "Container/renderer/LightingManager.h"
#include "Container/renderer/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace container::renderer {

using container::gpu::kMaxDeferredPointLights;
using container::gpu::kMaxClusteredLights;
using container::gpu::kMaxLightsPerTile;
using container::gpu::kClusterDepthSlices;
using container::gpu::kTileSize;
using container::gpu::LightCullingStats;
using container::gpu::LightingData;
using container::gpu::LightingSettings;
using container::gpu::PointLightData;
using container::gpu::TileCullPushConstants;
using container::gpu::TileLightGrid;

namespace {

constexpr uint32_t kMaxVisibleLightGizmos = 256u;
constexpr uint32_t kTimingQueryCount = 4u;
constexpr uint32_t kTimingQueryFrameSlots = 8u;
constexpr uint32_t kClusterCullStartQuery = 0u;
constexpr uint32_t kClusterCullEndQuery = 1u;
constexpr uint32_t kClusteredLightingStartQuery = 2u;
constexpr uint32_t kClusteredLightingEndQuery = 3u;

void syncLightingUboPointSlice(
    LightingData& lightingData,
    const std::vector<PointLightData>& allPointLights) {
  lightingData.pointLightCount = static_cast<uint32_t>(
      std::min<size_t>(allPointLights.size(), kMaxClusteredLights));
  std::fill(lightingData.pointLights.begin(), lightingData.pointLights.end(),
            PointLightData{});

  const uint32_t uboCount = std::min<uint32_t>(
      lightingData.pointLightCount, kMaxDeferredPointLights);
  for (uint32_t i = 0; i < uboCount; ++i) {
    lightingData.pointLights[i] = allPointLights[i];
  }
}

}  // namespace

LightingManager::LightingManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::AllocationManager&            allocationManager,
    container::gpu::PipelineManager&            pipelineManager,
    container::scene::SceneManager*                  sceneManager,
    container::scene::SceneGraph&                    sceneGraph)
    : device_(std::move(device))
    , allocationManager_(allocationManager)
    , pipelineManager_(pipelineManager)
    , sceneManager_(sceneManager)
    , sceneGraph_(sceneGraph) {
}

LightingManager::~LightingManager() {
  if (timestampQueryPool_ != VK_NULL_HANDLE && device_ &&
      device_->device() != VK_NULL_HANDLE) {
    vkDestroyQueryPool(device_->device(), timestampQueryPool_, nullptr);
    timestampQueryPool_ = VK_NULL_HANDLE;
  }
  allocationManager_.destroyBuffer(lightStatsBuffer_);
  allocationManager_.destroyBuffer(lightIndexListSsbo_);
  allocationManager_.destroyBuffer(tileGridSsbo_);
  allocationManager_.destroyBuffer(lightSsbo_);
  allocationManager_.destroyBuffer(lightingBuffer_);
}

void LightingManager::createDescriptorResources() {
  if (lightDescriptorSetLayout_ == VK_NULL_HANDLE) {
    const VkDescriptorSetLayoutBinding binding{
        0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    lightDescriptorSetLayout_ =
        pipelineManager_.createDescriptorSetLayout({binding}, {0});
  }

  if (lightingBuffer_.buffer == VK_NULL_HANDLE) {
    lightingBuffer_ = allocationManager_.createBuffer(
        sizeof(container::gpu::LightingData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }

  if (lightDescriptorPool_ == VK_NULL_HANDLE) {
    lightDescriptorPool_ = pipelineManager_.createDescriptorPool(
        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}}, 1, 0);
  }

  if (lightDescriptorSet_ == VK_NULL_HANDLE) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = lightDescriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &lightDescriptorSetLayout_;
    if (vkAllocateDescriptorSets(device_->device(), &allocInfo,
                                 &lightDescriptorSet_) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate light descriptor set");
    }
  }

  VkDescriptorBufferInfo bufInfo{lightingBuffer_.buffer, 0, sizeof(container::gpu::LightingData)};
  VkWriteDescriptorSet   write{};
  write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet          = lightDescriptorSet_;
  write.dstBinding      = 0;
  write.descriptorCount = 1;
  write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  write.pBufferInfo     = &bufInfo;
  vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
}

void LightingManager::createLightVolumeGeometry() {
  lightVolumeIndexCount_ = 36;
}

SceneLightingAnchor LightingManager::computeSceneLightingAnchor() const {
  SceneLightingAnchor anchor{};
  if (!sceneManager_) return anchor;

  const auto& bounds = sceneManager_->modelBounds();
  if (const auto* root = sceneGraph_.getNode(rootNode_)) {
    anchor.sceneTransform = root->worldTransform;
  }

  const glm::vec3 localCenter =
      bounds.valid ? bounds.center : glm::vec3(0.0f);
  anchor.center =
      glm::vec3(anchor.sceneTransform * glm::vec4(localCenter, 1.0f));

  const float sceneScale = std::max(
      {glm::length(glm::vec3(anchor.sceneTransform[0])),
       glm::length(glm::vec3(anchor.sceneTransform[1])),
       glm::length(glm::vec3(anchor.sceneTransform[2])), 1.0f});
  anchor.localRadius  = std::max(bounds.valid ? bounds.radius : 10.0f, 1.0f);
  anchor.worldRadius  = anchor.localRadius * sceneScale;
  return anchor;
}

glm::vec3 LightingManager::directionalLightPosition() const {
  const SceneLightingAnchor anchor = computeSceneLightingAnchor();
  return anchor.center -
         glm::vec3(lightingData_.directionalDirection) *
             (anchor.worldRadius * 1.15f);
}

void LightingManager::uploadLightingData() {
  if (lightingBuffer_.buffer != VK_NULL_HANDLE) {
    SceneController::writeToBuffer(allocationManager_, lightingBuffer_,
                                   &lightingData_, sizeof(container::gpu::LightingData));
  }
}

void LightingManager::setLightingSettings(
    const LightingSettings& settings) {
  lightingSettings_.preset = std::min(settings.preset, 3u);
  lightingSettings_.density =
      std::clamp(settings.density, 0.1f, 16.0f);
  lightingSettings_.radiusScale =
      std::clamp(settings.radiusScale, 0.05f, 8.0f);
  lightingSettings_.intensityScale =
      std::clamp(settings.intensityScale, 0.0f, 16.0f);
  lightingSettings_.directionalIntensity =
      std::clamp(settings.directionalIntensity, 0.0f, 16.0f);
}

void LightingManager::collectStats() {
  if (lightStatsBuffer_.buffer == VK_NULL_HANDLE ||
      lightStatsBuffer_.allocation == nullptr) {
    return;
  }

  VmaAllocationInfo allocInfo{};
  vmaGetAllocationInfo(allocationManager_.memoryManager()->allocator(),
                       lightStatsBuffer_.allocation, &allocInfo);
  if (allocInfo.pMappedData == nullptr) return;

  vmaInvalidateAllocation(allocationManager_.memoryManager()->allocator(),
                          lightStatsBuffer_.allocation, 0,
                          sizeof(uint32_t) * 4);
  const auto* data = static_cast<const uint32_t*>(allocInfo.pMappedData);
  lastStats_.maxLightsPerCluster = data[0];
  lastStats_.droppedLightReferences = data[1];
  lastStats_.activeClusters = data[2];
  lastStats_.submittedLights = data[3];
  lastStats_.totalClusters = lastDispatchClusterCount_;

  if (timestampQueryPool_ != VK_NULL_HANDLE && timingQueriesWritten_) {
    std::array<uint64_t, kTimingQueryCount * 2u> queryResults{};
    const VkResult queryResult = vkGetQueryPoolResults(
        device_->device(), timestampQueryPool_, lastTimingQueryBase_,
        kTimingQueryCount, sizeof(queryResults), queryResults.data(),
        sizeof(uint64_t) * 2u,
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    const auto queryAvailable = [&](uint32_t queryIndex) {
      return queryResults[queryIndex * 2u + 1u] != 0u;
    };
    const auto elapsedMs = [&](uint32_t startQuery, uint32_t endQuery) {
      if (queryResult != VK_SUCCESS ||
          !queryAvailable(startQuery) ||
          !queryAvailable(endQuery)) {
        return 0.0f;
      }
      const uint64_t start = queryResults[startQuery * 2u];
      const uint64_t end = queryResults[endQuery * 2u];
      if (end <= start) return 0.0f;
      return static_cast<float>(
          static_cast<double>(end - start) *
          static_cast<double>(timestampPeriodNs_) * 1.0e-6);
    };
    const float clusterCullMs =
        elapsedMs(kClusterCullStartQuery, kClusterCullEndQuery);
    const float clusteredLightingMs =
        elapsedMs(kClusteredLightingStartQuery, kClusteredLightingEndQuery);
    if (clusterCullMs > 0.0f) lastStats_.clusterCullMs = clusterCullMs;
    if (clusteredLightingMs > 0.0f)
      lastStats_.clusteredLightingMs = clusteredLightingMs;
  }
}

void LightingManager::updateLightingData() {
  const SceneLightingAnchor anchor = computeSceneLightingAnchor();
  const glm::mat4& sceneTransform  = anchor.sceneTransform;
  const float      radius          = anchor.worldRadius;

  lightingData_ = {};
  const glm::vec3 baseDir =
      glm::normalize(glm::vec3(-0.45f, -1.0f, -0.3f));
  lightingData_.directionalDirection = glm::vec4(
      glm::normalize(
          glm::vec3(sceneTransform * glm::vec4(baseDir, 0.0f))),
      0.0f);

  lightingData_.directionalColorIntensity =
      glm::vec4(1.0f, 0.96f, 0.9f,
                lightingSettings_.directionalIntensity);

  pointLightsSsbo_.clear();
  pointLightsSsbo_.reserve(std::min<uint32_t>(kMaxClusteredLights, 256u));

  const auto* bounds =
      sceneManager_ && sceneManager_->modelBounds().valid
          ? &sceneManager_->modelBounds()
          : nullptr;
  const float sceneScale =
      anchor.localRadius > 1e-5f ? radius / anchor.localRadius : 1.0f;
  const auto appendPointLight =
      [&](const glm::vec3& localPosition, const glm::vec3& color,
          float intensity, float localRadius) {
        if (pointLightsSsbo_.size() >= kMaxClusteredLights) return;
        PointLightData light{};
        light.positionRadius = glm::vec4(
            glm::vec3(sceneTransform * glm::vec4(localPosition, 1.0f)),
            std::max(localRadius * sceneScale *
                         lightingSettings_.radiusScale,
                     0.05f));
        light.colorIntensity =
            glm::vec4(color, intensity * lightingSettings_.intensityScale);
        pointLightsSsbo_.push_back(light);
      };

  if (bounds) {
    const glm::vec3 sz = bounds->size;
    const bool xIsLong = sz.x >= sz.z;
    const float longExtent = std::max(xIsLong ? sz.x : sz.z, 1.0f);
    const float crossExtent = std::max(xIsLong ? sz.z : sz.x, 1.0f);
    const float verticalExtent = std::max(sz.y, 1.0f);
    const bool hallLike = longExtent > 1.25f * crossExtent &&
                          longExtent > 1.5f * verticalExtent;

    if (hallLike) {
      constexpr uint32_t kHallLightRows = 2u;
      constexpr uint32_t kHallLightTiers = 2u;
      const float stationSpacing = std::max(crossExtent * 0.10f, 1.0f) /
                                   std::max(lightingSettings_.density, 0.1f);
      uint32_t hallStations = static_cast<uint32_t>(
          std::ceil(longExtent / stationSpacing));
      hallStations = std::clamp(hallStations, 10u, 512u);
      hallStations = std::min<uint32_t>(
          hallStations, kMaxClusteredLights / (kHallLightRows * kHallLightTiers));
      const float longMin = xIsLong ? bounds->min.x : bounds->min.z;
      const float longMax = xIsLong ? bounds->max.x : bounds->max.z;
      const float lateralCenter = xIsLong ? bounds->center.z
                                          : bounds->center.x;
      const float longInset = longExtent * 0.08f;
      const float sideOffset = crossExtent * 0.30f;
      const float lowerLightHeight = bounds->min.y + verticalExtent * 0.42f;
      const float upperLightHeight = bounds->min.y + verticalExtent * 0.68f;
      const float localRadius = std::max(crossExtent * 0.36f,
                                         verticalExtent * 0.44f);
      const std::array<glm::vec3, kHallLightRows> rowColors = {{
          {1.0f, 0.88f, 0.74f},
          {0.90f, 0.94f, 1.0f},
      }};
      const std::array<float, kHallLightTiers> tierHeights = {{
          lowerLightHeight, upperLightHeight,
      }};
      const std::array<float, kHallLightTiers> tierIntensities = {{
          3.4f, 2.1f,
      }};

      for (uint32_t station = 0; station < hallStations; ++station) {
        const float t = hallStations > 1
                            ? static_cast<float>(station) /
                                  static_cast<float>(hallStations - 1u)
                            : 0.5f;
        const float longCoord =
            (longMin + longInset) +
            ((longMax - longInset) - (longMin + longInset)) * t;
        for (uint32_t row = 0; row < kHallLightRows; ++row) {
          const float rowSign = row == 0u ? -1.0f : 1.0f;
          for (uint32_t tier = 0; tier < kHallLightTiers; ++tier) {
            glm::vec3 localPos = bounds->center;
            localPos.y = tierHeights[tier];
            if (xIsLong) {
              localPos.x = longCoord;
              localPos.z = lateralCenter + sideOffset * rowSign;
            } else {
              localPos.x = lateralCenter + sideOffset * rowSign;
              localPos.z = longCoord;
            }

            appendPointLight(localPos, rowColors[row],
                             tierIntensities[tier], localRadius);
          }
        }
      }

      syncLightingUboPointSlice(lightingData_, pointLightsSsbo_);
      uploadLightingData();
      return;
    }
  }

  const uint32_t densitySteps = static_cast<uint32_t>(
      std::round(std::max(lightingSettings_.density, 0.1f)));
  const uint32_t kFallbackGridX = std::clamp(4u * densitySteps, 2u, 32u);
  const uint32_t kFallbackGridZ = std::clamp(4u * densitySteps, 2u, 32u);
  const uint32_t kFallbackGridY = std::clamp(2u * densitySteps, 1u, 12u);
  const glm::vec3 localCenter =
      bounds ? bounds->center : glm::vec3(0.0f);
  const glm::vec3 localSize =
      bounds ? glm::max(bounds->size, glm::vec3(1.0f)) : glm::vec3(10.0f);
  const glm::vec3 localMin = bounds ? bounds->min
                                    : localCenter - localSize * 0.5f;
  const glm::vec3 localMax = bounds ? bounds->max
                                    : localCenter + localSize * 0.5f;
  const float localRadius =
      std::max({localSize.x, localSize.y, localSize.z}) * 0.32f;
  for (uint32_t y = 0; y < kFallbackGridY; ++y) {
    const float ty = (static_cast<float>(y) + 0.5f) /
                     static_cast<float>(kFallbackGridY);
    for (uint32_t z = 0; z < kFallbackGridZ; ++z) {
      const float tz = (static_cast<float>(z) + 0.5f) /
                       static_cast<float>(kFallbackGridZ);
      for (uint32_t x = 0; x < kFallbackGridX; ++x) {
        const float tx = (static_cast<float>(x) + 0.5f) /
                         static_cast<float>(kFallbackGridX);
        glm::vec3 localPos{
            localMin.x + (localMax.x - localMin.x) * tx,
            localMin.y + (localMax.y - localMin.y) * (0.35f + ty * 0.35f),
            localMin.z + (localMax.z - localMin.z) * tz,
        };
        const bool warm = ((x + y + z) & 1u) == 0u;
        appendPointLight(localPos,
                         warm ? glm::vec3(1.0f, 0.86f, 0.72f)
                              : glm::vec3(0.88f, 0.93f, 1.0f),
                         2.8f, localRadius);
      }
    }
  }

  syncLightingUboPointSlice(lightingData_, pointLightsSsbo_);
  uploadLightingData();
}

void LightingManager::updateLightingData(const container::scene::BaseCamera* camera) {
  updateLightingData();

  if (!sceneManager_ || !sceneManager_->isDefaultTestSceneActive() || !camera) {
    return;
  }

  const SceneLightingAnchor anchor = computeSceneLightingAnchor();
  const glm::vec3 cameraPos = camera->position();
  const glm::vec3 front = camera->frontVector();
  const glm::vec3 up = camera->upVector(front);
  const glm::vec3 right = camera->rightVector(front, up);
  const float ringRadius = std::max(anchor.worldRadius * 0.75f, 1.5f);
  const float lightRadius = std::max(anchor.worldRadius * 1.35f, 3.0f);
  const glm::vec3 ringCenter =
      cameraPos + front * std::max(anchor.worldRadius * 0.9f, 2.25f);

  constexpr uint32_t kCameraRingLightCount = 4u;
  const std::array<glm::vec2, kCameraRingLightCount> ringOffsets = {{
      { 1.0f,  0.0f},
      { 0.0f,  1.0f},
      {-1.0f,  0.0f},
      { 0.0f, -1.0f},
  }};
  const std::array<glm::vec3, kCameraRingLightCount> ringColors = {{
      {1.0f, 0.88f, 0.72f},
      {0.78f, 0.86f, 1.0f},
      {1.0f, 0.72f, 0.82f},
      {0.82f, 1.0f, 0.80f},
  }};

  pointLightsSsbo_.clear();
  pointLightsSsbo_.reserve(kCameraRingLightCount);
  for (uint32_t i = 0; i < kCameraRingLightCount; ++i) {
    const glm::vec2 offset = ringOffsets[i] * ringRadius;
    const glm::vec3 lightPos = ringCenter + right * offset.x + up * offset.y;
    PointLightData light{};
    light.positionRadius = glm::vec4(lightPos, lightRadius);
    light.colorIntensity = glm::vec4(ringColors[i], 14.0f);
    pointLightsSsbo_.push_back(light);
  }

  syncLightingUboPointSlice(lightingData_, pointLightsSsbo_);
  uploadLightingData();
}

void LightingManager::drawLightGizmos(
    VkCommandBuffer                        commandBuffer,
    const std::array<VkDescriptorSet, 2>&  lightingDescriptorSets,
    VkPipeline                             lightGizmoPipeline,
    VkPipelineLayout                       lightingPipelineLayout,
    const container::scene::BaseCamera*     camera) const {
  if (lightGizmoPipeline == VK_NULL_HANDLE) return;

  const SceneLightingAnchor anchor       = computeSceneLightingAnchor();
  const glm::vec3           cameraPos    =
      camera ? camera->position() : anchor.center;

  const auto computeGizmoExtent =
      [&](const glm::vec3& worldPos, float radiusBias) {
        const float dist =
            glm::max(glm::length(worldPos - cameraPos), 0.1f);
        return std::clamp(dist * 0.03f + radiusBias, 0.25f, 6.0f);
      };

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    lightGizmoPipeline);
  vkCmdBindDescriptorSets(
      commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, lightingPipelineLayout,
      0, static_cast<uint32_t>(lightingDescriptorSets.size()),
      lightingDescriptorSets.data(), 0, nullptr);

  LightPushConstants pc{};

  // Directional-light gizmo
  glm::vec3 dirColor =
      glm::vec3(lightingData_.directionalColorIntensity);
  const float dirMax = std::max(
      {dirColor.r, dirColor.g, dirColor.b, 0.0001f});
  dirColor /= dirMax;
  dirColor = glm::mix(glm::max(dirColor, glm::vec3(0.35f)),
                      glm::vec3(1.0f, 0.95f, 0.35f), 0.5f);
  const glm::vec3 dirGizmoPos =
      anchor.center -
      glm::vec3(lightingData_.directionalDirection) *
          (anchor.worldRadius * 1.15f);
  const float dirGizmoExtent = computeGizmoExtent(
      dirGizmoPos,
      std::clamp(anchor.worldRadius * 0.02f, 0.05f, 1.0f));
  pc.positionRadius = glm::vec4(dirGizmoPos, dirGizmoExtent);
  pc.colorIntensity = glm::vec4(dirColor, 1.0f);
  vkCmdPushConstants(commandBuffer, lightingPipelineLayout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(LightPushConstants), &pc);
  vkCmdDraw(commandBuffer, 6, 1, 0, 0);

  // Point-light gizmos
  const uint32_t pointCount =
      std::min<uint32_t>(
          static_cast<uint32_t>(pointLightsSsbo_.size()),
          kMaxVisibleLightGizmos);
  for (uint32_t i = 0; i < pointCount; ++i) {
    glm::vec3 lightColor =
        glm::vec3(pointLightsSsbo_[i].colorIntensity);
    const float maxCh =
        std::max({lightColor.r, lightColor.g, lightColor.b, 0.0001f});
    lightColor /= maxCh;
    lightColor = glm::mix(glm::max(lightColor, glm::vec3(0.35f)),
                          glm::vec3(1.0f), 0.35f);

    const glm::vec3 lightPos =
        glm::vec3(pointLightsSsbo_[i].positionRadius);
    const float pointExtent = computeGizmoExtent(
        lightPos,
        std::clamp(anchor.worldRadius * 0.015f, 0.04f, 0.75f));
    pc.positionRadius = glm::vec4(lightPos, pointExtent);
    pc.colorIntensity = glm::vec4(lightColor, 1.0f);
    vkCmdPushConstants(commandBuffer, lightingPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(LightPushConstants), &pc);
    vkCmdDraw(commandBuffer, 6, 1, 0, 0);
  }
}

// ---------------------------------------------------------------------------
// Tiled light culling
// ---------------------------------------------------------------------------

void LightingManager::allocateClusterBuffers(VkExtent2D extent) {
  const uint32_t tileCountX =
      std::max(1u, (extent.width + kTileSize - 1u) / kTileSize);
  const uint32_t tileCountY =
      std::max(1u, (extent.height + kTileSize - 1u) / kTileSize);
  const uint32_t requiredTileCount = tileCountX * tileCountY;
  const uint32_t requiredClusterCount =
      requiredTileCount * kClusterDepthSlices;

  if (requiredClusterCount == maxClusterCount_ &&
      tileGridSsbo_.buffer != VK_NULL_HANDLE &&
      lightIndexListSsbo_.buffer != VK_NULL_HANDLE) {
    return;
  }

  allocationManager_.destroyBuffer(lightIndexListSsbo_);
  allocationManager_.destroyBuffer(tileGridSsbo_);

  maxTileCount_ = requiredTileCount;
  maxClusterCount_ = requiredClusterCount;

  const VkDeviceSize tileGridSize =
      sizeof(TileLightGrid) * maxClusterCount_;
  tileGridSsbo_ = allocationManager_.createBuffer(
      tileGridSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

  const VkDeviceSize lightIndexListSize =
      sizeof(uint32_t) * maxClusterCount_ * kMaxLightsPerTile;
  lightIndexListSsbo_ = allocationManager_.createBuffer(
      lightIndexListSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
}

void LightingManager::writeTiledResourceDescriptors() const {
  if (tileCullSet1_ == VK_NULL_HANDLE ||
      tileCullSet2_ == VK_NULL_HANDLE ||
      tiledDescriptorSet_ == VK_NULL_HANDLE ||
      lightSsbo_.buffer == VK_NULL_HANDLE ||
      tileGridSsbo_.buffer == VK_NULL_HANDLE ||
      lightIndexListSsbo_.buffer == VK_NULL_HANDLE ||
      lightStatsBuffer_.buffer == VK_NULL_HANDLE ||
      maxClusterCount_ == 0) {
    return;
  }

  VkDescriptorBufferInfo lightInfo{
      lightSsbo_.buffer, 0, sizeof(PointLightData) * kMaxClusteredLights};
  VkDescriptorBufferInfo tileGridInfo{
      tileGridSsbo_.buffer, 0, sizeof(TileLightGrid) * maxClusterCount_};
  VkDescriptorBufferInfo indexListInfo{
      lightIndexListSsbo_.buffer, 0,
      sizeof(uint32_t) * maxClusterCount_ * kMaxLightsPerTile};
  VkDescriptorBufferInfo statsInfo{
      lightStatsBuffer_.buffer, 0, sizeof(uint32_t) * 4};

  std::array<VkWriteDescriptorSet, 7> writes{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = tileCullSet1_;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &lightInfo;

  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = tileCullSet2_;
  writes[1].dstBinding = 0;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo = &tileGridInfo;

  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = tileCullSet2_;
  writes[2].dstBinding = 1;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[2].pBufferInfo = &indexListInfo;

  writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[3].dstSet = tileCullSet2_;
  writes[3].dstBinding = 2;
  writes[3].descriptorCount = 1;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[3].pBufferInfo = &statsInfo;

  writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[4].dstSet = tiledDescriptorSet_;
  writes[4].dstBinding = 0;
  writes[4].descriptorCount = 1;
  writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[4].pBufferInfo = &lightInfo;

  writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[5].dstSet = tiledDescriptorSet_;
  writes[5].dstBinding = 1;
  writes[5].descriptorCount = 1;
  writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[5].pBufferInfo = &tileGridInfo;

  writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[6].dstSet = tiledDescriptorSet_;
  writes[6].dstBinding = 2;
  writes[6].descriptorCount = 1;
  writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[6].pBufferInfo = &indexListInfo;

  vkUpdateDescriptorSets(device_->device(),
                         static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);
}

void LightingManager::resizeTiledResources(VkExtent2D extent) {
  if (lightSsbo_.buffer == VK_NULL_HANDLE ||
      lightStatsBuffer_.buffer == VK_NULL_HANDLE) {
    return;
  }

  const uint32_t previousClusterCount = maxClusterCount_;
  allocateClusterBuffers(extent);
  if (maxClusterCount_ != previousClusterCount) {
    writeTiledResourceDescriptors();
  }
}

void LightingManager::createTiledResources(const std::filesystem::path& shaderDir,
                                           VkExtent2D initialExtent) {

  // ---- Allocate SSBOs -------------------------------------------------------
  const VkDeviceSize lightSsboSize =
      sizeof(PointLightData) * kMaxClusteredLights;
  lightSsbo_ = allocationManager_.createBuffer(
      lightSsboSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

  lightStatsBuffer_ = allocationManager_.createBuffer(
      sizeof(uint32_t) * 4,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  const std::array<uint32_t, 4> zeroStats{};
  SceneController::writeToBuffer(allocationManager_, lightStatsBuffer_,
                                 zeroStats.data(),
                                 sizeof(uint32_t) * zeroStats.size());

  if (timestampQueryPool_ == VK_NULL_HANDLE) {
    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(device_->physicalDevice(), &deviceProperties);
    timestampPeriodNs_ = deviceProperties.limits.timestampPeriod;

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        device_->physicalDevice(), &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(
        device_->physicalDevice(), &queueFamilyCount, queueFamilies.data());

    const auto queueIndices = device_->queueFamilyIndices();
    if (queueIndices.graphicsFamily.has_value() &&
        queueIndices.graphicsFamily.value() < queueFamilies.size() &&
        queueFamilies[queueIndices.graphicsFamily.value()].timestampValidBits > 0) {
      VkQueryPoolCreateInfo queryPoolInfo{
          VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
      queryPoolInfo.queryCount =
          kTimingQueryFrameSlots * kTimingQueryCount;
      timestampQueriesSupported_ =
          vkCreateQueryPool(device_->device(), &queryPoolInfo, nullptr,
                            &timestampQueryPool_) == VK_SUCCESS;
      if (!timestampQueriesSupported_) {
        timestampQueryPool_ = VK_NULL_HANDLE;
      }
    }
  }

  // ---- Descriptor set layouts -----------------------------------------------
  // Set 0: camera UBO + depth texture + depth sampler
  {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_SAMPLER,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    tileCullSet0Layout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }
  // Set 1: light SSBO
  {
    const std::array<VkDescriptorSetLayoutBinding, 1> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    tileCullSet1Layout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }
  // Set 2: cluster grid + light index list + stats SSBOs
  {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    tileCullSet2Layout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }

  // Tiled lighting fragment shader descriptor set (set 1 for the lighting pipeline):
  // 3 SSBO bindings: light SSBO, tile grid, light index list.
  {
    const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    const std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
    tiledDescriptorSetLayout_ = pipelineManager_.createDescriptorSetLayout(
        {bindings.begin(), bindings.end()}, flags);
  }

  // ---- Descriptor pools & sets ----------------------------------------------
  tileCullDescriptorPool_ = pipelineManager_.createDescriptorPool(
      {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
       {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
       {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7}},
      4, 0);

  auto allocSet = [&](VkDescriptorSetLayout layout) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool     = tileCullDescriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &layout;
    if (vkAllocateDescriptorSets(device_->device(), &ai, &set) != VK_SUCCESS)
      throw std::runtime_error("failed to allocate tiled descriptor set");
    return set;
  };

  tileCullSet0_        = allocSet(tileCullSet0Layout_);
  tileCullSet1_        = allocSet(tileCullSet1Layout_);
  tileCullSet2_        = allocSet(tileCullSet2Layout_);
  tiledDescriptorSet_  = allocSet(tiledDescriptorSetLayout_);

  resizeTiledResources(initialExtent);

  // ---- Compute pipeline -----------------------------------------------------
  {
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size       = sizeof(TileCullPushConstants);

    tileCullPipelineLayout_ = pipelineManager_.createPipelineLayout(
        {tileCullSet0Layout_, tileCullSet1Layout_, tileCullSet2Layout_},
        {pcRange});

    std::filesystem::path compPath = shaderDir / "spv_shaders" / "tile_light_cull.comp.spv";
    const auto spvData = container::util::readFile(compPath);
    VkShaderModule compModule =
        container::gpu::createShaderModule(device_->device(), spvData);

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = compModule;
    stage.pName  = "computeMain";

    VkComputePipelineCreateInfo ci{};
    ci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage  = stage;
    ci.layout = tileCullPipelineLayout_;

    tileCullPipeline_ = pipelineManager_.createComputePipeline(ci, "tile_light_cull");

    vkDestroyShaderModule(device_->device(), compModule, nullptr);
  }
}

void LightingManager::uploadLightSsbo() const {
  if (lightSsbo_.buffer == VK_NULL_HANDLE) return;
  const uint32_t count =
      std::min(static_cast<uint32_t>(pointLightsSsbo_.size()), kMaxClusteredLights);
  if (count == 0) return;
  SceneController::writeToBuffer(allocationManager_, lightSsbo_,
                                 pointLightsSsbo_.data(),
                                 sizeof(PointLightData) * count);
}

void LightingManager::dispatchTileCull(VkCommandBuffer cmd,
                                        VkExtent2D screenExtent,
                                        VkBuffer cameraBuffer,
                                        VkDeviceSize cameraBufferSize,
                                        VkImageView depthView,
                                        VkSampler depthSampler,
                                        float cameraNear,
                                        float cameraFar) const {
  if (!isTiledLightingReady()) return;

  uploadLightSsbo();

  if (lightStatsBuffer_.buffer != VK_NULL_HANDLE) {
    vkCmdFillBuffer(cmd, lightStatsBuffer_.buffer, 0,
                    sizeof(uint32_t) * 4, 0u);

    VkMemoryBarrier statsClearBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    statsClearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    statsClearBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &statsClearBarrier, 0, nullptr, 0, nullptr);
  }

  // Update tileCullSet0_ with the current frame's camera + depth resources.
  {
    VkDescriptorBufferInfo camInfo{cameraBuffer, 0, cameraBufferSize};
    VkDescriptorImageInfo depthInfo{};
    depthInfo.imageView   = depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    VkDescriptorImageInfo sampInfo{};
    sampInfo.sampler = depthSampler;

    std::array<VkWriteDescriptorSet, 3> w{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = tileCullSet0_; w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &camInfo;

    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = tileCullSet0_; w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[1].pImageInfo = &depthInfo;

    w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[2].dstSet = tileCullSet0_; w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[2].pImageInfo = &sampInfo;

    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(w.size()),
                           w.data(), 0, nullptr);
  }

  const uint32_t tileCountX = (screenExtent.width  + kTileSize - 1) / kTileSize;
  const uint32_t tileCountY = (screenExtent.height + kTileSize - 1) / kTileSize;
  const uint32_t totalLights =
      std::min(static_cast<uint32_t>(pointLightsSsbo_.size()), kMaxClusteredLights);
  lastDispatchClusterCount_ = tileCountX * tileCountY * kClusterDepthSlices;
  if (lastDispatchClusterCount_ > maxClusterCount_ ||
      tileGridSsbo_.buffer == VK_NULL_HANDLE ||
      lightIndexListSsbo_.buffer == VK_NULL_HANDLE) {
    lastDispatchClusterCount_ = 0;
    return;
  }

  TileCullPushConstants pc{};
  pc.tileCountX  = tileCountX;
  pc.tileCountY  = tileCountY;
  pc.depthSliceCount = kClusterDepthSlices;
  pc.totalLights = totalLights;
  pc.cameraNear = cameraNear;
  pc.cameraFar = cameraFar;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tileCullPipeline_);

  std::array<VkDescriptorSet, 3> sets = {
      tileCullSet0_, tileCullSet1_, tileCullSet2_};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          tileCullPipelineLayout_, 0,
                          static_cast<uint32_t>(sets.size()),
                          sets.data(), 0, nullptr);

  vkCmdPushConstants(cmd, tileCullPipelineLayout_,
                     VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(TileCullPushConstants), &pc);

  vkCmdDispatch(cmd, tileCountX, tileCountY, kClusterDepthSlices);

  // Pipeline barrier: compute writes → fragment reads on the tile SSBOs.
  VkMemoryBarrier barrier{};
  barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_HOST_BIT,
                       0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void LightingManager::resetGpuTimers(VkCommandBuffer cmd,
                                      uint32_t frameSlot) const {
  if (!timestampQueriesSupported_ ||
      timestampQueryPool_ == VK_NULL_HANDLE) {
    return;
  }
  const uint32_t slot = frameSlot % kTimingQueryFrameSlots;
  lastTimingQueryBase_ = slot * kTimingQueryCount;
  timingQueriesWritten_ = false;
  vkCmdResetQueryPool(cmd, timestampQueryPool_, lastTimingQueryBase_,
                      kTimingQueryCount);
}

void LightingManager::beginClusterCullTimer(VkCommandBuffer cmd) const {
  if (!timestampQueriesSupported_ ||
      timestampQueryPool_ == VK_NULL_HANDLE) {
    return;
  }
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      timestampQueryPool_,
                      lastTimingQueryBase_ + kClusterCullStartQuery);
}

void LightingManager::endClusterCullTimer(VkCommandBuffer cmd) const {
  if (!timestampQueriesSupported_ ||
      timestampQueryPool_ == VK_NULL_HANDLE) {
    return;
  }
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      timestampQueryPool_,
                      lastTimingQueryBase_ + kClusterCullEndQuery);
  timingQueriesWritten_ = true;
}

void LightingManager::beginClusteredLightingTimer(VkCommandBuffer cmd) const {
  if (!timestampQueriesSupported_ ||
      timestampQueryPool_ == VK_NULL_HANDLE) {
    return;
  }
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      timestampQueryPool_,
                      lastTimingQueryBase_ + kClusteredLightingStartQuery);
}

void LightingManager::endClusteredLightingTimer(VkCommandBuffer cmd) const {
  if (!timestampQueriesSupported_ ||
      timestampQueryPool_ == VK_NULL_HANDLE) {
    return;
  }
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      timestampQueryPool_,
                      lastTimingQueryBase_ + kClusteredLightingEndQuery);
  timingQueriesWritten_ = true;
}

}  // namespace container::renderer
