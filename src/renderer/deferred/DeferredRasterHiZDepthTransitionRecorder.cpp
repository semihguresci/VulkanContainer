#include "Container/renderer/deferred/DeferredRasterHiZDepthTransitionRecorder.h"

#include <span>

namespace container::renderer {

namespace {

constexpr VkImageAspectFlags kDepthStencilAspects =
    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
constexpr VkPipelineStageFlags kDepthStages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

VkImageMemoryBarrier makeHiZDepthBarrier(VkImage image,
                                         VkImageLayout oldLayout,
                                         VkImageLayout newLayout,
                                         VkAccessFlags srcAccess,
                                         VkAccessFlags dstAccess) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = srcAccess;
  barrier.dstAccessMask = dstAccess;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {kDepthStencilAspects, 0, 1, 0, 1};
  return barrier;
}

bool recordSingleHiZDepthTransition(
    VkCommandBuffer cmd,
    const DeferredRasterHiZDepthTransitionPlan &plan,
    const DeferredRasterImageBarrierStep &step) {
  if (!plan.active || step.barrier.image == VK_NULL_HANDLE) {
    return false;
  }
  return recordDeferredRasterImageBarrierSteps(
      cmd, std::span<const DeferredRasterImageBarrierStep>(&step, 1u));
}

} // namespace

DeferredRasterHiZDepthTransitionPlan buildDeferredRasterHiZDepthTransitionPlan(
    const DeferredRasterHiZDepthTransitionInputs &inputs) {
  if (inputs.depthStencilImage == VK_NULL_HANDLE) {
    return {};
  }

  DeferredRasterHiZDepthTransitionPlan plan{};
  plan.active = true;
  plan.depthToSampling = {
      .srcStageMask = kDepthStages,
      .dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      .barrier = makeHiZDepthBarrier(
          inputs.depthStencilImage,
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
          VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL,
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT)};
  plan.depthToAttachment = {
      .srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      .dstStageMask = kDepthStages,
      .barrier = makeHiZDepthBarrier(
          inputs.depthStencilImage,
          VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL,
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
          VK_ACCESS_SHADER_READ_BIT,
          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)};
  return plan;
}

bool recordDeferredRasterHiZDepthToSamplingTransitionCommands(
    VkCommandBuffer cmd,
    const DeferredRasterHiZDepthTransitionPlan &plan) {
  return recordSingleHiZDepthTransition(cmd, plan, plan.depthToSampling);
}

bool recordDeferredRasterHiZDepthToAttachmentTransitionCommands(
    VkCommandBuffer cmd,
    const DeferredRasterHiZDepthTransitionPlan &plan) {
  return recordSingleHiZDepthTransition(cmd, plan, plan.depthToAttachment);
}

} // namespace container::renderer
