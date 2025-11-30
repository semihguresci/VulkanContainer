#pragma once

#include <vulkan/vulkan.h>

namespace utility {
namespace memory {

class VulkanAlignment {
 public:
  // Common Vulkan alignment requirements
  static constexpr VkDeviceSize UNIFORM_BUFFER_ALIGNMENT = 256;
  static constexpr VkDeviceSize STORAGE_BUFFER_ALIGNMENT = 256;
  static constexpr VkDeviceSize VERTEX_BUFFER_ALIGNMENT = 4;
  static constexpr VkDeviceSize INDEX_BUFFER_ALIGNMENT = 4;

  static constexpr VkDeviceSize alignUp(VkDeviceSize value,
                                        VkDeviceSize alignment) {
    if (alignment <= 1) {
      return value;
    }

    const VkDeviceSize remainder = value % alignment;
    if (remainder == 0) {
      return value;
    }

    const VkDeviceSize adjustment = alignment - remainder;
    return value + adjustment;
  }
};

}  // namespace memory
}  // namespace utility

