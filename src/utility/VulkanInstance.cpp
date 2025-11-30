#include <Container/utility/VulkanInstance.h>

#include <stdexcept>

namespace utility::vulkan {

VulkanInstance::VulkanInstance(const InstanceCreateInfo& createInfo) {
  if (createInfo.enableValidationLayers &&
      !checkValidationLayerSupport(createInfo.validationLayers)) {
    throw std::runtime_error(
        "validation layers requested, but not available!");
  }

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = createInfo.applicationName.c_str();
  appInfo.applicationVersion = createInfo.applicationVersion;
  appInfo.pEngineName = createInfo.engineName.c_str();
  appInfo.engineVersion = createInfo.engineVersion;
  appInfo.apiVersion = createInfo.apiVersion;

  VkInstanceCreateInfo instanceCreateInfo{};
  instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instanceCreateInfo.pApplicationInfo = &appInfo;
  instanceCreateInfo.pNext = createInfo.next;

  instanceCreateInfo.enabledExtensionCount =
      static_cast<uint32_t>(createInfo.requiredExtensions.size());
  instanceCreateInfo.ppEnabledExtensionNames =
      createInfo.requiredExtensions.data();

  if (createInfo.enableValidationLayers) {
    instanceCreateInfo.enabledLayerCount =
        static_cast<uint32_t>(createInfo.validationLayers.size());
    instanceCreateInfo.ppEnabledLayerNames = createInfo.validationLayers.data();
  } else {
    instanceCreateInfo.enabledLayerCount = 0;
  }

  if (vkCreateInstance(&instanceCreateInfo, nullptr, &instance_) != VK_SUCCESS) {
    throw std::runtime_error("failed to create instance!");
  }
}

VulkanInstance::~VulkanInstance() {
  if (instance_ != VK_NULL_HANDLE) {
    vkDestroyInstance(instance_, nullptr);
  }
}

bool VulkanInstance::checkValidationLayerSupport(
    const std::vector<const char*>& validationLayers) {
  uint32_t layerCount = 0;
  vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

  std::vector<VkLayerProperties> availableLayers(layerCount);
  vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

  for (const char* layerName : validationLayers) {
    bool layerFound = false;
    for (const auto& layerProperties : availableLayers) {
      if (std::string(layerName) == std::string(layerProperties.layerName)) {
        layerFound = true;
        break;
      }
    }

    if (!layerFound) {
      return false;
    }
  }

  return true;
}

}  // namespace utility::vulkan

