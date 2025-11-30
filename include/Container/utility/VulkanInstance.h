#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace utility::vulkan {

struct InstanceCreateInfo {
  std::string applicationName{"VulkanApplication"};
  uint32_t applicationVersion{VK_MAKE_VERSION(1, 0, 0)};
  std::string engineName{"VulkanEngine"};
  uint32_t engineVersion{VK_MAKE_VERSION(1, 0, 0)};
  uint32_t apiVersion{VK_API_VERSION_1_3};

  bool enableValidationLayers{false};
  std::vector<const char*> validationLayers{};
  std::vector<const char*> requiredExtensions{};
  const void* next{nullptr};
};

class VulkanInstance {
 public:
  explicit VulkanInstance(const InstanceCreateInfo& createInfo);
  ~VulkanInstance();

  VulkanInstance(const VulkanInstance&) = delete;
  VulkanInstance& operator=(const VulkanInstance&) = delete;
  VulkanInstance(VulkanInstance&& other) = delete;
  VulkanInstance& operator=(VulkanInstance&& other) = delete;

  [[nodiscard]] VkInstance instance() const { return instance_; }

 private:
  static bool checkValidationLayerSupport(
      const std::vector<const char*>& validationLayers);

  VkInstance instance_{VK_NULL_HANDLE};
};

}  // namespace utility::vulkan

