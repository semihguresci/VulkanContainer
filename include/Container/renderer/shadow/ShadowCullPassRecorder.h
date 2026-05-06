#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/shadow/ShadowCullPassPlanner.h"

#include <cstdint>

namespace container::renderer {

class ShadowCullManager;

struct ShadowCullPassRecordInputs {
  ShadowCullManager *shadowCullManager{nullptr};
  ShadowCullPassPlan plan{};
  uint32_t imageIndex{0u};
  uint32_t cascadeIndex{0u};
};

[[nodiscard]] bool recordShadowCullPassCommands(
    VkCommandBuffer cmd, const ShadowCullPassRecordInputs &inputs);

} // namespace container::renderer
