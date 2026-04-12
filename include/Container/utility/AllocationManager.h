#pragma once

#include "Container/app/AppConfig.h"
#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonVMA.h"
#include "Container/geometry/Vertex.h"
#include "Container/utility/MaterialManager.h"
#include "Container/utility/TextureResource.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace container::gpu {

struct TextureAllocation {
  VkImage image{VK_NULL_HANDLE};
  VmaAllocation allocation{nullptr};
};

class AllocationManager {
 public:
  AllocationManager() = default;
  ~AllocationManager();

  void initialize(VkInstance instance, VkPhysicalDevice physicalDevice,
                  VkDevice device, VkQueue graphicsQueue,
                  VkCommandPool commandPool, const app::AppConfig& config);

  void cleanup();

  BufferSlice uploadVertices(std::span<const geometry::Vertex> vertices);
  BufferSlice uploadIndices(std::span<const uint32_t> indices);

  [[nodiscard]] AllocatedBuffer createBuffer(
      VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage,
      VmaAllocationCreateFlags allocationFlags = 0,
      VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE);

  void destroyBuffer(AllocatedBuffer& buffer);

  container::material::TextureResource createTextureFromFile(
      const std::string& texturePath);

  void resetTextureAllocations();

  [[nodiscard]] VulkanMemoryManager* memoryManager() const { return memoryManager_.get(); }

 private:
  VkCommandBuffer beginSingleTimeCommands();
  void endSingleTimeCommands(VkCommandBuffer commandBuffer);

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

}  // namespace container::gpu
