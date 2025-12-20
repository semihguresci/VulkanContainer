#include <Container/utility/AllocationManager.h>

#include <stb_image.h>

#include <filesystem>
#include <stdexcept>

namespace utility::memory {

AllocationManager::~AllocationManager() { cleanup(); }

void AllocationManager::initialize(VkInstance instance, VkPhysicalDevice physicalDevice,
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

  vertexArena_ = std::make_unique<BufferArena>(
      *memoryManager_, config_.maxVertexArenaSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);

  indexArena_ = std::make_unique<BufferArena>(
      *memoryManager_, config_.maxIndexArenaSize,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
      VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT);
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

BufferSlice AllocationManager::uploadVertices(boost::span<const geometry::Vertex> vertices) {
  const VkDeviceSize bufferSize = sizeof(geometry::Vertex) * vertices.size();
  StagingBuffer stagingBuffer(*memoryManager_, bufferSize);
  stagingBuffer.upload({reinterpret_cast<const std::byte*>(vertices.data()),
                        static_cast<size_t>(bufferSize)});

  auto slice = vertexArena_->allocate(bufferSize, alignof(geometry::Vertex));
  copyBuffer(stagingBuffer.buffer().buffer, slice.buffer, bufferSize, 0, slice.offset);
  return slice;
}

BufferSlice AllocationManager::uploadIndices(boost::span<const uint32_t> indices) {
  const VkDeviceSize bufferSize = sizeof(uint32_t) * indices.size();
  StagingBuffer stagingBuffer(*memoryManager_, bufferSize);
  stagingBuffer.upload({reinterpret_cast<const std::byte*>(indices.data()),
                        static_cast<size_t>(bufferSize)});

  constexpr VkDeviceSize indexAlignment = std::max<VkDeviceSize>(sizeof(uint32_t), 4U);
  auto slice = indexArena_->allocate(bufferSize, indexAlignment);
  copyBuffer(stagingBuffer.buffer().buffer, slice.buffer, bufferSize, 0, slice.offset);
  return slice;
}

AllocatedBuffer AllocationManager::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                                VmaMemoryUsage memoryUsage,
                                                VmaAllocationCreateFlags allocationFlags,
                                                VkSharingMode sharingMode) {
  return memoryManager_->createBuffer(size, usage, memoryUsage, allocationFlags, sharingMode);
}

void AllocationManager::destroyBuffer(AllocatedBuffer& buffer) {
  if (memoryManager_) {
    memoryManager_->destroyBuffer(buffer);
  }
}

utility::material::TextureResource AllocationManager::createTextureFromFile(
    const std::string& texturePath) {
  int texWidth, texHeight, texChannels;
  stbi_uc* pixels = stbi_load(texturePath.c_str(), &texWidth, &texHeight, &texChannels,
                              STBI_rgb_alpha);
  if (!pixels) {
    throw std::runtime_error("failed to load texture image: " + texturePath);
  }

  const VkDeviceSize imageSize = static_cast<VkDeviceSize>(texWidth) *
                                 static_cast<VkDeviceSize>(texHeight) * 4;
  StagingBuffer stagingBuffer(*memoryManager_, imageSize);
  stagingBuffer.upload(
      {reinterpret_cast<const std::byte*>(pixels), static_cast<size_t>(imageSize)});
  stbi_image_free(pixels);

  vk::ImageCreateInfo imageInfo{};
  imageInfo.imageType = vk::ImageType::e2D;
  imageInfo.extent =
      vk::Extent3D{static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight), 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = vk::Format::eR8G8B8A8Srgb;
  imageInfo.tiling = vk::ImageTiling::eOptimal;
  imageInfo.initialLayout = vk::ImageLayout::eUndefined;
  imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
  imageInfo.sharingMode = vk::SharingMode::eExclusive;
  imageInfo.samples = vk::SampleCountFlagBits::e1;

  VmaAllocationCreateInfo allocInfo{};
  allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  VkImage textureImage{VK_NULL_HANDLE};
  VmaAllocation textureAllocation{VK_NULL_HANDLE};
  if (vmaCreateImage(memoryManager_->allocator(),
                     reinterpret_cast<const VkImageCreateInfo*>(&imageInfo),
                     &allocInfo, &textureImage, &textureAllocation, nullptr) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture image!");
  }

  transitionImageLayout(textureImage, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copyBufferToImage(stagingBuffer.buffer().buffer, textureImage,
                    static_cast<uint32_t>(texWidth),
                    static_cast<uint32_t>(texHeight));
  transitionImageLayout(textureImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  VkImageView imageView =
      createImageView(textureImage, static_cast<VkFormat>(imageInfo.format));

  textureAllocations_.push_back({textureImage, textureAllocation});
  textureImages_.push_back(textureImage);
  textureImageViews_.push_back(imageView);

  utility::material::TextureResource resource{};
  resource.image = textureImage;
  resource.imageView = imageView;
  resource.name = std::filesystem::path(texturePath).lexically_normal().string();
  return resource;
}

void AllocationManager::resetTextureAllocations() {
  for (const auto& allocation : textureAllocations_) {
    if (allocation.allocation != VK_NULL_HANDLE && memoryManager_) {
      vmaFreeMemory(memoryManager_->allocator(), allocation.allocation);
    }
  }
  for (auto image : textureImages_) {
    if (image != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
      vkDestroyImage(device_, image, nullptr);
    }
  }
  for (auto view : textureImageViews_) {
    if (view != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, view, nullptr);
    }
  }
  textureAllocations_.clear();
  textureImages_.clear();
  textureImageViews_.clear();
}

vk::CommandBuffer AllocationManager::beginSingleTimeCommands() {
  vk::CommandBufferAllocateInfo allocInfo{
      .level = vk::CommandBufferLevel::ePrimary,
      .commandPool = commandPool_,
      .commandBufferCount = 1,
  };

  vk::Device device{device_};
  auto commandBuffers = device.allocateCommandBuffers(allocInfo);

  vk::CommandBufferBeginInfo beginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
  };

  commandBuffers.front().begin(beginInfo);
  return commandBuffers.front();
}

void AllocationManager::endSingleTimeCommands(vk::CommandBuffer commandBuffer) {
  commandBuffer.end();

  vk::SubmitInfo submitInfo{
      .commandBufferCount = 1,
      .pCommandBuffers = &commandBuffer,
  };

  vk::Queue queue{graphicsQueue_};
  queue.submit(1, &submitInfo, {});
  queue.waitIdle();

  vk::Device device{device_};
  device.freeCommandBuffers(commandPool_, commandBuffer);
}

void AllocationManager::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size,
                                   VkDeviceSize srcOffset, VkDeviceSize dstOffset) {
  vk::CommandBufferAllocateInfo allocInfo{};
  allocInfo.level = vk::CommandBufferLevel::ePrimary;
  allocInfo.commandPool = commandPool_;
  allocInfo.commandBufferCount = 1;

  vk::Device device{device_};
  auto commandBuffers = device.allocateCommandBuffersUnique(allocInfo);
  auto& commandBuffer = commandBuffers.front();

  vk::CommandBufferBeginInfo beginInfo{
      .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
  };

  commandBuffer->begin(beginInfo);

  vk::BufferCopy copyRegion{
      .srcOffset = srcOffset,
      .dstOffset = dstOffset,
      .size = size,
  };
  commandBuffer->copyBuffer(srcBuffer, dstBuffer, 1, &copyRegion);

  commandBuffer->end();

  vk::CommandBuffer commandBufferHandle = commandBuffer.get();
  vk::SubmitInfo submitInfo{
      .commandBufferCount = 1,
      .pCommandBuffers = &commandBufferHandle,
  };

  vk::Queue queue{graphicsQueue_};
  queue.submit(1, &submitInfo, {});
  queue.waitIdle();
}

