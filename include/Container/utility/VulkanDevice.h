#pragma once

#include <Container/utility/SwapChainManager.h>

#include <vulkan/vulkan.h>

#include <vector>

namespace utility::vulkan {

struct DeviceCreateInfo {
  std::vector<const char*> requiredExtensions{};
  std::vector<const char*> validationLayers{};
  bool enableValidationLayers{false};
  VkPhysicalDeviceFeatures enabledFeatures{};
  const void* next{nullptr};
};

class VulkanDevice {
 public:
  VulkanDevice(VkInstance instance, VkSurfaceKHR surface,
               const DeviceCreateInfo& createInfo);
  ~VulkanDevice();

  VulkanDevice(const VulkanDevice&) = delete;
  VulkanDevice& operator=(const VulkanDevice&) = delete;
  VulkanDevice(VulkanDevice&& other) = delete;
  VulkanDevice& operator=(VulkanDevice&& other) = delete;

  [[nodiscard]] VkPhysicalDevice physicalDevice() const {
    return physicalDevice_;
  }
  [[nodiscard]] VkDevice device() const { return device_; }
  [[nodiscard]] VkQueue graphicsQueue() const { return graphicsQueue_; }
  [[nodiscard]] VkQueue presentQueue() const { return presentQueue_; }
  [[nodiscard]] QueueFamilyIndices queueFamilyIndices() const {
    return queueFamilyIndices_;
  }

 private:
  bool isDeviceSuitable(VkPhysicalDevice device) const;
  bool checkDeviceExtensionSupport(VkPhysicalDevice device) const;
  bool supportsRequestedFeatures(VkPhysicalDevice device) const;

  void pickPhysicalDevice();
  void createLogicalDevice();

  VkInstance instance_{VK_NULL_HANDLE};
  VkSurfaceKHR surface_{VK_NULL_HANDLE};
  DeviceCreateInfo createInfo_{};

  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue graphicsQueue_{VK_NULL_HANDLE};
  VkQueue presentQueue_{VK_NULL_HANDLE};
  QueueFamilyIndices queueFamilyIndices_{};
};

}  // namespace utility::vulkan

