#include "Container/renderer/shadow/ShadowPassScopePlanner.h"
#include "Container/utility/SceneData.h"

namespace container::renderer {

ShadowPassScopePlan
buildShadowPassScopePlan(bool useSecondaryCommandBuffer, VkExtent2D extent) {
  if (extent.width == 0u || extent.height == 0u) {
    extent = {container::gpu::kShadowMapResolution,
              container::gpu::kShadowMapResolution};
  }

  ShadowPassScopePlan plan{};
  plan.renderArea.offset = {0, 0};
  plan.renderArea.extent = extent;
  plan.clearValues[0].depthStencil = {0.0f, 0};
  plan.contents = useSecondaryCommandBuffer
                      ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
                      : VK_SUBPASS_CONTENTS_INLINE;
  plan.executeSecondary = useSecondaryCommandBuffer;
  return plan;
}

} // namespace container::renderer
