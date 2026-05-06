#include "Container/renderer/deferred/DeferredRasterFrustumCullPassPlanner.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::DeferredRasterFrustumCullFreezeAction;
using container::renderer::DeferredRasterFrustumCullPassPlanInputs;
using container::renderer::RenderPassSkipReason;
using container::renderer::RenderResourceId;
using container::renderer::buildDeferredRasterFrustumCullPassPlan;

DeferredRasterFrustumCullPassPlanInputs readyInputs() {
  return {.gpuCullManagerReady = true,
          .sceneSingleSidedDrawsAvailable = true,
          .cameraBufferReady = true,
          .objectBufferReady = true,
          .sourceDrawCount = 24u};
}

} // namespace

TEST(DeferredRasterFrustumCullPassPlannerTests,
     UnreadyManagerReturnsNotNeeded) {
  auto inputs = readyInputs();
  inputs.gpuCullManagerReady = false;

  const auto plan = buildDeferredRasterFrustumCullPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(DeferredRasterFrustumCullPassPlannerTests,
     MissingOrEmptyDrawsReturnNotNeeded) {
  auto inputs = readyInputs();
  inputs.sceneSingleSidedDrawsAvailable = false;
  EXPECT_EQ(buildDeferredRasterFrustumCullPassPlan(inputs).readiness.skipReason,
            RenderPassSkipReason::NotNeeded);

  inputs = readyInputs();
  inputs.sourceDrawCount = 0u;
  EXPECT_EQ(buildDeferredRasterFrustumCullPassPlan(inputs).readiness.skipReason,
            RenderPassSkipReason::NotNeeded);
}

TEST(DeferredRasterFrustumCullPassPlannerTests,
     MissingCameraReturnsMissingCameraBuffer) {
  auto inputs = readyInputs();
  inputs.cameraBufferReady = false;

  const auto plan = buildDeferredRasterFrustumCullPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::MissingResource);
  EXPECT_EQ(plan.readiness.blockingResource, RenderResourceId::CameraBuffer);
}

TEST(DeferredRasterFrustumCullPassPlannerTests,
     MissingObjectReturnsMissingObjectBuffer) {
  auto inputs = readyInputs();
  inputs.objectBufferReady = false;

  const auto plan = buildDeferredRasterFrustumCullPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::MissingResource);
  EXPECT_EQ(plan.readiness.blockingResource, RenderResourceId::ObjectBuffer);
}

TEST(DeferredRasterFrustumCullPassPlannerTests,
     ReadyInputsProduceActivePlan) {
  const auto plan = buildDeferredRasterFrustumCullPassPlan(readyInputs());

  EXPECT_TRUE(plan.active);
  EXPECT_TRUE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::None);
  EXPECT_EQ(plan.drawCount, readyInputs().sourceDrawCount);
  EXPECT_TRUE(plan.updateObjectDescriptor);
  EXPECT_EQ(plan.freezeAction, DeferredRasterFrustumCullFreezeAction::None);
}

TEST(DeferredRasterFrustumCullPassPlannerTests,
     FreezeActionTracksDebugFreezeState) {
  auto inputs = readyInputs();
  inputs.debugFreezeCulling = true;
  inputs.cullingFrozen = false;
  EXPECT_EQ(buildDeferredRasterFrustumCullPassPlan(inputs).freezeAction,
            DeferredRasterFrustumCullFreezeAction::Freeze);

  inputs.debugFreezeCulling = false;
  inputs.cullingFrozen = true;
  EXPECT_EQ(buildDeferredRasterFrustumCullPassPlan(inputs).freezeAction,
            DeferredRasterFrustumCullFreezeAction::Unfreeze);

  inputs.debugFreezeCulling = true;
  inputs.cullingFrozen = true;
  EXPECT_EQ(buildDeferredRasterFrustumCullPassPlan(inputs).freezeAction,
            DeferredRasterFrustumCullFreezeAction::None);
}
