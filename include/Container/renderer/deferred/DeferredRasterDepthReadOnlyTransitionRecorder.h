#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/deferred/DeferredRasterImageBarrier.h"

#include <array>

namespace container::renderer {

struct DeferredRasterDepthReadOnlyTransitionInputs {
  VkImage depthStencilImage{VK_NULL_HANDLE};
  VkImage shadowAtlasImage{VK_NULL_HANDLE};
  bool shadowAtlasVisible{false};
  uint32_t shadowCascadeCount{0u};
};

struct DeferredRasterDepthReadOnlyTransitionPlan {
  std::array<DeferredRasterImageBarrierStep, 2> steps{};
  uint32_t stepCount{0u};
};

[[nodiscard]] DeferredRasterDepthReadOnlyTransitionPlan
buildDeferredRasterDepthReadOnlyTransitionPlan(
    const DeferredRasterDepthReadOnlyTransitionInputs &inputs);

[[nodiscard]] bool recordDeferredRasterDepthReadOnlyTransitionCommands(
    VkCommandBuffer cmd,
    const DeferredRasterDepthReadOnlyTransitionPlan &plan);

} // namespace container::renderer
