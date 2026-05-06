#include "Container/renderer/bim/BimFrameGpuVisibilityRecorder.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimDrawCompactionPlanner.h"

namespace container::renderer {

void prepareBimFrameGpuVisibility(BimManager *manager) {
  if (manager == nullptr) {
    return;
  }

  const auto compactionPlan =
      buildBimDrawCompactionPlan(makeBimDrawCompactionPlanInputs(*manager));
  for (const BimDrawCompactionPlanSource &source : compactionPlan) {
    manager->prepareDrawCompaction(source.slot, *source.commands);
  }
}

bool recordBimFrameGpuVisibilityCommands(
    const BimFrameGpuVisibilityRecordInputs &inputs) {
  if (inputs.manager == nullptr ||
      inputs.commandBuffer == VK_NULL_HANDLE) {
    return false;
  }

  inputs.manager->recordMeshletResidencyUpdate(
      inputs.commandBuffer, inputs.cameraBuffer, inputs.cameraBufferSize,
      inputs.objectBuffer, inputs.objectBufferSize);
  inputs.manager->recordVisibilityFilterUpdate(inputs.commandBuffer);
  inputs.manager->recordDrawCompactionUpdate(inputs.commandBuffer);
  return true;
}

} // namespace container::renderer
