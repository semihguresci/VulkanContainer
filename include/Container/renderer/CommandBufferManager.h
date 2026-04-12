#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanDevice.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace container::renderer {

// Owns a VkCommandPool and a set of primary VkCommandBuffers (one per
// swap-chain image). Handles allocation, reallocation and destruction.
class CommandBufferManager {
 public:
  CommandBufferManager(std::shared_ptr<container::gpu::VulkanDevice> device,
                       uint32_t graphicsQueueFamily);
  ~CommandBufferManager();

  CommandBufferManager(const CommandBufferManager&) = delete;
  CommandBufferManager& operator=(const CommandBufferManager&) = delete;

  // Allocate one primary command buffer per swap-chain image.
  void allocate(size_t imageCount);

  // Free all command buffers (pool is retained).
  void free();

  // Free existing buffers and allocate a fresh set.
  void reallocate(size_t imageCount);

  [[nodiscard]] VkCommandPool             pool()                   const { return pool_; }
  [[nodiscard]] VkCommandBuffer           buffer(size_t index)     const { return buffers_[index]; }
  [[nodiscard]] size_t                    count()                  const { return buffers_.size(); }
  [[nodiscard]] const std::vector<VkCommandBuffer>& buffers()      const { return buffers_; }

 private:
  std::shared_ptr<container::gpu::VulkanDevice> device_;
  VkCommandPool                                  pool_{VK_NULL_HANDLE};
  std::vector<VkCommandBuffer>                   buffers_;
};

}  // namespace container::renderer
