#include "Container/renderer/SceneViewport.h"

namespace container::renderer {

SceneViewportScissor buildSceneViewportScissor(VkExtent2D extent) {
  SceneViewportScissor state{};
  state.viewport.x = 0.0f;
  state.viewport.y = static_cast<float>(extent.height);
  state.viewport.width = static_cast<float>(extent.width);
  // Scene passes use a negative-height viewport so NDC +Y maps to the top of
  // the framebuffer while projection matrices remain glTF/right-handed.
  state.viewport.height = -static_cast<float>(extent.height);
  state.viewport.minDepth = 0.0f;
  state.viewport.maxDepth = 1.0f;
  state.scissor.offset = {0, 0};
  state.scissor.extent = extent;
  return state;
}

void recordSceneViewportAndScissor(VkCommandBuffer commandBuffer,
                                   VkExtent2D extent) {
  const SceneViewportScissor state = buildSceneViewportScissor(extent);
  vkCmdSetViewport(commandBuffer, 0, 1, &state.viewport);
  vkCmdSetScissor(commandBuffer, 0, 1, &state.scissor);
}

}  // namespace container::renderer
