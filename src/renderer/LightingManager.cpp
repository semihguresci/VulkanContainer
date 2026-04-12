#include "Container/renderer/LightingManager.h"
#include "Container/renderer/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace container::renderer {

using container::gpu::kMaxDeferredPointLights;
using container::gpu::LightingData;

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

  if (lightingBuffer_.buffer != VK_NULL_HANDLE) {
    SceneController::writeToBuffer(allocationManager_, lightingBuffer_,
                                   &lightingData_, sizeof(container::gpu::LightingData));
  }
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

}  // namespace container::renderer
