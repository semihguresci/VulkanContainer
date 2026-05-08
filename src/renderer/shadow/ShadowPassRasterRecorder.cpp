#include "Container/renderer/shadow/ShadowPassRasterRecorder.h"

#include "Container/renderer/core/RenderPassScopeRecorder.h"

#include <span>

namespace container::renderer {

bool recordShadowPassRasterCommands(
    VkCommandBuffer cmd, const ShadowPassRasterRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE || inputs.plan == nullptr ||
      !inputs.plan->active || inputs.renderPass == VK_NULL_HANDLE ||
      inputs.framebuffer == VK_NULL_HANDLE) {
    return false;
  }

  const ShadowPassRasterPlan &plan = *inputs.plan;
  if (plan.scope.executeSecondary &&
      plan.secondaryCommandBuffer == VK_NULL_HANDLE) {
    return false;
  }
  if (!plan.scope.executeSecondary && !inputs.recordBody) {
    return false;
  }

  if (!recordRenderPassBeginCommands(
          cmd, {.renderPass = inputs.renderPass,
                .framebuffer = inputs.framebuffer,
                .renderArea = plan.scope.renderArea,
                .clearValues = std::span{plan.scope.clearValues},
                .contents = plan.scope.contents})) {
    return false;
  }

  if (plan.scope.executeSecondary) {
    const VkCommandBuffer secondary = plan.secondaryCommandBuffer;
    const bool executed = recordRenderPassExecuteSecondaryCommands(
        cmd, std::span<const VkCommandBuffer>(&secondary, 1u));
    const bool ended = recordRenderPassEndCommands(cmd);
    return executed && ended;
  }

  inputs.recordBody(cmd);
  return recordRenderPassEndCommands(cmd);
}

} // namespace container::renderer
