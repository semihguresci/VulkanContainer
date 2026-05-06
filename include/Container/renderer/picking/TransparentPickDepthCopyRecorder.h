#pragma once

#include "Container/common/CommonVulkan.h"

#include <array>

namespace container::renderer {

struct TransparentPickDepthCopyInputs {
  VkImage sourceDepthStencilImage{VK_NULL_HANDLE};
  VkImage pickDepthImage{VK_NULL_HANDLE};
  VkExtent2D extent{};
};

struct TransparentPickDepthCopyPlan {
  bool active{false};
  VkImage sourceDepthStencilImage{VK_NULL_HANDLE};
  VkImage pickDepthImage{VK_NULL_HANDLE};
  VkPipelineStageFlags depthStages{0};
  VkPipelineStageFlags transferStage{0};
  std::array<VkImageMemoryBarrier, 2> toTransfer{};
  VkImageCopy depthCopy{};
  std::array<VkImageMemoryBarrier, 2> toAttachment{};
};

[[nodiscard]] TransparentPickDepthCopyPlan
buildTransparentPickDepthCopyPlan(
    const TransparentPickDepthCopyInputs &inputs);

[[nodiscard]] bool recordTransparentPickDepthCopyCommands(
    VkCommandBuffer cmd, const TransparentPickDepthCopyPlan &plan);

} // namespace container::renderer
