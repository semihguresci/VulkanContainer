#include "Container/renderer/picking/TransparentPickDepthCopyRecorder.h"

namespace container::renderer {

namespace {

constexpr VkImageAspectFlags kDepthStencilAspects =
    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
constexpr VkPipelineStageFlags kDepthStages =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
constexpr VkPipelineStageFlags kTransferStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

[[nodiscard]] bool hasDepthCopyWork(
    const TransparentPickDepthCopyInputs &inputs) {
  return inputs.sourceDepthStencilImage != VK_NULL_HANDLE &&
         inputs.pickDepthImage != VK_NULL_HANDLE && inputs.extent.width > 0u &&
         inputs.extent.height > 0u;
}

VkImageMemoryBarrier makeDepthBarrier(VkImage image, VkImageLayout oldLayout,
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

} // namespace

TransparentPickDepthCopyPlan buildTransparentPickDepthCopyPlan(
    const TransparentPickDepthCopyInputs &inputs) {
  if (!hasDepthCopyWork(inputs)) {
    return {};
  }

  TransparentPickDepthCopyPlan plan{};
  plan.active = true;
  plan.sourceDepthStencilImage = inputs.sourceDepthStencilImage;
  plan.pickDepthImage = inputs.pickDepthImage;
  plan.depthStages = kDepthStages;
  plan.transferStage = kTransferStage;
  plan.toTransfer = {
      makeDepthBarrier(inputs.sourceDepthStencilImage,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT),
      makeDepthBarrier(inputs.pickDepthImage,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT),
  };
  plan.depthCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  plan.depthCopy.srcSubresource.layerCount = 1;
  plan.depthCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  plan.depthCopy.dstSubresource.layerCount = 1;
  plan.depthCopy.extent = {inputs.extent.width, inputs.extent.height, 1u};
  plan.toAttachment = {
      makeDepthBarrier(inputs.sourceDepthStencilImage,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_ACCESS_TRANSFER_READ_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
      makeDepthBarrier(inputs.pickDepthImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
  };
  return plan;
}

bool recordTransparentPickDepthCopyCommands(
    VkCommandBuffer cmd, const TransparentPickDepthCopyPlan &plan) {
  if (cmd == VK_NULL_HANDLE || !plan.active ||
      plan.sourceDepthStencilImage == VK_NULL_HANDLE ||
      plan.pickDepthImage == VK_NULL_HANDLE) {
    return false;
  }

  vkCmdPipelineBarrier(
      cmd, plan.depthStages, plan.transferStage, 0, 0, nullptr, 0, nullptr,
      static_cast<uint32_t>(plan.toTransfer.size()), plan.toTransfer.data());
  vkCmdCopyImage(cmd, plan.sourceDepthStencilImage,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, plan.pickDepthImage,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &plan.depthCopy);
  vkCmdPipelineBarrier(
      cmd, plan.transferStage, plan.depthStages, 0, 0, nullptr, 0, nullptr,
      static_cast<uint32_t>(plan.toAttachment.size()),
      plan.toAttachment.data());
  return true;
}

} // namespace container::renderer
