#pragma once
#include <cstdint>
#include <string>

#include "Container/common/CommonVulkan.h"

namespace container::material {

struct TextureResource {
  std::string name;
  VkImage image{VK_NULL_HANDLE};
  VkImageView imageView{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  uint32_t samplerIndex{0};
};

}  // namespace container::material
