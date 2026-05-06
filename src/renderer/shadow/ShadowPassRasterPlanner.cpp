#include "Container/renderer/shadow/ShadowPassRasterPlanner.h"

namespace container::renderer {

ShadowPassRasterPlan
buildShadowPassRasterPlan(const ShadowPassRasterPlanInputs &inputs) {
  if (!inputs.shadowAtlasVisible || !inputs.shadowPassRecordable) {
    return {};
  }

  const bool executeSecondary = inputs.useSecondaryCommandBuffer &&
                                inputs.secondaryCommandBuffer != VK_NULL_HANDLE;
  return {.active = true,
          .scope = buildShadowPassScopePlan(executeSecondary),
          .secondaryCommandBuffer = executeSecondary
                                        ? inputs.secondaryCommandBuffer
                                        : VK_NULL_HANDLE};
}

} // namespace container::renderer
