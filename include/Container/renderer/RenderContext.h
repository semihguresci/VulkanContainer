#pragma once

#include "Container/common/CommonVulkan.h"

#include <cstdint>
#include <memory>

namespace container::gpu {
class VulkanDevice;
}

namespace container::renderer {

// Lightweight value object bundling the immutable (per-frame) Vulkan state
// that many renderer subsystems need.  Built once per frame by
// RendererFrontend and passed by const-ref — no ownership transfer.
struct RenderContext {
  std::shared_ptr<container::gpu::VulkanDevice> device;
  VkInstance        instance{VK_NULL_HANDLE};
  VkExtent2D        extent{};
  VkFormat          swapChainFormat{VK_FORMAT_UNDEFINED};
  VkFormat          depthFormat{VK_FORMAT_UNDEFINED};
  uint32_t          currentFrame{0};
  uint32_t          imageIndex{0};
  bool              wireframeRasterModeSupported{false};
  bool              wireframeWideLinesSupported{false};
  bool              wireframeSupported{false};
};

}  // namespace container::renderer
