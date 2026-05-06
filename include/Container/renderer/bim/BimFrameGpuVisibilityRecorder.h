#pragma once

#include "Container/common/CommonVulkan.h"

namespace container::renderer {

class BimManager;

struct BimFrameGpuVisibilityRecordInputs {
  BimManager *manager{nullptr};
  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  VkBuffer cameraBuffer{VK_NULL_HANDLE};
  VkDeviceSize cameraBufferSize{0};
  VkBuffer objectBuffer{VK_NULL_HANDLE};
  VkDeviceSize objectBufferSize{0};
};

void prepareBimFrameGpuVisibility(BimManager *manager);

[[nodiscard]] bool recordBimFrameGpuVisibilityCommands(
    const BimFrameGpuVisibilityRecordInputs &inputs);

} // namespace container::renderer
