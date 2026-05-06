#pragma once

#include "Container/common/CommonVulkan.h"

#include <array>

namespace container::renderer {

struct DeferredLightingPassPlan {
  VkRect2D renderArea{};
  std::array<VkClearValue, 2> clearValues{};
  VkClearAttachment selectionStencilClearAttachment{};
  VkClearRect selectionStencilClearRect{};
};

[[nodiscard]] DeferredLightingPassPlan
buildDeferredLightingPassPlan(VkExtent2D framebufferExtent);

} // namespace container::renderer
