#ifndef UTILITY_VULKAN_MEMORY_MANAGER_H
#define UTILITY_VULKAN_MEMORY_MANAGER_H

#include <cstddef>
#include <utility>

#include <boost/core/span.hpp>

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace utility {
namespace memory {

struct AllocatedBuffer {
  VkBuffer buffer{VK_NULL_HANDLE};
  VmaAllocation allocation{VK_NULL_HANDLE};
  VmaAllocationInfo allocation_info{};
};

struct BufferSlice {
  VkBuffer buffer{VK_NULL_HANDLE};
  VkDeviceSize offset{0};
  VkDeviceSize size{0};
};

class StagingBuffer {
 public:
  StagingBuffer(VulkanMemoryManager& manager, VkDeviceSize size,
                VmaAllocationCreateFlags allocation_flags =
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT);
  ~StagingBuffer();

  StagingBuffer(const StagingBuffer&) = delete;
  StagingBuffer& operator=(const StagingBuffer&) = delete;
  StagingBuffer(StagingBuffer&& other) noexcept;
  StagingBuffer& operator=(StagingBuffer&& other) noexcept;

  [[nodiscard]] auto size() const -> VkDeviceSize { return size_; }
  [[nodiscard]] auto buffer() const -> const AllocatedBuffer& { return buffer_; }
  [[nodiscard]] auto data() -> void*;
  void upload(boost::span<const std::byte> bytes);

 private:
  VulkanMemoryManager* manager_;
  AllocatedBuffer buffer_{};
  VkDeviceSize size_{};
  void* mapped_data_{nullptr};
  bool mapped_here_{false};
};

class VulkanMemoryManager {
 public:
  VulkanMemoryManager(VkInstance instance, VkPhysicalDevice physical_device,
                      VkDevice device,
                      uint32_t vulkan_api_version = VK_API_VERSION_1_3);
  ~VulkanMemoryManager();

  VulkanMemoryManager(const VulkanMemoryManager&) = delete;
  VulkanMemoryManager& operator=(const VulkanMemoryManager&) = delete;
  VulkanMemoryManager(VulkanMemoryManager&& other) noexcept;
  VulkanMemoryManager& operator=(VulkanMemoryManager&& other) noexcept;

  [[nodiscard]] auto createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VmaMemoryUsage memory_usage,
                                  VmaAllocationCreateFlags allocation_flags = 0,
                                  VkSharingMode sharing_mode =
                                      VK_SHARING_MODE_EXCLUSIVE) -> AllocatedBuffer;

  void destroyBuffer(AllocatedBuffer& buffer);

  [[nodiscard]] auto allocator() const -> VmaAllocator { return allocator_; }

 private:
  void cleanup();

  VmaAllocator allocator_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
};

class BufferArena {
 public:
  BufferArena(VulkanMemoryManager& manager, VkDeviceSize total_size,
              VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
              VmaAllocationCreateFlags allocation_flags = 0,
              VkSharingMode sharing_mode = VK_SHARING_MODE_EXCLUSIVE);
  ~BufferArena();

  BufferArena(const BufferArena&) = delete;
  BufferArena& operator=(const BufferArena&) = delete;
  BufferArena(BufferArena&& other) noexcept;
  BufferArena& operator=(BufferArena&& other) noexcept;

  [[nodiscard]] auto allocate(VkDeviceSize size,
                              VkDeviceSize alignment = 16) -> BufferSlice;
  void reset();

  [[nodiscard]] auto backingBuffer() const -> const AllocatedBuffer& {
    return buffer_;
  }
  [[nodiscard]] auto remainingSize() const -> VkDeviceSize {
    return total_size_ - next_offset_;
  }

 private:
  VulkanMemoryManager* manager_;
  AllocatedBuffer buffer_{};
  VkDeviceSize total_size_;
  VkDeviceSize next_offset_ = 0;
};

}  // namespace memory
}  // namespace utility

#endif  // UTILITY_VULKAN_MEMORY_MANAGER_H
