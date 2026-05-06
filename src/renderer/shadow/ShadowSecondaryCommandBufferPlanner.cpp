#include "Container/renderer/shadow/ShadowSecondaryCommandBufferPlanner.h"

namespace container::renderer {

ShadowSecondaryCommandBufferPlan buildShadowSecondaryCommandBufferPlan(
    const ShadowSecondaryCommandBufferPlanInputs &inputs) {
  ShadowSecondaryCommandBufferPlan plan{};
  plan.useSecondaryCommandBuffer =
      !inputs.usesGpuFilteredBimMeshShadowPath &&
      inputs.secondaryCommandBuffersEnabled && inputs.shadowPassRecordable &&
      inputs.secondaryCommandBufferAvailable &&
      inputs.cpuCommandCount >= kMinShadowSecondaryCommandBufferCpuCommands;
  return plan;
}

} // namespace container::renderer
