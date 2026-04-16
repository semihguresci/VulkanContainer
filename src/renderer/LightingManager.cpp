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
#include <cstdint>
#include <filesystem>
#include <stdexcept>

namespace container::renderer {

using container::gpu::kMaxDeferredPointLights;
using container::gpu::kMaxClusteredLights;
using container::gpu::kMaxLightsPerTile;
using container::gpu::kTileSize;
using container::gpu::LightingData;
using container::gpu::PointLightData;
using container::gpu::TileCullPushConstants;
using container::gpu::TileLightGrid;

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
  // Mirror point lights to the SSBO vector for tiled culling.
  pointLightsSsbo_.resize(lightingData_.pointLightCount);
  for (uint32_t i = 0; i < lightingData_.pointLightCount; ++i) {
    pointLightsSsbo_[i] = lightingData_.pointLights[i];
  }

  if (lightingBuffer_.buffer != VK_NULL_HANDLE) {
    SceneController::writeToBuffer(allocationManager_, lightingBuffer_,
                                   &lightingData_, sizeof(container::gpu::LightingData));
  }
}

void LightingManager::updateLightingData() {
  const SceneLightingAnchor anchor = computeSceneLightingAnchor();
  const glm::mat4& sceneTransform  = anchor.sceneTransform;
  const glm::vec3& center          = anchor.center;
  const float      radius          = anchor.worldRadius;

  lightingData_ = {};
  const glm::vec3 baseDir =
      glm::normalize(glm::vec3(-0.45f, -1.0f, -0.3f));
  lightingData_.directionalDirection = glm::vec4(
      glm::normalize(
          glm::vec3(sceneTransform * glm::vec4(baseDir, 0.0f))),
      0.0f);

  constexpr float kDirectionalIntensity  = 1.75f;
  constexpr float kPointLightIntensity   = 6.0f;
  constexpr float kPointLightRadiusScale = 0.5f;
  lightingData_.directionalColorIntensity =
      glm::vec4(1.0f, 0.96f, 0.9f, kDirectionalIntensity);

  lightingData_.pointLightCount = kMaxDeferredPointLights;
  const std::array<glm::vec3, kMaxDeferredPointLights> localPositions = {{
      {0.0f,  3.0f,  5.0f},
      {0.0f,  3.0f, -5.0f},
      {5.0f,  3.0f,  5.0f},
      {-5.0f, 3.0f,  5.0f},
  }};
  const std::array<glm::vec3, kMaxDeferredPointLights> colors = {{
      {1.0f,  0.78f, 0.58f},
      {0.55f, 0.7f,  1.0f },
      {1.0f,  0.58f, 0.62f},
      {0.72f, 1.0f,  0.7f },
  }};

  for (uint32_t i = 0; i < lightingData_.pointLightCount; ++i) {
    const glm::vec3 worldPos =
        glm::vec3(sceneTransform * glm::vec4(localPositions[i], 1.0f));
    lightingData_.pointLights[i].positionRadius =
        glm::vec4(worldPos, radius * kPointLightRadiusScale);
    lightingData_.pointLights[i].colorIntensity =
        glm::vec4(colors[i], kPointLightIntensity);
  }

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
  const glm::vec3 ringCenter = cameraPos + front * std::max(anchor.worldRadius * 0.9f, 2.25f);

  const std::array<glm::vec2, kMaxDeferredPointLights> ringOffsets = {{
      { 1.0f,  0.0f},
      { 0.0f,  1.0f},
      {-1.0f,  0.0f},
      { 0.0f, -1.0f},
  }};
  const std::array<glm::vec3, kMaxDeferredPointLights> ringColors = {{
      {1.0f, 0.88f, 0.72f},
      {0.78f, 0.86f, 1.0f},
      {1.0f, 0.72f, 0.82f},
      {0.82f, 1.0f, 0.80f},
  }};

  lightingData_.pointLightCount = kMaxDeferredPointLights;
  for (uint32_t i = 0; i < lightingData_.pointLightCount; ++i) {
    const glm::vec2 offset = ringOffsets[i] * ringRadius;
    const glm::vec3 lightPos = ringCenter + right * offset.x + up * offset.y;
    lightingData_.pointLights[i].positionRadius = glm::vec4(lightPos, lightRadius);
    lightingData_.pointLights[i].colorIntensity = glm::vec4(ringColors[i], 14.0f);
  }

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
      std::min(lightingData_.pointLightCount, kMaxDeferredPointLights);
  for (uint32_t i = 0; i < pointCount; ++i) {
    glm::vec3 lightColor =
        glm::vec3(lightingData_.pointLights[i].colorIntensity);
    const float maxCh =
        std::max({lightColor.r, lightColor.g, lightColor.b, 0.0001f});
    lightColor /= maxCh;
    lightColor = glm::mix(glm::max(lightColor, glm::vec3(0.35f)),
                          glm::vec3(1.0f), 0.35f);

    const glm::vec3 lightPos =
        glm::vec3(lightingData_.pointLights[i].positionRadius);
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

void LightingManager::createTiledResources(const std::filesystem::path& shaderDir) {
  // Estimate max tiles for a 4K display: 3840/16 * 2160/16 = 240 * 135 = 32400
  maxTileCount_ = 240 * 135;

  // ---- Allocate SSBOs -------------------------------------------------------
  const VkDeviceSize lightSsboSize =
      sizeof(PointLightData) * kMaxClusteredLights;
  lightSsbo_ = allocationManager_.createBuffer(
      lightSsboSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);

  const VkDeviceSize tileGridSize =
      sizeof(TileLightGrid) * maxTileCount_;
  tileGridSsbo_ = allocationManager_.createBuffer(
      tileGridSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

  const VkDeviceSize lightIndexListSize =
      sizeof(uint32_t) * maxTileCount_ * kMaxLightsPerTile;
  lightIndexListSsbo_ = allocationManager_.createBuffer(
      lightIndexListSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

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
  // Set 2: tile grid + light index list SSBOs
  {
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
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
       {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}},  // 1 (set1) + 2 (set2) + 3 (tiled frag)
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

  // ---- Write descriptors for SSBO sets (set1, set2, tiled frag) -------------
  {
    VkDescriptorBufferInfo lightInfo{lightSsbo_.buffer, 0,
                                     sizeof(PointLightData) * kMaxClusteredLights};
    VkDescriptorBufferInfo tileGridInfo{tileGridSsbo_.buffer, 0,
                                        sizeof(TileLightGrid) * maxTileCount_};
    VkDescriptorBufferInfo indexListInfo{lightIndexListSsbo_.buffer, 0,
                                         sizeof(uint32_t) * maxTileCount_ * kMaxLightsPerTile};

    std::array<VkWriteDescriptorSet, 6> writes{};
    // Cull set1 binding 0: light SSBO
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = tileCullSet1_; writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &lightInfo;
    // Cull set2 binding 0: tile grid SSBO
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = tileCullSet2_; writes[1].dstBinding = 0;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &tileGridInfo;
    // Cull set2 binding 1: light index list SSBO
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = tileCullSet2_; writes[2].dstBinding = 1;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &indexListInfo;
    // Tiled frag set binding 0: light SSBO
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = tiledDescriptorSet_; writes[3].dstBinding = 0;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &lightInfo;
    // Tiled frag set binding 1: tile grid SSBO
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = tiledDescriptorSet_; writes[4].dstBinding = 1;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &tileGridInfo;
    // Tiled frag set binding 2: light index list SSBO
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = tiledDescriptorSet_; writes[5].dstBinding = 2;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo = &indexListInfo;

    vkUpdateDescriptorSets(device_->device(),
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  }

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
                                        VkSampler depthSampler) const {
  if (!isTiledLightingReady()) return;

  uploadLightSsbo();

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

  TileCullPushConstants pc{};
  pc.tileCountX  = tileCountX;
  pc.tileCountY  = tileCountY;
  pc.totalLights = totalLights;

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

  vkCmdDispatch(cmd, tileCountX, tileCountY, 1);

  // Pipeline barrier: compute writes → fragment reads on the tile SSBOs.
  VkMemoryBarrier barrier{};
  barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 1, &barrier, 0, nullptr, 0, nullptr);
}

}  // namespace container::renderer
