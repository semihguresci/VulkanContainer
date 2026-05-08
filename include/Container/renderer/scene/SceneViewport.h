#pragma once

#include "Container/common/CommonVulkan.h"

namespace container::renderer {

struct SceneViewportScissor {
  VkViewport viewport{};
  VkRect2D scissor{};
};

[[nodiscard]] SceneViewportScissor buildSceneViewportScissor(
    VkExtent2D extent);

void recordSceneViewportAndScissor(VkCommandBuffer commandBuffer,
                                   VkExtent2D extent);

}  // namespace container::renderer
