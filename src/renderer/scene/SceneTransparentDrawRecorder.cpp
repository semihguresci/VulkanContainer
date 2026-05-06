#include "Container/renderer/scene/SceneTransparentDrawRecorder.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool hasRequiredInputs(
    const SceneTransparentDrawRecordInputs &inputs) {
  return inputs.plan != nullptr && inputs.plan->routeCount > 0u &&
         inputs.pipelineLayout != VK_NULL_HANDLE &&
         inputs.debugOverlay != nullptr;
}

[[nodiscard]] bool hasReadyGeometry(
    const SceneTransparentDrawGeometryBinding &geometry) {
  if (geometry.descriptorSets.empty() ||
      geometry.vertexSlice.buffer == VK_NULL_HANDLE ||
      geometry.indexSlice.buffer == VK_NULL_HANDLE) {
    return false;
  }
  for (VkDescriptorSet descriptorSet : geometry.descriptorSets) {
    if (descriptorSet == VK_NULL_HANDLE) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] VkPipeline pipelineForSceneTransparentRoute(
    const SceneTransparentDrawRoute &route,
    const SceneTransparentDrawPipelineHandles &pipelines) {
  switch (route.pipeline) {
  case SceneTransparentDrawPipeline::Primary:
    return pipelines.primary;
  case SceneTransparentDrawPipeline::FrontCull:
    return pipelines.frontCull;
  case SceneTransparentDrawPipeline::NoCull:
    return pipelines.noCull;
  }
  return pipelines.primary;
}

[[nodiscard]] bool hasRecordableRoute(
    const SceneTransparentDrawRecordInputs &inputs) {
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->routeCount;
       ++routeIndex) {
    const SceneTransparentDrawRoute &route = inputs.plan->routes[routeIndex];
    if (hasDrawCommands(route.commands) &&
        pipelineForSceneTransparentRoute(route, inputs.pipelines) !=
            VK_NULL_HANDLE) {
      return true;
    }
  }
  return false;
}

void bindGeometry(VkCommandBuffer cmd,
                  const SceneTransparentDrawGeometryBinding &geometry,
                  VkPipelineLayout layout) {
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                          static_cast<uint32_t>(geometry.descriptorSets.size()),
                          geometry.descriptorSets.data(), 0, nullptr);
  const VkDeviceSize offsets[] = {geometry.vertexSlice.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &geometry.vertexSlice.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, geometry.indexSlice.buffer,
                       geometry.indexSlice.offset, geometry.indexType);
}

} // namespace

bool recordSceneTransparentDrawCommands(
    VkCommandBuffer cmd, const SceneTransparentDrawRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs) || !hasRecordableRoute(inputs) ||
      !hasReadyGeometry(inputs.geometry)) {
    return false;
  }

  bindGeometry(cmd, inputs.geometry, inputs.pipelineLayout);
  bool recorded = false;
  container::gpu::BindlessPushConstants pushConstants = inputs.pushConstants;
  for (uint32_t routeIndex = 0u; routeIndex < inputs.plan->routeCount;
       ++routeIndex) {
    const SceneTransparentDrawRoute &route = inputs.plan->routes[routeIndex];
    const VkPipeline pipeline =
        pipelineForSceneTransparentRoute(route, inputs.pipelines);
    if (!hasDrawCommands(route.commands) || pipeline == VK_NULL_HANDLE) {
      continue;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    inputs.debugOverlay->drawScene(cmd, inputs.pipelineLayout,
                                   *route.commands, pushConstants);
    recorded = true;
  }
  return recorded;
}

} // namespace container::renderer
