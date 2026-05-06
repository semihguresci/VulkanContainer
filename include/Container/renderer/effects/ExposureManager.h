#pragma once

#include "Container/common/CommonVMA.h"
#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
struct ExposureSettings;
class VulkanDevice;
}  // namespace container::gpu

namespace container::renderer {

// GPU luminance histogram and GPU-resident exposure adaptation for the
// post-process tone mapper.
class ExposureManager {
 public:
  static constexpr uint32_t kHistogramBinCount = 64;

  ExposureManager(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager& allocationManager,
      container::gpu::PipelineManager& pipelineManager);
  ~ExposureManager();

  ExposureManager(const ExposureManager&) = delete;
  ExposureManager& operator=(const ExposureManager&) = delete;

  void createResources(const std::filesystem::path& shaderDir);
  void dispatch(VkCommandBuffer cmd,
                VkImageView sceneColorView,
                uint32_t sceneWidth,
                uint32_t sceneHeight,
                const container::gpu::ExposureSettings& settings);
  void collectReadback(const container::gpu::ExposureSettings& settings);
  void destroy();

  [[nodiscard]] bool isReady() const {
    return histogramPipeline_ != VK_NULL_HANDLE &&
           adaptPipeline_ != VK_NULL_HANDLE &&
           descriptorSet_ != VK_NULL_HANDLE &&
           histogramBuffer_.buffer != VK_NULL_HANDLE &&
           exposureStateBuffer_.buffer != VK_NULL_HANDLE;
  }

  [[nodiscard]] float resolvedExposure(
      const container::gpu::ExposureSettings& settings) const;
  [[nodiscard]] float averageLuminance() const { return averageLuminance_; }
  [[nodiscard]] bool hasExposureDebugState() const { return hasCurrentExposure_; }
  [[nodiscard]] VkBuffer exposureStateBuffer() const {
    return exposureStateBuffer_.buffer;
  }
  [[nodiscard]] VkDeviceSize exposureStateBufferSize() const;

 private:
  void createPipeline(const std::filesystem::path& shaderDir);
  void createHistogramBuffer();
  void createExposureStateBuffer();
  void updateDescriptorSet(VkImageView sceneColorView);

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager& allocationManager_;
  container::gpu::PipelineManager& pipelineManager_;

  container::gpu::AllocatedBuffer histogramBuffer_{};
  container::gpu::AllocatedBuffer exposureStateBuffer_{};
  VkDescriptorSetLayout setLayout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout_{VK_NULL_HANDLE};
  VkPipeline histogramPipeline_{VK_NULL_HANDLE};
  VkPipeline adaptPipeline_{VK_NULL_HANDLE};

  float currentExposure_{0.25f};
  float averageLuminance_{0.18f};
  bool hasCurrentExposure_{false};
};

}  // namespace container::renderer
