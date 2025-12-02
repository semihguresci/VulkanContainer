#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <vector>

namespace app {

struct AppConfig {
  uint32_t windowWidth{800};
  uint32_t windowHeight{600};
  uint32_t maxFramesInFlight{2};
  uint32_t maxSceneObjects{16};
  vk::DeviceSize maxVertexArenaSize{4 * 1024 * 1024};
  vk::DeviceSize maxIndexArenaSize{2 * 1024 * 1024};
  bool enableValidationLayers{true};
  std::vector<const char*> validationLayers{"VK_LAYER_KHRONOS_validation"};
  std::vector<const char*> deviceExtensions{
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
      VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
      VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME};
};

inline AppConfig DefaultAppConfig() { return AppConfig{}; }

}  // namespace app

