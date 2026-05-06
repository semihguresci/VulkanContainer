#include "Container/renderer/deferred/DeferredLightingPassPlanner.h"

namespace container::renderer {

DeferredLightingPassPlan
buildDeferredLightingPassPlan(VkExtent2D framebufferExtent) {
  DeferredLightingPassPlan plan{};
  plan.renderArea.offset = {0, 0};
  plan.renderArea.extent = framebufferExtent;

  plan.clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  plan.clearValues[1].depthStencil = {0.0f, 0};

  plan.selectionStencilClearAttachment.aspectMask =
      VK_IMAGE_ASPECT_STENCIL_BIT;
  plan.selectionStencilClearAttachment.clearValue.depthStencil = {0.0f, 0};
  plan.selectionStencilClearRect.rect.offset = {0, 0};
  plan.selectionStencilClearRect.rect.extent = framebufferExtent;
  plan.selectionStencilClearRect.baseArrayLayer = 0;
  plan.selectionStencilClearRect.layerCount = 1;
  return plan;
}

} // namespace container::renderer
