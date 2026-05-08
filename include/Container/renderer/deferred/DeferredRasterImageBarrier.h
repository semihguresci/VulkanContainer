#pragma once

#include "Container/common/CommonVulkan.h"

#include <span>

namespace container::renderer {

struct DeferredRasterImageBarrierStep {
  VkPipelineStageFlags srcStageMask{0};
  VkPipelineStageFlags dstStageMask{0};
  VkImageMemoryBarrier barrier{};
};

[[nodiscard]] bool recordDeferredRasterImageBarrierSteps(
    VkCommandBuffer cmd,
    std::span<const DeferredRasterImageBarrierStep> steps);

} // namespace container::renderer
