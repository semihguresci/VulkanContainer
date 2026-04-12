#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Container/common/CommonGLFW.h"
#include "Container/common/VulkanTypes.h"

namespace container::gpu {

class SwapChainManager {
 public:
  SwapChainManager(GLFWwindow* window, VkPhysicalDevice physicalDevice,
                   VkDevice device, VkSurfaceKHR surface);
  ~SwapChainManager();

  SwapChainManager(const SwapChainManager&) = delete;
  SwapChainManager& operator=(const SwapChainManager&) = delete;
  SwapChainManager(SwapChainManager&&) = delete;
  SwapChainManager& operator=(SwapChainManager&&) = delete;

  void initialize();
  void recreate(VkRenderPass renderPass);
  void cleanup();
  void createFramebuffers(VkRenderPass renderPass);

  [[nodiscard]] VkResult present(VkQueue presentQueue, uint32_t imageIndex,
                                 VkSemaphore waitSemaphore) const;

  [[nodiscard]] VkSwapchainKHR swapChain() const { return swapChain_; }
  [[nodiscard]] const std::vector<VkImageView>& imageViews() const {
    return swapChainImageViews_;
  }
  [[nodiscard]] size_t imageCount() const { return swapChainImages_.size(); }
  [[nodiscard]] const std::vector<VkFramebuffer>& framebuffers() const {
    return swapChainFramebuffers_;
  }
  [[nodiscard]] VkFormat imageFormat() const { return swapChainImageFormat_; }
  [[nodiscard]] VkExtent2D extent() const { return swapChainExtent_; }

  static SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device,
                                                       VkSurfaceKHR surface);

  static QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device,
                                              VkSurfaceKHR surface);

 private:
  void createSwapChain();
  void createImageViews();

  VkSurfaceFormatKHR chooseSwapSurfaceFormat(
      const std::vector<VkSurfaceFormatKHR>& availableFormats);

  VkPresentModeKHR chooseSwapPresentMode(
      const std::vector<VkPresentModeKHR>& availablePresentModes);

  VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);

  GLFWwindow* window_{nullptr};
  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkSurfaceKHR surface_{VK_NULL_HANDLE};

  VkSwapchainKHR swapChain_{VK_NULL_HANDLE};
  std::vector<VkImage> swapChainImages_;
  VkFormat swapChainImageFormat_{VK_FORMAT_UNDEFINED};
  VkExtent2D swapChainExtent_{};
  std::vector<VkImageView> swapChainImageViews_;
  std::vector<VkFramebuffer> swapChainFramebuffers_;
};

}  // namespace container::gpu
