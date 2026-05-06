#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/core/RenderGraph.h"

namespace container::renderer {

struct DeferredRasterTileCullPlanInputs {
  bool tileCullDisplayMode{false};
  bool tiledLightingReady{false};
  bool frameAvailable{false};
  VkImageView depthSamplingView{VK_NULL_HANDLE};
  VkBuffer cameraBuffer{VK_NULL_HANDLE};
  VkDeviceSize cameraBufferSize{0};
  VkExtent2D screenExtent{};
  float cameraNear{0.0f};
  float cameraFar{0.0f};
};

struct DeferredRasterTileCullPlan {
  bool active{false};
  RenderPassReadiness readiness{};
  VkExtent2D screenExtent{};
  VkBuffer cameraBuffer{VK_NULL_HANDLE};
  VkDeviceSize cameraBufferSize{0};
  VkImageView depthSamplingView{VK_NULL_HANDLE};
  float cameraNear{0.0f};
  float cameraFar{0.0f};
};

[[nodiscard]] DeferredRasterTileCullPlan
buildDeferredRasterTileCullPlan(
    const DeferredRasterTileCullPlanInputs &inputs);

} // namespace container::renderer
