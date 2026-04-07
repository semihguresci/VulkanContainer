#include "Container/utility/AllocationManager.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include "stb_image.h"

namespace utility::memory {

namespace {

void EnsureArenaCapacity(std::unique_ptr<BufferArena>& arena,
                         VulkanMemoryManager& memoryManager,
                         VkDeviceSize requiredSize,
                         VkBufferUsageFlags usage,
                         VmaMemoryUsage memoryUsage,
                         VmaAllocationCreateFlags allocationFlags) {
  const VkDeviceSize safeRequiredSize = std::max<VkDeviceSize>(1, requiredSize);

  if (!arena || arena->totalSize() < safeRequiredSize) {
    VkDeviceSize requestedSize = safeRequiredSize;
    if (arena) {
      requestedSize = std::max(safeRequiredSize, arena->totalSize() * 2);
    }
    arena = std::make_unique<BufferArena>(memoryManager, requestedSize, usage,
                                          memoryUsage, allocationFlags);
  }

  arena->reset();
}

}  // namespace

AllocationManager::~AllocationManager() { cleanup(); }

void AllocationManager::initialize(VkInstance instance,
                                   VkPhysicalDevice physicalDevice,
                                   VkDevice device, VkQueue graphicsQueue,
                                   VkCommandPool commandPool,
                                   const app::AppConfig& config) {
  instance_ = instance;
  physicalDevice_ = physicalDevice;
  device_ = device;
  graphicsQueue_ = graphicsQueue;
  commandPool_ = commandPool;
  config_ = config;

  memoryManager_ = std::make_unique<VulkanMemoryManager>(
      instance_, physicalDevice_, device_);
}

void AllocationManager::cleanup() {
  resetTextureAllocations();
  indexArena_.reset();
  vertexArena_.reset();
  memoryManager_.reset();

  instance_ = VK_NULL_HANDLE;
  physicalDevice_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  graphicsQueue_ = VK_NULL_HANDLE;
  commandPool_ = VK_NULL_HANDLE;
}

BufferSlice AllocationManager::uploadVertices(
    boost::span<const geometry::Vertex> vertices) {
  VkDeviceSize bufferSize = sizeof(geometry::Vertex) * vertices.size();

  EnsureArenaCapacity(
      vertexArena_, *memoryManager_, bufferSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

  StagingBuffer stagingBuffer(*memoryManager_, bufferSize);
  stagingBuffer.upload({reinterpret_cast<const std::byte*>(vertices.data()),
                        static_cast<size_t>(bufferSize)});

  BufferSlice slice =
      vertexArena_->allocate(bufferSize, alignof(geometry::Vertex));

  copyBuffer(stagingBuffer.buffer().buffer, slice.buffer, bufferSize, 0,
             slice.offset);

  return slice;
}

BufferSlice AllocationManager::uploadIndices(
    boost::span<const uint32_t> indices) {
  VkDeviceSize bufferSize = sizeof(uint32_t) * indices.size();

  EnsureArenaCapacity(
      indexArena_, *memoryManager_, bufferSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

  StagingBuffer stagingBuffer(*memoryManager_, bufferSize);
  stagingBuffer.upload({reinterpret_cast<const std::byte*>(indices.data()),
                        static_cast<size_t>(bufferSize)});

  VkDeviceSize alignment = std::max<VkDeviceSize>(sizeof(uint32_t), 4);
  BufferSlice slice = indexArena_->allocate(bufferSize, alignment);

  copyBuffer(stagingBuffer.buffer().buffer, slice.buffer, bufferSize, 0,
             slice.offset);

  return slice;
}

AllocatedBuffer AllocationManager::createBuffer(
    VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage,
    VmaAllocationCreateFlags allocationFlags, VkSharingMode sharingMode) {
  return memoryManager_->createBuffer(size, usage, memoryUsage, allocationFlags,
                                      sharingMode);
}

void AllocationManager::destroyBuffer(AllocatedBuffer& buffer) {
  if (memoryManager_) {
    memoryManager_->destroyBuffer(buffer);
  }
}

utility::material::TextureResource AllocationManager::createTextureFromFile(
    const std::string& texturePath) {
  int texWidth, texHeight, texChannels;
  stbi_uc* pixels = stbi_load(texturePath.c_str(), &texWidth, &texHeight,
                              &texChannels, STBI_rgb_alpha);

  if (!pixels) {
    throw std::runtime_error("failed to load texture: " + texturePath);
  }

  VkDeviceSize imageSize = static_cast<VkDeviceSize>(texWidth) *
                           static_cast<VkDeviceSize>(texHeight) * 4;

  StagingBuffer stagingBuffer(*memoryManager_, imageSize);
  stagingBuffer.upload({reinterpret_cast<const std::byte*>(pixels),
                        static_cast<size_t>(imageSize)});

  stbi_image_free(pixels);

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
  imageInfo.extent = {static_cast<uint32_t>(texWidth),
                      static_cast<uint32_t>(texHeight), 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  VkImage image;
  VmaAllocation allocation;

  if (vmaCreateImage(memoryManager_->allocator(), &imageInfo, &allocInfo,
                     &image, &allocation, nullptr) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture image");
  }

  transitionImageLayout(image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  copyBufferToImage(stagingBuffer.buffer().buffer, image, texWidth, texHeight);

  transitionImageLayout(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  VkImageView imageView = createImageView(image, imageInfo.format);

  textureAllocations_.push_back({image, allocation});
  textureImages_.push_back(image);
  textureImageViews_.push_back(imageView);

  utility::material::TextureResource resource{};
  resource.name =
      std::filesystem::path(texturePath).lexically_normal().string();
  resource.image = image;
  resource.imageView = imageView;

  return resource;
}

void AllocationManager::resetTextureAllocations() {
  for (auto& alloc : textureAllocations_) {
    vmaFreeMemory(memoryManager_->allocator(), alloc.allocation);
  }

  for (VkImageView view : textureImageViews_) {
    vkDestroyImageView(device_, view, nullptr);
  }

  for (VkImage image : textureImages_) {
    vkDestroyImage(device_, image, nullptr);
  }

  textureAllocations_.clear();
  textureImages_.clear();
  textureImageViews_.clear();
}

/* ---------- Command helpers ---------- */

VkCommandBuffer AllocationManager::beginSingleTimeCommands() {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.commandPool = commandPool_;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer;
  vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  vkBeginCommandBuffer(commandBuffer, &beginInfo);
  return commandBuffer;
}

void AllocationManager::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
  vkEndCommandBuffer(commandBuffer);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;

  vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
  vkQueueWaitIdle(graphicsQueue_);

  vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
}

void AllocationManager::copyBuffer(VkBuffer src, VkBuffer dst,
                                   VkDeviceSize size, VkDeviceSize srcOffset,
                                   VkDeviceSize dstOffset) {
  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkBufferCopy region{};
  region.srcOffset = srcOffset;
  region.dstOffset = dstOffset;
  region.size = size;

  vkCmdCopyBuffer(cmd, src, dst, 1, &region);

  endSingleTimeCommands(cmd);
}

void AllocationManager::transitionImageLayout(VkImage image,
                                              VkImageLayout oldLayout,
                                              VkImageLayout newLayout) {
  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  VkPipelineStageFlags srcStage;
  VkPipelineStageFlags dstStage;

  if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
      newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  }

  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);

  endSingleTimeCommands(cmd);
}

void AllocationManager::copyBufferToImage(VkBuffer buffer, VkImage image,
                                          uint32_t width, uint32_t height) {
  VkCommandBuffer cmd = beginSingleTimeCommands();

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {width, height, 1};

  vkCmdCopyBufferToImage(cmd, buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  endSingleTimeCommands(cmd);
}

VkImageView AllocationManager::createImageView(VkImage image, VkFormat format) {
  VkImageViewCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  info.image = image;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = format;
  info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  info.subresourceRange.levelCount = 1;
  info.subresourceRange.layerCount = 1;

  VkImageView view;
  vkCreateImageView(device_, &info, nullptr, &view);
  return view;
}

}  // namespace utility::memory
