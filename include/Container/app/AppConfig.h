#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Container/common/CommonVulkan.h"

namespace container::app {

// Sentinel used by SceneManager to build the local diagnostic scene from
// multiple generated/sample assets instead of loading a single glTF file.
inline constexpr std::string_view kDefaultSceneModelToken = "__default_test_scene__";
inline constexpr std::array<std::string_view, 3> kDefaultSceneModelRelativePaths = {{
    "models/glTF-Sample-Models/2.0/Triangle/glTF/Triangle.gltf",
    "models/glTF-Sample-Models/2.0/Cube/glTF/Cube.gltf",
    "__procedural_uv_sphere__",
}};

inline constexpr std::string_view kDefaultEnvironmentHdrRelativePath =
    "hdr/citrus_orchard_road_puresky_4k.exr";

// Runtime default for normal application startup. Tests can still exercise the
// diagnostic scene through kDefaultSceneModelToken without changing this path.
inline constexpr std::string_view kDefaultModelRelativePath =
    "models/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf";

struct AppConfig {
  uint32_t windowWidth{800};
  uint32_t windowHeight{600};
  uint32_t maxFramesInFlight{2};
  // Upper bound for the per-object SSBO. This is scene capacity, not a draw-call budget.
  uint32_t maxSceneObjects{4096};
  bool enableValidationLayers{true};
  std::string modelPath{std::string(kDefaultModelRelativePath)};
  std::vector<const char*> validationLayers{"VK_LAYER_KHRONOS_validation"};
  std::vector<const char*> deviceExtensions{
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
      VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
      VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME};
};

[[nodiscard]] inline AppConfig DefaultAppConfig() { return AppConfig{}; }

}  // namespace container::app
