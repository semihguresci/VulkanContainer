#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/VulkanDevice.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace container::renderer {

// Owns primary command buffers plus optional per-worker secondary buffers.
// Secondary buffers use separate command pools so worker threads can record
// in parallel without synchronizing on the primary command pool.
class CommandBufferManager {
 public:
  CommandBufferManager(std::shared_ptr<container::gpu::VulkanDevice> device,
                       uint32_t graphicsQueueFamily);
  ~CommandBufferManager();

  CommandBufferManager(const CommandBufferManager&) = delete;
  CommandBufferManager& operator=(const CommandBufferManager&) = delete;

  // Allocate one primary command buffer per swap-chain image.
  void allocate(size_t imageCount);

  // Free all command buffers. The primary pool is retained; secondary worker
  // pools are recreated with their buffers because they are only used for
  // parallel recording slots.
  void free();

  // Free existing buffers and allocate a fresh set.
  void reallocate(size_t imageCount);

  // Configure secondary buffers for parallel recording. Each worker owns a
  // separate command pool so worker threads never record from a shared pool.
  void configureSecondaryBuffers(uint32_t workerCount,
                                 uint32_t buffersPerWorker);

  [[nodiscard]] VkCommandPool             pool()                   const { return pool_; }
  [[nodiscard]] VkCommandBuffer           buffer(size_t index)     const { return buffers_[index]; }
  [[nodiscard]] VkCommandBuffer           secondaryBuffer(size_t imageIndex,
                                                          uint32_t workerIndex,
                                                          uint32_t slotIndex) const;
  [[nodiscard]] size_t                    count()                  const { return buffers_.size(); }
  [[nodiscard]] uint32_t                  secondaryWorkerCount()   const { return secondaryWorkerCount_; }
  [[nodiscard]] uint32_t                  secondaryBuffersPerWorker() const { return secondaryBuffersPerWorker_; }
  [[nodiscard]] const std::vector<VkCommandBuffer>& buffers()      const { return buffers_; }

 private:
  [[nodiscard]] VkCommandPool createPool() const;
  void allocateSecondary(size_t imageCount);
  void freeSecondary();

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  uint32_t                                       graphicsQueueFamily_{0};
  VkCommandPool                                  pool_{VK_NULL_HANDLE};
  std::vector<VkCommandBuffer>                   buffers_;
  uint32_t                                       secondaryWorkerCount_{0};
  uint32_t                                       secondaryBuffersPerWorker_{0};
  std::vector<VkCommandPool>                     secondaryPools_;
  std::vector<std::vector<VkCommandBuffer>>      secondaryBuffersByWorker_;
};

}  // namespace container::renderer
