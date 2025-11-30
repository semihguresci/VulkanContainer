#include <Container/utility/VulkanDevice.h>

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace utility::vulkan {

VulkanDevice::VulkanDevice(VkInstance instance, VkSurfaceKHR surface,
                           const DeviceCreateInfo& createInfo)
    : instance_(instance), surface_(surface), createInfo_(createInfo) {
  pickPhysicalDevice();
  createLogicalDevice();
}

VulkanDevice::~VulkanDevice() {
  if (device_ != VK_NULL_HANDLE) {
    vkDestroyDevice(device_, nullptr);
  }
}

void VulkanDevice::pickPhysicalDevice() {
  uint32_t deviceCount = 0;
  vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);

  if (deviceCount == 0) {
    throw std::runtime_error("failed to find GPUs with Vulkan support!");
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

  for (const auto& device : devices) {
    if (isDeviceSuitable(device)) {
      physicalDevice_ = device;
      break;
    }
  }

  if (physicalDevice_ == VK_NULL_HANDLE) {
    throw std::runtime_error("failed to find a suitable GPU!");
  }
}

void VulkanDevice::createLogicalDevice() {
  queueFamilyIndices_ =
      SwapChainManager::FindQueueFamilies(physicalDevice_, surface_);

  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
  std::set<uint32_t> uniqueQueueFamilies = {queueFamilyIndices_.graphicsFamily.value(),
                                            queueFamilyIndices_.presentFamily.value()};

  float queuePriority = 1.0f;
  for (uint32_t queueFamily : uniqueQueueFamilies) {
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = queueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;
    queueCreateInfos.push_back(queueCreateInfo);
  }

  VkDeviceCreateInfo deviceCreateInfo{};
  deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceCreateInfo.queueCreateInfoCount =
      static_cast<uint32_t>(queueCreateInfos.size());
  deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
  deviceCreateInfo.pEnabledFeatures = &createInfo_.enabledFeatures;
  deviceCreateInfo.enabledExtensionCount =
      static_cast<uint32_t>(createInfo_.requiredExtensions.size());
  deviceCreateInfo.ppEnabledExtensionNames = createInfo_.requiredExtensions.data();

  if (createInfo_.enableValidationLayers) {
    deviceCreateInfo.enabledLayerCount =
        static_cast<uint32_t>(createInfo_.validationLayers.size());
    deviceCreateInfo.ppEnabledLayerNames = createInfo_.validationLayers.data();
  }

  if (vkCreateDevice(physicalDevice_, &deviceCreateInfo, nullptr, &device_) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create logical device!");
  }

  vkGetDeviceQueue(device_, queueFamilyIndices_.graphicsFamily.value(), 0,
                   &graphicsQueue_);
  vkGetDeviceQueue(device_, queueFamilyIndices_.presentFamily.value(), 0,
                   &presentQueue_);
}

bool VulkanDevice::isDeviceSuitable(VkPhysicalDevice device) const {
  QueueFamilyIndices indices = SwapChainManager::FindQueueFamilies(device, surface_);

  bool extensionsSupported = checkDeviceExtensionSupport(device);

  bool swapChainAdequate = false;
  if (extensionsSupported) {
    SwapChainSupportDetails swapChainSupport =
        SwapChainManager::QuerySwapChainSupport(device, surface_);
    swapChainAdequate = !swapChainSupport.formats.empty() &&
                        !swapChainSupport.presentModes.empty();
  }

  return indices.isComplete() && extensionsSupported && swapChainAdequate &&
         supportsRequestedFeatures(device);
}

bool VulkanDevice::checkDeviceExtensionSupport(VkPhysicalDevice device) const {
  uint32_t extensionCount = 0;
  vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

  std::vector<VkExtensionProperties> availableExtensions(extensionCount);
  vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                       availableExtensions.data());

  std::set<std::string> requiredExtensions(createInfo_.requiredExtensions.begin(),
                                           createInfo_.requiredExtensions.end());

  for (const auto& extension : availableExtensions) {
    requiredExtensions.erase(extension.extensionName);
  }

  return requiredExtensions.empty();
}

bool VulkanDevice::supportsRequestedFeatures(VkPhysicalDevice device) const {
  VkPhysicalDeviceFeatures supportedFeatures{};
  vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

  const auto* supported = reinterpret_cast<const VkBool32*>(&supportedFeatures);
  const auto* requested = reinterpret_cast<const VkBool32*>(&createInfo_.enabledFeatures);
  const size_t featureCount = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);

  for (size_t i = 0; i < featureCount; ++i) {
    if (requested[i] && !supported[i]) {
      return false;
    }
  }

  return true;
}

}  // namespace utility::vulkan