void AllocationManager::transitionImageLayout(VkImage image, VkImageLayout oldLayout,
                                              VkImageLayout newLayout) {
  vk::CommandBuffer commandBuffer = beginSingleTimeCommands();

  vk::ImageMemoryBarrier barrier{
      .oldLayout = static_cast<vk::ImageLayout>(oldLayout),
      .newLayout = static_cast<vk::ImageLayout>(newLayout),
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = vk::ImageSubresourceRange{
          .aspectMask = vk::ImageAspectFlagBits::eColor,
          .baseMipLevel = 0,
          .levelCount = 1,
          .baseArrayLayer = 0,
          .layerCount = 1,
      },
  };

  vk::PipelineStageFlags sourceStage;
  vk::PipelineStageFlags destinationStage;

  if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
      newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = {};
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;

    sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
    destinationStage = vk::PipelineStageFlagBits::eTransfer;
  } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;

    sourceStage = vk::PipelineStageFlagBits::eTransfer;
    destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
  } else {
    throw std::invalid_argument("unsupported layout transition!");
  }

  commandBuffer.pipelineBarrier(sourceStage, destinationStage, {}, 0, nullptr, 0,
                                nullptr, 1, &barrier);

  endSingleTimeCommands(commandBuffer);
}

void AllocationManager::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                                          uint32_t height) {
  vk::CommandBuffer commandBuffer = beginSingleTimeCommands();

  vk::BufferImageCopy region{
      .bufferOffset = 0,
      .bufferRowLength = 0,
      .bufferImageHeight = 0,
      .imageSubresource =
          vk::ImageSubresourceLayers{
              .aspectMask = vk::ImageAspectFlagBits::eColor,
              .mipLevel = 0,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
      .imageOffset = {0, 0, 0},
      .imageExtent = {width, height, 1},
  };

  commandBuffer.copyBufferToImage(buffer, image,
                                  vk::ImageLayout::eTransferDstOptimal, 1,
                                  &region);

  endSingleTimeCommands(commandBuffer);
}

VkImageView AllocationManager::createImageView(VkImage image, VkFormat format) {
  vk::ImageViewCreateInfo viewInfo{
      .image = image,
      .viewType = vk::ImageViewType::e2D,
      .format = static_cast<vk::Format>(format),
      .subresourceRange =
          vk::ImageSubresourceRange{
              .aspectMask = vk::ImageAspectFlagBits::eColor,
              .baseMipLevel = 0,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
          },
  };

  VkImageView imageView{VK_NULL_HANDLE};
  vk::Device device{device_};
  if (device.createImageView(&viewInfo, nullptr, &imageView) != vk::Result::eSuccess) {
    throw std::runtime_error("failed to create texture image view!");
  }

  return imageView;
}

}  // namespace utility::memory
