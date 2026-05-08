#include "Container/renderer/deferred/DeferredRasterHiZPassPlanner.h"

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

DeferredRasterHiZPassPlan
buildDeferredRasterHiZPassPlan(const DeferredRasterHiZPassPlanInputs &inputs) {
  DeferredRasterHiZPassPlan plan{};
  if (!inputs.gpuCullManagerReady) {
    plan.readiness = notNeeded();
    return plan;
  }

  if (!inputs.frameReady || !inputs.depthSamplingViewReady ||
      !inputs.depthSamplerReady || !inputs.depthStencilImageReady) {
    plan.readiness = missingSceneDepth();
    return plan;
  }

  plan.active = true;
  plan.readiness = ready();
  return plan;
}

} // namespace container::renderer
