#include "Container/renderer/bim/BimSectionClipCapPassRecorder.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

VkPipeline
pipelineForBimSectionClipCapPass(BimSectionClipCapPassPipeline pipeline,
                                 VkPipeline fillPipeline,
                                 VkPipeline hatchPipeline) {
  switch (pipeline) {
  case BimSectionClipCapPassPipeline::Fill:
    return fillPipeline;
  case BimSectionClipCapPassPipeline::Hatch:
    return hatchPipeline;
  }
  return VK_NULL_HANDLE;
}

void bindGeometry(VkCommandBuffer cmd, container::gpu::BufferSlice vertex,
                  container::gpu::BufferSlice index, VkIndexType indexType) {
  const VkDeviceSize offsets[] = {vertex.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, index.buffer, index.offset, indexType);
}

[[nodiscard]] bool
hasRequiredInputs(const BimSectionClipCapPassRecordInputs &inputs) {
  return inputs.plan != nullptr && inputs.plan->active &&
         inputs.wireframeLayout != VK_NULL_HANDLE &&
         inputs.sceneDescriptorSet != VK_NULL_HANDLE &&
         inputs.vertexSlice.buffer != VK_NULL_HANDLE &&
         inputs.indexSlice.buffer != VK_NULL_HANDLE &&
         inputs.pushConstants != nullptr && inputs.debugOverlay != nullptr;
}

} // namespace

bool hasBimSectionClipCapFramePassGeometry(
    const BimSectionClipCapPassGeometryBinding &geometry) {
  return geometry.sceneDescriptorSet != VK_NULL_HANDLE &&
         geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

BimSectionClipCapPassInputs buildBimSectionClipCapFramePassPlanInputs(
    const BimSectionClipCapFramePassRecordInputs &inputs) {
  return {
      .enabled = inputs.style.enabled,
      .fillEnabled = inputs.style.fillEnabled,
      .hatchEnabled = inputs.style.hatchEnabled,
      .geometryReady = hasBimSectionClipCapFramePassGeometry(inputs.geometry),
      .wireframeLayoutReady = inputs.wireframeLayout != VK_NULL_HANDLE,
      .wireframePushConstantsReady = inputs.pushConstants != nullptr,
      .wideLinesSupported = inputs.style.wideLinesSupported,
      .fillPipelineReady = inputs.fillPipeline != VK_NULL_HANDLE,
      .hatchPipelineReady = inputs.hatchPipeline != VK_NULL_HANDLE,
      .fillColor = inputs.style.fillColor,
      .hatchColor = inputs.style.hatchColor,
      .hatchLineWidth = inputs.style.hatchLineWidth,
      .fillDrawCommands = inputs.style.fillDrawCommands,
      .hatchDrawCommands = inputs.style.hatchDrawCommands,
  };
}

bool recordBimSectionClipCapPassCommands(
    VkCommandBuffer cmd, const BimSectionClipCapPassRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          inputs.wireframeLayout, 0, 1,
                          &inputs.sceneDescriptorSet, 0, nullptr);
  bindGeometry(cmd, inputs.vertexSlice, inputs.indexSlice, inputs.indexType);

  WireframePushConstants pc = *inputs.pushConstants;
  pc.sectionPlaneEnabled = 0u;
  bool recorded = false;
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->routeCount;
       ++routeIndex) {
    const BimSectionClipCapPassRoute &route = inputs.plan->routes[routeIndex];
    const VkPipeline pipeline = pipelineForBimSectionClipCapPass(
        route.pipeline, inputs.fillPipeline, inputs.hatchPipeline);
    if (!hasDrawCommands(route.commands) || pipeline == VK_NULL_HANDLE) {
      continue;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (route.rasterLineWidthApplies) {
      vkCmdSetLineWidth(cmd, route.rasterLineWidth);
    }
    inputs.debugOverlay->drawWireframe(cmd, inputs.wireframeLayout,
                                       *route.commands, route.color,
                                       route.opacity, route.drawLineWidth, pc);
    if (route.resetRasterLineWidth) {
      vkCmdSetLineWidth(cmd, 1.0f);
    }
    recorded = true;
  }
  return recorded;
}

bool recordBimSectionClipCapFramePassCommands(
    VkCommandBuffer cmd, const BimSectionClipCapFramePassRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE) {
    return false;
  }

  const BimSectionClipCapPassPlan plan = buildBimSectionClipCapPassPlan(
      buildBimSectionClipCapFramePassPlanInputs(inputs));
  if (!plan.active) {
    return false;
  }

  return recordBimSectionClipCapPassCommands(
      cmd, {.plan = &plan,
            .fillPipeline = inputs.fillPipeline,
            .hatchPipeline = inputs.hatchPipeline,
            .wireframeLayout = inputs.wireframeLayout,
            .sceneDescriptorSet = inputs.geometry.sceneDescriptorSet,
            .vertexSlice = inputs.geometry.vertexSlice,
            .indexSlice = inputs.geometry.indexSlice,
            .indexType = inputs.geometry.indexType,
            .pushConstants = inputs.pushConstants,
            .debugOverlay = inputs.debugOverlay});
}

} // namespace container::renderer
