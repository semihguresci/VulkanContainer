#include "Container/renderer/deferred/DeferredRasterSceneColorReadBarrierRecorder.h"

namespace container::renderer {

DeferredRasterSceneColorReadBarrierPlan
buildDeferredRasterSceneColorReadBarrierPlan(
    const DeferredRasterSceneColorReadBarrierInputs &inputs) {
  if (inputs.sceneColorImage == VK_NULL_HANDLE) {
    return {};
  }

  DeferredRasterSceneColorReadBarrierPlan plan{};
  plan.active = true;
  plan.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  plan.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  plan.barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  plan.barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  plan.barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  plan.barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  plan.barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  plan.barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  plan.barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  plan.barrier.image = inputs.sceneColorImage;
  plan.barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return plan;
}

bool recordDeferredRasterSceneColorReadBarrierCommands(
    VkCommandBuffer cmd,
    const DeferredRasterSceneColorReadBarrierPlan &plan) {
  if (cmd == VK_NULL_HANDLE || !plan.active ||
      plan.barrier.image == VK_NULL_HANDLE) {
    return false;
  }

  vkCmdPipelineBarrier(cmd, plan.srcStageMask, plan.dstStageMask, 0, 0,
                       nullptr, 0, nullptr, 1, &plan.barrier);
  return true;
}

} // namespace container::renderer
