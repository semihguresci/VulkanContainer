#pragma once

#include <cstddef>
#include <vector>

#include "Container/common/CommonVulkan.h"

namespace container::gpu {

class FrameSyncManager {
 public:
  FrameSyncManager(VkDevice device, size_t framesInFlight);
  ~FrameSyncManager();

  void initialize(size_t swapChainImageCount);
  void cleanup();

  [[nodiscard]] VkSemaphore imageAvailable(size_t frameIndex) const;
  [[nodiscard]] VkSemaphore renderFinishedForImage(size_t imageIndex) const;
  [[nodiscard]] VkFence fence(size_t frameIndex) const;

  void waitForFrame(size_t frameIndex) const;
  void resetFence(size_t frameIndex) const;

  void recreateRenderFinishedSemaphores(size_t swapChainImageCount);

  [[nodiscard]] size_t framesInFlight() const noexcept {
    return framesInFlight_;
  }

 private:
  void destroyRenderFinishedSemaphores();

  VkDevice device_{VK_NULL_HANDLE};
  size_t framesInFlight_{0};
  size_t swapChainImageCount_{0};

  std::vector<VkSemaphore> imageAvailableSemaphores_;
  std::vector<VkSemaphore> renderFinishedSemaphores_;  // per swapchain image
  std::vector<VkFence> inFlightFences_;
};

}  // namespace container::gpu
