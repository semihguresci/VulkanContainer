#include "Container/renderer/deferred/DeferredTransparentOitRecorder.h"

#include "Container/renderer/effects/OitManager.h"
#include "Container/renderer/scene/DrawCommand.h"

#include <vector>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool hasRequiredInputs(
    const DeferredTransparentOitRecordInputs &inputs) {
  return inputs.pipelineLayout != VK_NULL_HANDLE &&
         inputs.debugOverlay != nullptr;
}

[[nodiscard]] bool hasReadyDescriptorSets(
    const std::array<VkDescriptorSet, 4> &descriptorSets) {
  for (VkDescriptorSet descriptorSet : descriptorSets) {
    if (descriptorSet == VK_NULL_HANDLE) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool hasReadyGeometry(
    const DeferredTransparentOitGeometryBinding &geometry) {
  return geometry.descriptorSet != VK_NULL_HANDLE &&
         geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

[[nodiscard]] VkPipeline pipelineForBimTransparentRoute(
    const BimSurfaceDrawRoute &route,
    const DeferredTransparentOitPipelineHandles &pipelines) {
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

[[nodiscard]] VkPipeline choosePipeline(VkPipeline preferred,
                                        VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

[[nodiscard]] DeferredTransparentOitPipelineHandles resolvedPipelines(
    const DeferredTransparentOitPipelineHandles &pipelines) {
  return {.primary = pipelines.primary,
          .frontCull = choosePipeline(pipelines.frontCull, pipelines.primary),
          .noCull = choosePipeline(pipelines.noCull, pipelines.primary)};
}

[[nodiscard]] bool hasSceneRecordablePlan(
    const SceneTransparentDrawPlan *plan) {
  return plan != nullptr && plan->routeCount > 0u;
}

[[nodiscard]] bool hasBimRecordableRoute(
    const DeferredTransparentOitRecordInputs &inputs) {
  if (inputs.bimPlan == nullptr || !inputs.bimPlan->active) {
    return false;
  }
  const DeferredTransparentOitPipelineHandles pipelines =
      resolvedPipelines(inputs.pipelines);
  for (uint32_t sourceIndex = 0u; sourceIndex < inputs.bimPlan->sourceCount;
       ++sourceIndex) {
    const BimSurfacePassSourcePlan &sourcePlan =
        inputs.bimPlan->sources[sourceIndex];
    for (uint32_t routeIndex = 0u; routeIndex < sourcePlan.routeCount;
         ++routeIndex) {
      const BimSurfaceDrawRoute &route = sourcePlan.routes[routeIndex];
      const VkPipeline pipeline =
          pipelineForBimTransparentRoute(route, pipelines);
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

[[nodiscard]] std::array<VkDescriptorSet, 4> descriptorSetsForGeometry(
    std::array<VkDescriptorSet, 4> descriptorSets,
    const DeferredTransparentOitGeometryBinding &geometry) {
  descriptorSets[0] = geometry.descriptorSet;
  return descriptorSets;
}

[[nodiscard]] bool hasClearResources(const OitFrameResources &resources) {
  return resources.headPointerImage != VK_NULL_HANDLE &&
         resources.counterBuffer != VK_NULL_HANDLE;
}

[[nodiscard]] bool hasResolveResources(const OitFrameResources &resources) {
  return resources.headPointerImage != VK_NULL_HANDLE &&
         resources.nodeBuffer != VK_NULL_HANDLE &&
         resources.counterBuffer != VK_NULL_HANDLE;
}

void bindTransparentGeometry(
    VkCommandBuffer cmd, VkPipelineLayout pipelineLayout,
    const std::array<VkDescriptorSet, 4> &descriptorSets,
    const DeferredTransparentOitGeometryBinding &geometry) {
  vkCmdBindDescriptorSets(
      cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
      static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data(), 0,
      nullptr);
  const VkDeviceSize offsets[] = {geometry.vertexSlice.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &geometry.vertexSlice.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, geometry.indexSlice.buffer,
                       geometry.indexSlice.offset, geometry.indexType);
}

} // namespace

bool recordDeferredTransparentOitCommands(
    VkCommandBuffer cmd, const DeferredTransparentOitRecordInputs &inputs) {
  if (!hasRequiredInputs(inputs)) {
    return false;
  }

  bool recorded = false;
  const DeferredTransparentOitPipelineHandles pipelines =
      resolvedPipelines(inputs.pipelines);
  if (hasSceneRecordablePlan(inputs.scenePlan) &&
      hasReadyGeometry(inputs.scene)) {
    const std::array<VkDescriptorSet, 4> sceneTransparentSets =
        descriptorSetsForGeometry(inputs.descriptorSets, inputs.scene);
    if (hasReadyDescriptorSets(sceneTransparentSets)) {
      container::gpu::BindlessPushConstants transparentPc =
          inputs.pushConstants;
      transparentPc.semanticColorMode = 0u;
      recorded =
          recordSceneTransparentDrawCommands(
              cmd,
              {.plan = inputs.scenePlan,
               .geometry = {.descriptorSets = sceneTransparentSets,
                            .vertexSlice = inputs.scene.vertexSlice,
                            .indexSlice = inputs.scene.indexSlice,
                            .indexType = inputs.scene.indexType},
               .pipelines = {.primary = pipelines.primary,
                             .frontCull = pipelines.frontCull,
                             .noCull = pipelines.noCull},
               .pipelineLayout = inputs.pipelineLayout,
               .pushConstants = transparentPc,
               .debugOverlay = inputs.debugOverlay}) ||
          recorded;
    }
  }

  if (hasBimRecordableRoute(inputs) && hasReadyGeometry(inputs.bim)) {
    const std::array<VkDescriptorSet, 4> bimTransparentSets =
        descriptorSetsForGeometry(inputs.descriptorSets, inputs.bim);
    if (hasReadyDescriptorSets(bimTransparentSets)) {
      container::gpu::BindlessPushConstants bimTransparentPc =
          inputs.pushConstants;
      if (inputs.bimPlan->writesSemanticColorMode) {
        bimTransparentPc.semanticColorMode = inputs.bimPlan->semanticColorMode;
      }
      recorded =
          recordBimSurfacePassCommands(
              cmd, {.plan = inputs.bimPlan,
                    .geometry = {.descriptorSets = bimTransparentSets,
                                 .vertexSlice = inputs.bim.vertexSlice,
                                 .indexSlice = inputs.bim.indexSlice,
                                 .indexType = inputs.bim.indexType},
                    .singleSidedPipeline = pipelines.primary,
                    .windingFlippedPipeline = pipelines.frontCull,
                    .doubleSidedPipeline = pipelines.noCull,
                    .pipelineLayout = inputs.pipelineLayout,
                    .pushConstants = bimTransparentPc,
                    .debugOverlay = inputs.debugOverlay,
                    .bimManager = inputs.bimManager}) ||
          recorded;
    }
  }
  return recorded;
}

bool recordDeferredTransparentOitClearCommands(
    VkCommandBuffer cmd,
    const DeferredTransparentOitFrameResourceInputs &inputs) {
  if (cmd == VK_NULL_HANDLE || inputs.oitManager == nullptr ||
      !hasClearResources(inputs.resources)) {
    return false;
  }
  inputs.oitManager->clearResources(cmd, inputs.resources,
                                    inputs.invalidNodeIndex);
  return true;
}

bool recordDeferredTransparentOitResolvePreparationCommands(
    VkCommandBuffer cmd,
    const DeferredTransparentOitFrameResourceInputs &inputs) {
  if (cmd == VK_NULL_HANDLE || inputs.oitManager == nullptr ||
      !hasResolveResources(inputs.resources)) {
    return false;
  }
  inputs.oitManager->prepareResolve(cmd, inputs.resources);
  return true;
}

} // namespace container::renderer
