#include "Container/renderer/deferred/DeferredPointLightingRecorder.h"

#include "Container/renderer/lighting/LightingManager.h"

namespace container::renderer {

namespace {

constexpr VkShaderStageFlags kLightPushStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

VkPipeline pipelineForDeferredPointLighting(
    DeferredPointLightingStencilPipeline pipeline,
    const DeferredPointLightingRecordInputs &inputs) {
  switch (pipeline) {
  case DeferredPointLightingStencilPipeline::PointLight:
    return inputs.pointLightPipeline;
  case DeferredPointLightingStencilPipeline::PointLightStencilDebug:
    return inputs.pointLightStencilDebugPipeline;
  }
  return VK_NULL_HANDLE;
}

void copyLightPushConstants(LightPushConstants &pushConstants,
                            const container::gpu::PointLightData &light) {
  pushConstants.positionRadius = light.positionRadius;
  pushConstants.colorIntensity = light.colorIntensity;
  pushConstants.directionInnerCos = light.directionInnerCos;
  pushConstants.coneOuterCosType = light.coneOuterCosType;
}

} // namespace

bool recordDeferredPointLightingCommands(
    VkCommandBuffer cmd, const DeferredPointLightingRecordInputs &inputs) {
  if (inputs.plan == nullptr ||
      inputs.plan->path == DeferredPointLightingPath::None) {
    return false;
  }

  const DeferredPointLightingDrawPlan &plan = *inputs.plan;
  if (plan.path == DeferredPointLightingPath::Tiled) {
    if (inputs.tiledPointLightPipeline == VK_NULL_HANDLE ||
        inputs.tiledLightingLayout == VK_NULL_HANDLE ||
        inputs.lightingManager == nullptr) {
      return false;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      inputs.tiledPointLightPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inputs.tiledLightingLayout, 0,
        static_cast<uint32_t>(inputs.tiledLightingDescriptorSets.size()),
        inputs.tiledLightingDescriptorSets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, inputs.tiledLightingLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(plan.tiledPushConstants),
                       &plan.tiledPushConstants);
    inputs.lightingManager->beginClusteredLightingTimer(cmd);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    inputs.lightingManager->endClusteredLightingTimer(cmd);
    return true;
  }

  if (inputs.lightingLayout == VK_NULL_HANDLE ||
      inputs.stencilVolumePipeline == VK_NULL_HANDLE ||
      inputs.lightPushConstants == nullptr ||
      plan.stencilRouteCount == 0u) {
    return false;
  }

  const VkPipeline activePointPipeline =
      pipelineForDeferredPointLighting(plan.stencilPipeline, inputs);
  if (activePointPipeline == VK_NULL_HANDLE) {
    return false;
  }

  VkClearAttachment stencilClearAttachment{};
  stencilClearAttachment.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
  stencilClearAttachment.clearValue.depthStencil = {0.0f, 0};
  VkClearRect stencilClearRect{};
  stencilClearRect.rect.offset = {0, 0};
  stencilClearRect.rect.extent = inputs.framebufferExtent;
  stencilClearRect.baseArrayLayer = 0;
  stencilClearRect.layerCount = 1;

  vkCmdBindDescriptorSets(
      cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inputs.lightingLayout, 0,
      static_cast<uint32_t>(inputs.pointLightingDescriptorSets.size()),
      inputs.pointLightingDescriptorSets.data(), 0, nullptr);

  for (uint32_t routeIndex = 0u; routeIndex < plan.stencilRouteCount;
       ++routeIndex) {
    const container::gpu::PointLightData &light =
        plan.stencilRoutes[routeIndex].light;
    vkCmdClearAttachments(cmd, 1, &stencilClearAttachment, 1,
                          &stencilClearRect);
    copyLightPushConstants(*inputs.lightPushConstants, light);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      inputs.stencilVolumePipeline);
    vkCmdPushConstants(cmd, inputs.lightingLayout, kLightPushStages, 0,
                       sizeof(LightPushConstants), inputs.lightPushConstants);
    vkCmdDraw(cmd, plan.lightVolumeIndexCount, 1, 0, 0);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      activePointPipeline);
    vkCmdPushConstants(cmd, inputs.lightingLayout, kLightPushStages, 0,
                       sizeof(LightPushConstants), inputs.lightPushConstants);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }
  return true;
}

} // namespace container::renderer
