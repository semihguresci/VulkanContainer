#include "Container/renderer/CommandBufferManager.h"

#include <stdexcept>

namespace container::renderer {

CommandBufferManager::CommandBufferManager(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    uint32_t graphicsQueueFamily)
    : device_(std::move(device)) {
  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = graphicsQueueFamily;

  if (vkCreateCommandPool(device_->device(), &poolInfo, nullptr, &pool_) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to create command pool!");
  }
}

CommandBufferManager::~CommandBufferManager() {
  if (pool_ != VK_NULL_HANDLE) {
    // Freeing all buffers before destroying the pool is optional (pool
    // destruction implicitly frees them), but keeps the sequence explicit.
    if (!buffers_.empty()) {
      vkFreeCommandBuffers(device_->device(), pool_,
                           static_cast<uint32_t>(buffers_.size()),
                           buffers_.data());
      buffers_.clear();
    }
    vkDestroyCommandPool(device_->device(), pool_, nullptr);
    pool_ = VK_NULL_HANDLE;
  }
}

void CommandBufferManager::allocate(size_t imageCount) {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool        = pool_;
  allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = static_cast<uint32_t>(imageCount);

  buffers_.resize(imageCount);
  if (vkAllocateCommandBuffers(device_->device(), &allocInfo,
                               buffers_.data()) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate command buffers!");
  }
}

void CommandBufferManager::free() {
  if (!buffers_.empty()) {
    vkFreeCommandBuffers(device_->device(), pool_,
                         static_cast<uint32_t>(buffers_.size()),
                         buffers_.data());
    buffers_.clear();
  }
}

void CommandBufferManager::reallocate(size_t imageCount) {
  free();
  allocate(imageCount);
}

}  // namespace container::renderer
