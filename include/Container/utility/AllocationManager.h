#pragma once

#include <Container/app/AppConfig.h>
#include <Container/geometry/Vertex.h>
#include <Container/utility/MaterialManager.h>
#include <Container/utility/VulkanMemoryManager.h>

#include <boost/core/span.hpp>
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace utility::memory {

struct TextureAllocation {
  VkImage image{VK_NULL_HANDLE};
  VmaAllocation allocation{VK_NULL_HANDLE};
};

class AllocationManager {
 public:
  AllocationManager() = default;
  ~AllocationManager();

  void initialize(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                  VkQueue graphicsQueue, VkCommandPool commandPool,
                  const app::AppConfig& config);
  void cleanup();

  BufferSlice uploadVertices(boost::span<const geometry::Vertex> vertices);
  BufferSlice uploadIndices(boost::span<const uint32_t> indices);

  AllocatedBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                               VmaMemoryUsage memoryUsage,
                               VmaAllocationCreateFlags allocationFlags = 0,
                               VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE);
  void destroyBuffer(AllocatedBuffer& buffer);

  utility::material::TextureResource createTextureFromFile(const std::string& texturePath);
  void resetTextureAllocations();

  VulkanMemoryManager* memoryManager() const { return memoryManager_.get(); }

 private:
  vk::CommandBuffer beginSingleTimeCommands();
  void endSingleTimeCommands(vk::CommandBuffer commandBuffer);
  void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size,
                  VkDeviceSize srcOffset = 0, VkDeviceSize dstOffset = 0);
  void transitionImageLayout(VkImage image, VkImageLayout oldLayout,
                             VkImageLayout newLayout);
  void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                         uint32_t height);
  VkImageView createImageView(VkImage image, VkFormat format);

  VkInstance instance_{VK_NULL_HANDLE};
  VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue graphicsQueue_{VK_NULL_HANDLE};
  VkCommandPool commandPool_{VK_NULL_HANDLE};
  app::AppConfig config_{};

  std::unique_ptr<VulkanMemoryManager> memoryManager_;
  std::unique_ptr<BufferArena> vertexArena_;
  std::unique_ptr<BufferArena> indexArena_;

  std::vector<TextureAllocation> textureAllocations_{};
  std::vector<VkImage> textureImages_{};
  std::vector<VkImageView> textureImageViews_{};
};

}  // namespace utility::memory
