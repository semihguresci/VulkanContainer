#include "Container/renderer/picking/TransparentPickPassRecorder.h"

#include "Container/renderer/scene/DrawCommand.h"

#include <array>
#include <vector>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool hasReadyGeometry(
    const TransparentPickPassGeometryBinding &geometry) {
  return geometry.descriptorSet != VK_NULL_HANDLE &&
         geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

[[nodiscard]] VkPipeline choosePipeline(VkPipeline preferred,
                                        VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

[[nodiscard]] TransparentPickPassPipelineHandles resolvedPipelines(
    const TransparentPickPassPipelineHandles &pipelines) {
  return {.primary = pipelines.primary,
          .frontCull = choosePipeline(pipelines.frontCull, pipelines.primary),
          .noCull = choosePipeline(pipelines.noCull, pipelines.primary)};
}

[[nodiscard]] VkPipeline pipelineForBimTransparentPickRoute(
    const BimSurfaceDrawRoute &route,
    const TransparentPickPassPipelineHandles &pipelines) {
  switch (route.kind) {
  case BimSurfaceDrawRouteKind::SingleSided:
    return pipelines.primary;
  case BimSurfaceDrawRouteKind::WindingFlipped:
    return pipelines.frontCull;
  case BimSurfaceDrawRouteKind::DoubleSided:
    return pipelines.noCull;
  }
  return VK_NULL_HANDLE;
}

[[nodiscard]] bool hasSceneRecordablePlan(
    const SceneTransparentDrawPlan *plan) {
  return plan != nullptr && plan->routeCount > 0u;
}

[[nodiscard]] bool hasBimRecordableRoute(
    const TransparentPickPassRecordInputs &inputs,
    const TransparentPickPassPipelineHandles &pipelines) {
  if (inputs.bimPlan == nullptr || !inputs.bimPlan->active) {
    return false;
  }

  for (uint32_t sourceIndex = 0u; sourceIndex < inputs.bimPlan->sourceCount;
       ++sourceIndex) {
    const BimSurfacePassSourcePlan &sourcePlan =
        inputs.bimPlan->sources[sourceIndex];
    for (uint32_t routeIndex = 0u; routeIndex < sourcePlan.routeCount;
         ++routeIndex) {
      const BimSurfaceDrawRoute &route = sourcePlan.routes[routeIndex];
      const VkPipeline pipeline =
          pipelineForBimTransparentPickRoute(route, pipelines);
      if (pipeline == VK_NULL_HANDLE) {
        continue;
      }
      if ((route.cpuFallbackAllowed && hasDrawCommands(route.cpuCommands)) ||
          (route.gpuCompactionAllowed && inputs.bimManager != nullptr)) {
        return true;
      }
    }
  }
  return false;
}

} // namespace

bool recordTransparentPickPassCommands(
    VkCommandBuffer cmd, const TransparentPickPassRecordInputs &inputs) {
  if (inputs.pipelineLayout == VK_NULL_HANDLE ||
      inputs.debugOverlay == nullptr) {
    return false;
  }

  bool recorded = false;
  const TransparentPickPassPipelineHandles pipelines =
      resolvedPipelines(inputs.pipelines);
  if (hasSceneRecordablePlan(inputs.scenePlan) &&
      hasReadyGeometry(inputs.scene)) {
    const std::array<VkDescriptorSet, 1> sceneDescriptorSets = {
        inputs.scene.descriptorSet};
    recorded =
        recordSceneTransparentDrawCommands(
            cmd,
            {.plan = inputs.scenePlan,
             .geometry = {.descriptorSets = sceneDescriptorSets,
                          .vertexSlice = inputs.scene.vertexSlice,
                          .indexSlice = inputs.scene.indexSlice,
                          .indexType = inputs.scene.indexType},
             .pipelines = {.primary = pipelines.primary,
                           .frontCull = pipelines.frontCull,
                           .noCull = pipelines.noCull},
             .pipelineLayout = inputs.pipelineLayout,
             .pushConstants = inputs.pushConstants,
             .debugOverlay = inputs.debugOverlay}) ||
        recorded;
  }

  if (hasBimRecordableRoute(inputs, pipelines) && hasReadyGeometry(inputs.bim)) {
    const std::array<VkDescriptorSet, 1> bimDescriptorSets = {
        inputs.bim.descriptorSet};
    recorded =
        recordBimSurfacePassCommands(
            cmd, {.plan = inputs.bimPlan,
                  .geometry = {.descriptorSets = bimDescriptorSets,
                               .vertexSlice = inputs.bim.vertexSlice,
                               .indexSlice = inputs.bim.indexSlice,
                               .indexType = inputs.bim.indexType},
                  .singleSidedPipeline = pipelines.primary,
                  .windingFlippedPipeline = pipelines.frontCull,
                  .doubleSidedPipeline = pipelines.noCull,
                  .pipelineLayout = inputs.pipelineLayout,
                  .pushConstants = inputs.pushConstants,
                  .debugOverlay = inputs.debugOverlay,
                  .bimManager = inputs.bimManager}) ||
        recorded;
  }
  return recorded;
}

} // namespace container::renderer
