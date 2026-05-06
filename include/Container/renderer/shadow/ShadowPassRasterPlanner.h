#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/shadow/ShadowPassScopePlanner.h"

namespace container::renderer {

struct ShadowPassRasterPlanInputs {
  bool shadowAtlasVisible{false};
  bool shadowPassRecordable{false};
  bool useSecondaryCommandBuffer{false};
  VkCommandBuffer secondaryCommandBuffer{VK_NULL_HANDLE};
};

struct ShadowPassRasterPlan {
  bool active{false};
  ShadowPassScopePlan scope{};
  VkCommandBuffer secondaryCommandBuffer{VK_NULL_HANDLE};
};

[[nodiscard]] ShadowPassRasterPlan
buildShadowPassRasterPlan(const ShadowPassRasterPlanInputs &inputs);

} // namespace container::renderer
