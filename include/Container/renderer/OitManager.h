#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/FrameResources.h"
#include "Container/utility/VulkanDevice.h"

#include <memory>

namespace container::renderer {

// Handles OIT buffer clear, barrier, and resolve-prepare barriers.
// Does not own any resources — operates on FrameResources passed in.
class OitManager {
 public:
  explicit OitManager(std::shared_ptr<container::gpu::VulkanDevice> device);

  OitManager(const OitManager&) = delete;
  OitManager& operator=(const OitManager&) = delete;

  void clearResources(VkCommandBuffer cmd, const FrameResources& frame,
                      uint32_t invalidNodeIndex) const;

  void prepareResolve(VkCommandBuffer cmd, const FrameResources& frame) const;

 private:
  std::shared_ptr<container::gpu::VulkanDevice> device_;
};

}  // namespace container::renderer
