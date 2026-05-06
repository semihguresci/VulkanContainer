#include "Container/renderer/deferred/DeferredRasterImageBarrier.h"

namespace container::renderer {

bool recordDeferredRasterImageBarrierSteps(
    VkCommandBuffer cmd,
    std::span<const DeferredRasterImageBarrierStep> steps) {
  if (cmd == VK_NULL_HANDLE || steps.empty()) {
    return false;
  }

  bool recorded = false;
  for (const DeferredRasterImageBarrierStep &step : steps) {
    if (step.barrier.image == VK_NULL_HANDLE) {
      continue;
    }
    vkCmdPipelineBarrier(cmd, step.srcStageMask, step.dstStageMask, 0, 0,
                         nullptr, 0, nullptr, 1, &step.barrier);
    recorded = true;
  }
  return recorded;
}

} // namespace container::renderer
