#define VMA_IMPLEMENTATION
#include <Container/utility/VulkanMemoryManager.h>
#include <Container/utility/VulkanAlignment.h>

#include <cstring>
#include <stdexcept>

namespace utility {
namespace memory {

StagingBuffer::StagingBuffer(VulkanMemoryManager& manager, VkDeviceSize size,
                             VmaAllocationCreateFlags allocation_flags)
    : manager_(&manager), size_(size) {
  buffer_ = manager_->createBuffer(
      size_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
      allocation_flags);

  mapped_data_ = buffer_.allocation_info.pMappedData;
}

StagingBuffer::~StagingBuffer() {
  if (manager_ != nullptr && buffer_.allocation != VK_NULL_HANDLE) {
    if (mapped_here_ && mapped_data_ != nullptr) {
      vmaUnmapMemory(manager_->allocator(), buffer_.allocation);
    }
    manager_->destroyBuffer(buffer_);
  }
  manager_ = nullptr;
  mapped_data_ = nullptr;
  size_ = 0;
  mapped_here_ = false;
}

StagingBuffer::StagingBuffer(StagingBuffer&& other) noexcept
    : manager_(other.manager_),
      buffer_(other.buffer_),
      size_(other.size_),
      mapped_data_(other.mapped_data_),
      mapped_here_(other.mapped_here_) {
  other.manager_ = nullptr;
  other.buffer_ = {};
  other.size_ = 0;
  other.mapped_data_ = nullptr;
  other.mapped_here_ = false;
}

StagingBuffer& StagingBuffer::operator=(StagingBuffer&& other) noexcept {
  if (this != &other) {
    this->~StagingBuffer();
    manager_ = other.manager_;
    buffer_ = other.buffer_;
    size_ = other.size_;
    mapped_data_ = other.mapped_data_;
    mapped_here_ = other.mapped_here_;

    other.manager_ = nullptr;
    other.buffer_ = {};
    other.size_ = 0;
    other.mapped_data_ = nullptr;
    other.mapped_here_ = false;
  }
  return *this;
}

auto StagingBuffer::data() -> void* {
  if (mapped_data_ != nullptr) {
    return mapped_data_;
  }

  if (manager_ == nullptr || buffer_.allocation == VK_NULL_HANDLE) {
    throw std::runtime_error("StagingBuffer is not properly initialized");
  }

  if (const auto result = vmaMapMemory(manager_->allocator(),
                                       buffer_.allocation, &mapped_data_);
      result != VK_SUCCESS) {
    mapped_data_ = nullptr;
    throw std::runtime_error("Failed to map staging buffer");
  }

  mapped_here_ = true;
  return mapped_data_;
}

void StagingBuffer::upload(boost::span<const std::byte> bytes) {
  if (bytes.size_bytes() > size_) {
    throw std::runtime_error("StagingBuffer upload exceeds buffer size");
  }

  void* destination = data();
  if (destination == nullptr) {
    throw std::runtime_error("StagingBuffer mapping returned null pointer");
  }

  std::memcpy(destination, bytes.data(), bytes.size_bytes());
}

VulkanMemoryManager::VulkanMemoryManager(VkInstance instance,
                                         VkPhysicalDevice physical_device,
                                         VkDevice device,
                                         uint32_t vulkan_api_version)
    : device_(device) {
  VmaAllocatorCreateInfo allocator_info{};
  allocator_info.physicalDevice = physical_device;
  allocator_info.device = device;
  allocator_info.instance = instance;
  allocator_info.vulkanApiVersion = vulkan_api_version;
  allocator_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

  if (const auto result = vmaCreateAllocator(&allocator_info, &allocator_);
      result != VK_SUCCESS) {
    throw std::runtime_error("Failed to create VMA allocator");
  }
}

VulkanMemoryManager::~VulkanMemoryManager() { cleanup(); }

VulkanMemoryManager::VulkanMemoryManager(VulkanMemoryManager&& other) noexcept
    : allocator_(other.allocator_), device_(other.device_) {
  other.allocator_ = VK_NULL_HANDLE;
  other.device_ = VK_NULL_HANDLE;
}

VulkanMemoryManager& VulkanMemoryManager::operator=(
    VulkanMemoryManager&& other) noexcept {
  if (this != &other) {
    cleanup();
    allocator_ = other.allocator_;
    device_ = other.device_;
    other.allocator_ = VK_NULL_HANDLE;
    other.device_ = VK_NULL_HANDLE;
  }
  return *this;
}

auto VulkanMemoryManager::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                       VmaMemoryUsage memory_usage,
                                       VmaAllocationCreateFlags allocation_flags,
                                       VkSharingMode sharing_mode)
    -> AllocatedBuffer {
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = sharing_mode;

  VmaAllocationCreateInfo allocation_info{};
  allocation_info.usage = memory_usage;
  allocation_info.flags = allocation_flags;

  AllocatedBuffer buffer{};
  const auto result = vmaCreateBuffer(allocator_, &buffer_info, &allocation_info,
                                      &buffer.buffer, &buffer.allocation,
                                      &buffer.allocation_info);

  if (result != VK_SUCCESS) {
    throw std::runtime_error("Failed to allocate Vulkan buffer");
  }

  return buffer;
}

void VulkanMemoryManager::destroyBuffer(AllocatedBuffer& buffer) {
  if (allocator_ != VK_NULL_HANDLE && buffer.buffer != VK_NULL_HANDLE &&
      buffer.allocation != VK_NULL_HANDLE) {
    vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
  }

  buffer.buffer = VK_NULL_HANDLE;
  buffer.allocation = VK_NULL_HANDLE;
  buffer.allocation_info = {};
}

void VulkanMemoryManager::cleanup() {
  if (allocator_ != VK_NULL_HANDLE) {
    vmaDestroyAllocator(allocator_);
    allocator_ = VK_NULL_HANDLE;
  }
  device_ = VK_NULL_HANDLE;
}

BufferArena::BufferArena(VulkanMemoryManager& manager, VkDeviceSize total_size,
                         VkBufferUsageFlags usage, VmaMemoryUsage memory_usage,
                         VmaAllocationCreateFlags allocation_flags,
                         VkSharingMode sharing_mode)
    : manager_(&manager), total_size_(total_size) {
  buffer_ = manager_->createBuffer(total_size_, usage, memory_usage,
                                   allocation_flags, sharing_mode);
}

BufferArena::~BufferArena() {
  if (manager_ != nullptr && buffer_.buffer != VK_NULL_HANDLE) {
    manager_->destroyBuffer(buffer_);
  }
  manager_ = nullptr;
  total_size_ = 0;
  next_offset_ = 0;
}

BufferArena::BufferArena(BufferArena&& other) noexcept
    : manager_(other.manager_),
      buffer_(other.buffer_),
      total_size_(other.total_size_),
      next_offset_(other.next_offset_) {
  other.manager_ = nullptr;
  other.buffer_ = {};
  other.total_size_ = 0;
  other.next_offset_ = 0;
}

BufferArena& BufferArena::operator=(BufferArena&& other) noexcept {
  if (this != &other) {
    if (manager_ != nullptr && buffer_.buffer != VK_NULL_HANDLE) {
      manager_->destroyBuffer(buffer_);
    }

    manager_ = other.manager_;
    buffer_ = other.buffer_;
    total_size_ = other.total_size_;
    next_offset_ = other.next_offset_;

    other.manager_ = nullptr;
    other.buffer_ = {};
    other.total_size_ = 0;
    other.next_offset_ = 0;
  }
  return *this;
}

auto BufferArena::allocate(VkDeviceSize size, VkDeviceSize alignment)
    -> BufferSlice {
  if (alignment == 0) {
    alignment = 1;
  }

  const VkDeviceSize aligned_offset =
      VulkanAlignment::alignUp(next_offset_, alignment);

  if (aligned_offset > total_size_ || total_size_ - aligned_offset < size) {
    throw std::runtime_error("BufferArena out of space for allocation");
  }

  BufferSlice slice{buffer_.buffer, aligned_offset, size};
  next_offset_ = aligned_offset + size;
  return slice;
}

void BufferArena::reset() { next_offset_ = 0; }

}  // namespace memory
}  // namespace utility
