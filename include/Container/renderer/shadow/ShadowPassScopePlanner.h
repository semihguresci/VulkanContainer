#pragma once

#include "Container/common/CommonVulkan.h"

#include <array>

namespace container::renderer {

struct ShadowPassScopePlan {
  VkRect2D renderArea{};
  std::array<VkClearValue, 1> clearValues{};
  VkSubpassContents contents{VK_SUBPASS_CONTENTS_INLINE};
  bool executeSecondary{false};
};

[[nodiscard]] ShadowPassScopePlan
buildShadowPassScopePlan(bool useSecondaryCommandBuffer);

} // namespace container::renderer
