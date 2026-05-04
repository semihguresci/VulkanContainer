#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/DebugOverlayRenderer.h"
#include "Container/renderer/TransformGizmoState.h"

#include <cstdint>

namespace container::ui {
enum class ViewportTool : uint32_t;
}  // namespace container::ui

namespace container::renderer {

struct DeferredTransformGizmoDrawInputs {
  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  FrameTransformGizmoState gizmo{};
  bool wideLinesSupported{false};
  VkDescriptorSet lightingDescriptorSet{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipeline solidPipeline{VK_NULL_HANDLE};
  TransformGizmoPushConstants* pushConstants{nullptr};
};

struct DeferredTransformGizmoPassInputs {
  VkRenderPass renderPass{VK_NULL_HANDLE};
  VkFramebuffer framebuffer{VK_NULL_HANDLE};
  DeferredTransformGizmoDrawInputs draw{};
};

[[nodiscard]] uint32_t deferredTransformGizmoVertexCount(
    container::ui::ViewportTool tool);
[[nodiscard]] bool deferredTransformGizmoUsesSolidArrowheads(
    container::ui::ViewportTool tool);

void updateDeferredTransformGizmoPushConstants(
    TransformGizmoPushConstants& pushConstants,
    const FrameTransformGizmoState& gizmo);

void recordDeferredTransformGizmoPass(
    const DeferredTransformGizmoPassInputs& inputs);
void recordDeferredTransformGizmoOverlay(
    const DeferredTransformGizmoDrawInputs& inputs);

}  // namespace container::renderer
