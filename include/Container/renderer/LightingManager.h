#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonMath.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <cstdint>
#include <memory>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}  // namespace container::gpu

namespace container::scene {
class BaseCamera;
class SceneManager;
}  // namespace container::scene

namespace container::renderer {

// Struct carrying the scene-space anchor used for light positioning.
struct SceneLightingAnchor {
  glm::mat4 sceneTransform{1.0f};
  glm::vec3 center{0.0f, 0.0f, 0.0f};
  float     localRadius{1.0f};
  float     worldRadius{1.0f};
};

// Push constants for light-gizmo draw calls.
struct LightPushConstants {
  glm::vec4 positionRadius{0.0f, 0.0f, 0.0f, 1.0f};
  glm::vec4 colorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
};

// Manages the LightingData uniform buffer, directional + point lights,
// and the light-gizmo draw helper.
class LightingManager {
 public:
  LightingManager(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager&            allocationManager,
      container::gpu::PipelineManager&            pipelineManager,
      container::scene::SceneManager*                  sceneManager,
      container::scene::SceneGraph&                    sceneGraph);

  ~LightingManager() = default;
  LightingManager(const LightingManager&) = delete;
  LightingManager& operator=(const LightingManager&) = delete;

  // ---- Lifecycle ----------------------------------------------------------

  // Trivial: sets lightVolumeIndexCount = 36.
  void createLightVolumeGeometry();

  // Creates lightDescriptorSetLayout, lightingBuffer, lightDescriptorPool,
  // lightDescriptorSet, and writes the initial descriptor.
  void createDescriptorResources();

  // ---- Per-frame updates --------------------------------------------------

  // Recomputes LightingData from current scene anchor and uploads to lightingBuffer_.
  void updateLightingData();

  // ---- Draw helpers -------------------------------------------------------

  // Records the directional-light gizmo + point-light gizmos into commandBuffer.
  // Requires the light gizmo pipeline to already be bound by the caller.
  void drawLightGizmos(
      VkCommandBuffer                          commandBuffer,
      const std::array<VkDescriptorSet, 2>&    lightingDescriptorSets,
      VkPipeline                               lightGizmoPipeline,
      VkPipelineLayout                         lightingPipelineLayout,
      const container::scene::BaseCamera*       camera) const;

  // ---- Accessors ----------------------------------------------------------

  const container::gpu::LightingData& lightingData()      const { return lightingData_; }
  container::gpu::LightingData&       lightingData()            { return lightingData_; }
  uint32_t            lightVolumeIndexCount() const { return lightVolumeIndexCount_; }

  // Descriptor set / buffer accessors (valid after createDescriptorResources())
  VkDescriptorSetLayout lightDescriptorSetLayout() const { return lightDescriptorSetLayout_; }
  VkDescriptorSet       lightDescriptorSet()        const { return lightDescriptorSet_; }
  VkDescriptorPool      lightDescriptorPool()       const { return lightDescriptorPool_; }
  const container::gpu::AllocatedBuffer& lightingBuffer() const { return lightingBuffer_; }

  SceneLightingAnchor computeSceneLightingAnchor() const;
  glm::vec3           directionalLightPosition()   const;

  // The root node must be kept in sync with the scene graph after buildSceneGraph.
  void setRootNode(uint32_t rootNode) { rootNode_ = rootNode; }

 private:
  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::PipelineManager&            pipelineManager_;
  container::scene::SceneManager*                  sceneManager_{nullptr};
  container::scene::SceneGraph&                    sceneGraph_;

  container::gpu::LightingData lightingData_{};
  uint32_t     lightVolumeIndexCount_{0};
  uint32_t     rootNode_{container::scene::SceneGraph::kInvalidNode};

  // Descriptor resources created by createDescriptorResources()
  container::gpu::AllocatedBuffer lightingBuffer_{};
  VkDescriptorSetLayout            lightDescriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool                 lightDescriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet                  lightDescriptorSet_{VK_NULL_HANDLE};
};

}  // namespace container::renderer
