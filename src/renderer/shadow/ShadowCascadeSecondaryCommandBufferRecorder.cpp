#include "Container/renderer/shadow/ShadowCascadeSecondaryCommandBufferRecorder.h"

#include "Container/renderer/core/CommandBufferScopeRecorder.h"

#include <future>
#include <stdexcept>
#include <vector>

namespace container::renderer {

ShadowCascadeSecondaryCommandBufferRecordPlan
buildShadowCascadeSecondaryCommandBufferRecordPlan(
    const ShadowCascadeSecondaryCommandBufferPlanInputs &inputs) {
  ShadowCascadeSecondaryCommandBufferRecordPlan plan{};
  if (!inputs.secondaryCommandBuffersEnabled) {
    return plan;
  }

  for (uint32_t cascadeIndex = 0u;
       cascadeIndex < container::gpu::kShadowCascadeCount; ++cascadeIndex) {
    if (!inputs.cascadePassActive[cascadeIndex] ||
        !inputs.useSecondaryCommandBuffer[cascadeIndex] ||
        inputs.commandBuffers[cascadeIndex] == VK_NULL_HANDLE) {
      continue;
    }

    plan.cascadeIndices[plan.cascadeCount] = cascadeIndex;
    plan.commandBuffers[plan.cascadeCount] =
        inputs.commandBuffers[cascadeIndex];
    ++plan.cascadeCount;
  }
  return plan;
}

void recordShadowCascadeSecondaryCommandBufferCommands(
    const ShadowCascadeSecondaryCommandBufferCommands &commands) {
  if (!recordCommandBufferResetCommands(commands.commandBuffer)) {
    throw std::runtime_error(
        "failed to reset shadow secondary command buffer!");
  }

  if (!recordSecondaryCommandBufferBeginCommands(
          commands.commandBuffer,
          {.renderPass = commands.renderPass,
           .framebuffer = commands.framebuffer})) {
    throw std::runtime_error(
        "failed to begin shadow secondary command buffer!");
  }

  if (commands.recordBody) {
    commands.recordBody(commands.commandBuffer);
  }

  if (!recordCommandBufferEndCommands(commands.commandBuffer)) {
    throw std::runtime_error(
        "failed to record shadow secondary command buffer!");
  }
}

void recordShadowCascadeSecondaryCommandBufferPlan(
    const ShadowCascadeSecondaryCommandBufferRecordPlan &plan,
    const ShadowCascadeSecondaryCommandBufferRecordCallback &recordCascade) {
  if (plan.empty() || !recordCascade) {
    return;
  }

  std::vector<std::future<void>> workers;
  workers.reserve(plan.cascadeCount);

  for (uint32_t planIndex = 0u; planIndex < plan.cascadeCount; ++planIndex) {
    const VkCommandBuffer commandBuffer = plan.commandBuffers[planIndex];
    const uint32_t cascadeIndex = plan.cascadeIndices[planIndex];
    ShadowCascadeSecondaryCommandBufferRecordCallback recordCascadeCopy =
        recordCascade;
    workers.emplace_back(std::async(
        std::launch::async,
        [recordCascadeCopy, commandBuffer, cascadeIndex]() {
          recordCascadeCopy(commandBuffer, cascadeIndex);
        }));
  }

  for (auto &worker : workers) {
    worker.get();
  }
}

} // namespace container::renderer
