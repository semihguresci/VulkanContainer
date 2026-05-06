#pragma once

#include "Container/common/CommonVulkan.h"

namespace container::renderer {

struct DeferredRasterSceneColorReadBarrierInputs {
  VkImage sceneColorImage{VK_NULL_HANDLE};
};

struct DeferredRasterSceneColorReadBarrierPlan {
  bool active{false};
  VkPipelineStageFlags srcStageMask{0};
  VkPipelineStageFlags dstStageMask{0};
  VkImageMemoryBarrier barrier{};
};

[[nodiscard]] DeferredRasterSceneColorReadBarrierPlan
buildDeferredRasterSceneColorReadBarrierPlan(
    const DeferredRasterSceneColorReadBarrierInputs &inputs);

[[nodiscard]] bool recordDeferredRasterSceneColorReadBarrierCommands(
    VkCommandBuffer cmd,
    const DeferredRasterSceneColorReadBarrierPlan &plan);

} // namespace container::renderer
