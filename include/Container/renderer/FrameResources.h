#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonVMA.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>

namespace container::renderer {

struct AttachmentImage {
  VkImage image{VK_NULL_HANDLE};
  VmaAllocation allocation{nullptr};
  VkImageView view{VK_NULL_HANDLE};
  VkFormat format{VK_FORMAT_UNDEFINED};
};

struct FrameResources {
  AttachmentImage albedo{};
  AttachmentImage normal{};
  AttachmentImage material{};
  AttachmentImage emissive{};
  AttachmentImage specular{};
  AttachmentImage pickId{};
  AttachmentImage pickDepth{};
  AttachmentImage depthStencil{};
  VkImageView     depthSamplingView{VK_NULL_HANDLE};
  AttachmentImage sceneColor{};
  AttachmentImage oitHeadPointers{};
  container::gpu::AllocatedBuffer oitNodeBuffer{};
  container::gpu::AllocatedBuffer oitCounterBuffer{};
  container::gpu::AllocatedBuffer oitMetadataBuffer{};
  uint32_t oitNodeCapacity{0};
  VkFramebuffer depthPrepassFramebuffer{VK_NULL_HANDLE};
  VkFramebuffer bimDepthPrepassFramebuffer{VK_NULL_HANDLE};
  VkFramebuffer gBufferFramebuffer{VK_NULL_HANDLE};
  VkFramebuffer bimGBufferFramebuffer{VK_NULL_HANDLE};
  VkFramebuffer transparentPickFramebuffer{VK_NULL_HANDLE};
  VkFramebuffer lightingFramebuffer{VK_NULL_HANDLE};
  VkDescriptorSet lightingDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet postProcessDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet oitDescriptorSet{VK_NULL_HANDLE};
};

}  // namespace container::renderer
