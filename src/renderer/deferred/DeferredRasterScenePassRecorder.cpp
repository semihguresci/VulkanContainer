#include "Container/renderer/deferred/DeferredRasterScenePassRecorder.h"

#include "Container/renderer/culling/GpuCullManager.h"

namespace container::renderer {

namespace {

bool sceneOpaqueGpuIndirectAvailable(const GpuCullManager *gpuCullManager,
                                     bool frustumCullActive) {
  return gpuCullManager != nullptr && gpuCullManager->isReady() &&
         frustumCullActive && gpuCullManager->frustumDrawsValid();
}

} // namespace

bool recordDeferredRasterScenePassCommands(
    VkCommandBuffer cmd, const DeferredRasterScenePassRecordInputs &inputs) {
  if (inputs.pushConstants == nullptr) {
    return false;
  }

  const SceneRasterPassPlan sceneRasterPlan = buildSceneRasterPassPlan(
      {.kind = inputs.kind,
       .gpuIndirectAvailable = sceneOpaqueGpuIndirectAvailable(
           inputs.gpuCullManager, inputs.frustumCullActive),
       .draws = inputs.draws,
       .pipelines = inputs.pipelines});

  return recordSceneRasterPassCommands(
      cmd, {.renderPass = inputs.renderPass,
            .framebuffer = inputs.framebuffer,
            .extent = inputs.extent,
            .clearValues = sceneRasterPlan.clearValues,
            .plan = &sceneRasterPlan.drawPlan,
            .geometry = inputs.geometry,
            .pipelines = sceneRasterPlan.pipelines,
            .pipelineLayout = inputs.pipelineLayout,
            .pushConstants = *inputs.pushConstants,
            .debugOverlay = inputs.debugOverlay,
            .gpuCullManager = inputs.gpuCullManager,
            .diagnosticCube = inputs.diagnosticCube});
}

} // namespace container::renderer
