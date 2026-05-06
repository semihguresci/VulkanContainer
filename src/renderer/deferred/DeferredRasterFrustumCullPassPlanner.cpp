#include "Container/renderer/deferred/DeferredRasterFrustumCullPassPlanner.h"

namespace container::renderer {

namespace {

[[nodiscard]] RenderPassReadiness ready() { return {}; }

[[nodiscard]] RenderPassReadiness notNeeded() {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::NotNeeded;
  return readiness;
}

[[nodiscard]] RenderPassReadiness missingResource(RenderResourceId resource) {
  RenderPassReadiness readiness{};
  readiness.ready = false;
  readiness.skipReason = RenderPassSkipReason::MissingResource;
  readiness.blockingResource = resource;
  return readiness;
}

} // namespace

DeferredRasterFrustumCullPassPlan buildDeferredRasterFrustumCullPassPlan(
    const DeferredRasterFrustumCullPassPlanInputs &inputs) {
  DeferredRasterFrustumCullPassPlan plan{};
  if (!inputs.gpuCullManagerReady ||
      !inputs.sceneSingleSidedDrawsAvailable ||
      inputs.sourceDrawCount == 0u) {
    plan.readiness = notNeeded();
    return plan;
  }

  if (!inputs.cameraBufferReady) {
    plan.readiness = missingResource(RenderResourceId::CameraBuffer);
    return plan;
  }

  if (!inputs.objectBufferReady) {
    plan.readiness = missingResource(RenderResourceId::ObjectBuffer);
    return plan;
  }

  plan.active = true;
  plan.readiness = ready();
  plan.drawCount = inputs.sourceDrawCount;
  plan.updateObjectDescriptor = true;
  if (inputs.debugFreezeCulling && !inputs.cullingFrozen) {
    plan.freezeAction = DeferredRasterFrustumCullFreezeAction::Freeze;
  } else if (!inputs.debugFreezeCulling && inputs.cullingFrozen) {
    plan.freezeAction = DeferredRasterFrustumCullFreezeAction::Unfreeze;
  }
  return plan;
}

} // namespace container::renderer
