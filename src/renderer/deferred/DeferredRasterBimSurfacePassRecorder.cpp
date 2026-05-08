#include "Container/renderer/deferred/DeferredRasterBimSurfacePassRecorder.h"

namespace container::renderer {

namespace {

bool isSupportedBimSurfaceRasterPass(BimSurfacePassKind kind) {
  return kind == BimSurfacePassKind::DepthPrepass ||
         kind == BimSurfacePassKind::GBuffer;
}

} // namespace

bool recordDeferredRasterBimSurfacePassCommands(
    VkCommandBuffer cmd,
    const DeferredRasterBimSurfacePassRecordInputs &inputs) {
  if (!isSupportedBimSurfaceRasterPass(inputs.kind)) {
    return false;
  }

  return recordBimSurfaceFramePassCommands(
      cmd, {.kind = inputs.kind,
            .passReady = inputs.passReady,
            .draws = inputs.binding.draws,
            .renderPass = inputs.renderPass,
            .framebuffer = inputs.framebuffer,
            .extent = inputs.extent,
            .geometry = inputs.binding.geometry,
            .pipelines = inputs.pipelines,
            .pipelineLayout = inputs.pipelineLayout,
            .pushConstants = inputs.pushConstants,
            .semanticColorMode = inputs.binding.semanticColorMode,
            .debugOverlay = inputs.debugOverlay,
            .bimManager = inputs.bimManager});
}

} // namespace container::renderer
