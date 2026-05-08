#include "Container/renderer/deferred/DeferredRasterTileCullPlanner.h"

namespace container::renderer {

namespace {

[[nodiscard]] RenderPassReadiness ready() { return {}; }

[[nodiscard]] RenderPassReadiness notNeeded() {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::NotNeeded;
  return readiness;
}

[[nodiscard]] RenderPassReadiness missingSceneDepth() {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::MissingResource;
  readiness.blockingResource = RenderResourceId::SceneDepth;
  return readiness;
}

} // namespace

DeferredRasterTileCullPlan buildDeferredRasterTileCullPlan(
    const DeferredRasterTileCullPlanInputs &inputs) {
  DeferredRasterTileCullPlan plan{};
  if (!inputs.tileCullDisplayMode || !inputs.tiledLightingReady) {
    plan.readiness = notNeeded();
    return plan;
  }

  if (!inputs.frameAvailable ||
      inputs.depthSamplingView == VK_NULL_HANDLE ||
      inputs.cameraBuffer == VK_NULL_HANDLE || inputs.cameraBufferSize == 0u) {
    plan.readiness = missingSceneDepth();
    return plan;
  }

  plan.active = true;
  plan.readiness = ready();
  plan.screenExtent = inputs.screenExtent;
  plan.cameraBuffer = inputs.cameraBuffer;
  plan.cameraBufferSize = inputs.cameraBufferSize;
  plan.depthSamplingView = inputs.depthSamplingView;
  plan.cameraNear = inputs.cameraNear;
  plan.cameraFar = inputs.cameraFar;
  return plan;
}

} // namespace container::renderer
