#include "Container/renderer/shadow/ShadowCascadePreparationPlanner.h"

namespace container::renderer {

ShadowCascadePreparationPlan buildShadowCascadePreparationPlan(
    const ShadowCascadePreparationPlanInputs &inputs) {
  ShadowCascadePreparationPlan plan{};
  if (!inputs.shadowAtlasVisible) {
    return plan;
  }

  for (const ShadowCascadePreparationCascadeInputs &cascade :
       inputs.cascades) {
    if (!cascade.shadowPassActive || !cascade.shadowPassRecordable) {
      continue;
    }

    if (inputs.hasSceneWindingFlippedDraws ||
        inputs.hasSceneDoubleSidedDraws) {
      plan.prepareDrawCommands = true;
      return plan;
    }

    if (inputs.hasSceneSingleSidedDraws &&
        !cascade.sceneSingleSidedUsesGpuCull) {
      plan.prepareDrawCommands = true;
      return plan;
    }

    if (inputs.hasBimShadowGeometry) {
      plan.prepareDrawCommands = true;
      return plan;
    }
  }
  return plan;
}

} // namespace container::renderer
