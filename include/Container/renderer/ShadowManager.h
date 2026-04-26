#pragma once

#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonVMA.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}  // namespace container::gpu

namespace container::scene {
class BaseCamera;
}

namespace container::renderer {

struct ShadowCascadeCullBounds {
  glm::mat4 lightView{1.0f};
  glm::vec3 receiverMinBounds{0.0f};
  float     padding0{0.0f};
  glm::vec3 receiverMaxBounds{0.0f};
  float     padding1{0.0f};
  glm::vec3 casterMinBounds{0.0f};
  float     padding2{0.0f};
  glm::vec3 casterMaxBounds{0.0f};
  float     padding3{0.0f};
};

class ShadowManager {
 public:
  ShadowManager(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager&            allocationManager,
      container::gpu::PipelineManager&              pipelineManager);

  ~ShadowManager();
  ShadowManager(const ShadowManager&) = delete;
  ShadowManager& operator=(const ShadowManager&) = delete;

  // Create shadow atlas image, per-layer views, sampler, UBO, descriptor
  // resources.  Must be called once after construction.
  void createResources(VkFormat depthFormat, uint32_t descriptorSetCount);
  void recreatePerFrameResources(uint32_t descriptorSetCount);

  // Destroy all GPU resources.
  void destroy();

  // Recompute cascade splits and view-projection matrices, upload to UBO.
  void update(const container::scene::BaseCamera* camera,
              float aspectRatio,
              const glm::vec3& lightDirection,
              uint32_t imageIndex);

  // ---- Accessors -----------------------------------------------------------

  [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const {
    return descriptorSetLayout_;
  }
  [[nodiscard]] VkDescriptorSet descriptorSet(uint32_t imageIndex) const {
    return imageIndex < descriptorSets_.size() ? descriptorSets_[imageIndex]
                                               : VK_NULL_HANDLE;
  }
  [[nodiscard]] const container::gpu::AllocatedBuffer& shadowUbo(uint32_t imageIndex) const {
    return shadowUbos_[imageIndex];
  }
  [[nodiscard]] const container::gpu::AllocatedBuffer& shadowCullUbo(uint32_t imageIndex) const {
    return shadowCullUbos_[imageIndex];
  }
  [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> shadowUbos() const {
    return shadowUbos_;
  }
  [[nodiscard]] std::span<const container::gpu::AllocatedBuffer> shadowCullUbos() const {
    return shadowCullUbos_;
  }
  [[nodiscard]] const container::gpu::ShadowData& shadowData() const {
    return shadowData_;
  }
  [[nodiscard]] const container::gpu::ShadowCullData& shadowCullData() const {
    return shadowCullData_;
  }
  [[nodiscard]] const std::array<ShadowCascadeCullBounds,
                                 container::gpu::kShadowCascadeCount>&
      cascadeCullBounds() const {
    return cascadeCullBounds_;
  }
  [[nodiscard]] VkImageView shadowAtlasArrayView() const {
    return shadowAtlasArrayView_;
  }
  [[nodiscard]] VkImageView cascadeView(uint32_t cascadeIndex) const {
    return cascadeViews_[cascadeIndex];
  }
  [[nodiscard]] VkSampler shadowSampler() const { return shadowSampler_; }
  [[nodiscard]] VkFormat  depthFormat()   const { return depthFormat_; }
  [[nodiscard]] VkImage   shadowAtlasImage() const { return shadowAtlasImage_; }

  [[nodiscard]] const std::array<VkFramebuffer, container::gpu::kShadowCascadeCount>&
      framebuffers() const { return framebuffers_; }

  void createFramebuffers(VkRenderPass shadowRenderPass);
  void destroyFramebuffers();

  [[nodiscard]] bool cascadeIntersectsSphere(
      uint32_t cascadeIndex,
      const glm::vec4& boundingSphere) const;

 private:
  struct CascadeViewProjData {
    glm::mat4               viewProj{1.0f};
    ShadowCascadeCullBounds cullBounds{};
  };

  void computeCascadeSplits(float nearPlane, float farPlane);
  CascadeViewProjData computeCascadeViewProj(
      uint32_t cascadeIndex,
      const glm::vec3& lightDirection,
      const glm::mat4& cameraView,
      const glm::mat4& cameraProj,
      float cameraNear,
      float cameraFar) const;

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::PipelineManager&              pipelineManager_;

  VkFormat      depthFormat_{VK_FORMAT_UNDEFINED};
  VkImage       shadowAtlasImage_{VK_NULL_HANDLE};
  VmaAllocation shadowAtlasAllocation_{nullptr};
  VkImageView   shadowAtlasArrayView_{VK_NULL_HANDLE};
  std::array<VkImageView, container::gpu::kShadowCascadeCount>
      cascadeViews_{};
  std::array<VkFramebuffer, container::gpu::kShadowCascadeCount>
      framebuffers_{};
  VkSampler     shadowSampler_{VK_NULL_HANDLE};

  std::vector<container::gpu::AllocatedBuffer> shadowUbos_{};
  std::vector<container::gpu::AllocatedBuffer> shadowCullUbos_{};
  container::gpu::ShadowData      shadowData_{};
  container::gpu::ShadowCullData  shadowCullData_{};
  std::array<float, container::gpu::kShadowCascadeCount> cascadeSplits_{};
  std::array<ShadowCascadeCullBounds, container::gpu::kShadowCascadeCount>
      cascadeCullBounds_{};

  VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool      descriptorPool_{VK_NULL_HANDLE};
  std::vector<VkDescriptorSet> descriptorSets_{};

  static constexpr float kCascadeLambda = 0.75f;
};

}  // namespace container::renderer
