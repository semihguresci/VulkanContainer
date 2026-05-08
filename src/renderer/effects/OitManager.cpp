#include "Container/renderer/effects/OitManager.h"

#include <array>

namespace container::renderer {

OitManager::OitManager(std::shared_ptr<container::gpu::VulkanDevice> device)
    : device_(std::move(device)) {}

void OitManager::clearResources(VkCommandBuffer cmd,
                                const OitFrameResources& resources,
                                uint32_t invalidNodeIndex) const {
  if (cmd == VK_NULL_HANDLE ||
      resources.headPointerImage == VK_NULL_HANDLE ||
      resources.counterBuffer == VK_NULL_HANDLE) {
    return;
  }

  VkImageMemoryBarrier headClear{};
  headClear.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  headClear.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
  headClear.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
  headClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  headClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  headClear.image               = resources.headPointerImage;
  headClear.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  headClear.srcAccessMask       = 0;
  headClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

  VkBufferMemoryBarrier counterClear{};
  counterClear.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  counterClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  counterClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  counterClear.buffer              = resources.counterBuffer;
  counterClear.offset              = 0;
  counterClear.size                = sizeof(uint32_t);
  counterClear.srcAccessMask       = 0;
  counterClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                       1, &counterClear, 1, &headClear);

  VkClearColorValue clearColor{};
  clearColor.uint32[0] = invalidNodeIndex;
  const VkImageSubresourceRange clearRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, resources.headPointerImage,
                       VK_IMAGE_LAYOUT_GENERAL, &clearColor, 1, &clearRange);
  vkCmdFillBuffer(cmd, resources.counterBuffer, 0, sizeof(uint32_t), 0u);

  VkImageMemoryBarrier headReady = headClear;
  headReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  headReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

  VkBufferMemoryBarrier counterReady = counterClear;
  counterReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  counterReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                       1, &counterReady, 1, &headReady);
}

void OitManager::prepareResolve(VkCommandBuffer cmd,
                                const OitFrameResources& resources) const {
  if (cmd == VK_NULL_HANDLE ||
      resources.headPointerImage == VK_NULL_HANDLE ||
      resources.nodeBuffer == VK_NULL_HANDLE ||
      resources.counterBuffer == VK_NULL_HANDLE) {
    return;
  }

  VkImageMemoryBarrier headBarrier{};
  headBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  headBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
  headBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
  headBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  headBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  headBarrier.image               = resources.headPointerImage;
  headBarrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  headBarrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
  headBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

  VkBufferMemoryBarrier nodeBarrier{};
  nodeBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  nodeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  nodeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  nodeBarrier.buffer              = resources.nodeBuffer;
  nodeBarrier.offset              = 0;
  nodeBarrier.size                = VK_WHOLE_SIZE;
  nodeBarrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
  nodeBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

  VkBufferMemoryBarrier counterBarrier{};
  counterBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  counterBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  counterBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  counterBarrier.buffer              = resources.counterBuffer;
  counterBarrier.offset              = 0;
  counterBarrier.size                = sizeof(uint32_t);
  counterBarrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
  counterBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;

  std::array<VkBufferMemoryBarrier, 2> bufBarriers = {nodeBarrier, counterBarrier};
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 0, nullptr,
                       static_cast<uint32_t>(bufBarriers.size()),
                        bufBarriers.data(), 1, &headBarrier);
}

}  // namespace container::renderer
