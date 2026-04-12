#pragma once
#include <string>

#include "Container/common/CommonVulkan.h"

namespace container::material {

struct TextureResource {
  std::string name;
  VkImage image{VK_NULL_HANDLE};
  VkImageView imageView{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
};

}  // namespace container::material