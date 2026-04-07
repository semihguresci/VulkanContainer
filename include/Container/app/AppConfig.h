#pragma once

#include "Container/common/CommonVulkan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

struct AppConfig {
  uint32_t windowWidth{800};
  uint32_t windowHeight{600};
  uint32_t maxFramesInFlight{2};
  uint32_t maxSceneObjects{16};
  bool enableValidationLayers{true};
  std::string modelPath{
      "F:\\Projects\\Container\\out\\build\\windows-debug\\models\\glTF-Sample-Models\\2.0\\Sponza\\glTF\\Sponza.gltf"};
  std::vector<const char*> validationLayers{"VK_LAYER_KHRONOS_validation"};
  std::vector<const char*> deviceExtensions{
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
      VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
      VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME};
};

[[nodiscard]] inline AppConfig DefaultAppConfig() { return AppConfig{}; }

}  // namespace app

