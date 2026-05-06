#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/deferred/DeferredRasterImageBarrier.h"

namespace container::renderer {

struct DeferredRasterHiZDepthTransitionInputs {
  VkImage depthStencilImage{VK_NULL_HANDLE};
};

struct DeferredRasterHiZDepthTransitionPlan {
  bool active{false};
  DeferredRasterImageBarrierStep depthToSampling{};
  DeferredRasterImageBarrierStep depthToAttachment{};
};

[[nodiscard]] DeferredRasterHiZDepthTransitionPlan
buildDeferredRasterHiZDepthTransitionPlan(
    const DeferredRasterHiZDepthTransitionInputs &inputs);

[[nodiscard]] bool recordDeferredRasterHiZDepthToSamplingTransitionCommands(
    VkCommandBuffer cmd,
    const DeferredRasterHiZDepthTransitionPlan &plan);

[[nodiscard]] bool recordDeferredRasterHiZDepthToAttachmentTransitionCommands(
    VkCommandBuffer cmd,
    const DeferredRasterHiZDepthTransitionPlan &plan);

} // namespace container::renderer
