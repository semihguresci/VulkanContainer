#include "Container/renderer/deferred/DeferredRasterHiZPassPlanner.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::DeferredRasterHiZPassPlanInputs;
using container::renderer::RenderPassSkipReason;
using container::renderer::RenderResourceId;
using container::renderer::buildDeferredRasterHiZPassPlan;

DeferredRasterHiZPassPlanInputs readyInputs() {
  return {.gpuCullManagerReady = true,
          .frameReady = true,
          .depthSamplingViewReady = true,
          .depthSamplerReady = true,
          .depthStencilImageReady = true};
}

} // namespace

TEST(DeferredRasterHiZPassPlannerTests, UnreadyManagerReturnsNotNeeded) {
  auto inputs = readyInputs();
  inputs.gpuCullManagerReady = false;

  const auto plan = buildDeferredRasterHiZPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(DeferredRasterHiZPassPlannerTests,
     MissingFrameOrDepthResourcesReturnMissingSceneDepth) {
  auto inputs = readyInputs();
  inputs.frameReady = false;
  EXPECT_EQ(buildDeferredRasterHiZPassPlan(inputs).readiness.skipReason,
            RenderPassSkipReason::MissingResource);
  EXPECT_EQ(buildDeferredRasterHiZPassPlan(inputs).readiness.blockingResource,
            RenderResourceId::SceneDepth);

  inputs = readyInputs();
  inputs.depthSamplingViewReady = false;
  EXPECT_EQ(buildDeferredRasterHiZPassPlan(inputs).readiness.blockingResource,
            RenderResourceId::SceneDepth);

  inputs = readyInputs();
  inputs.depthSamplerReady = false;
  EXPECT_EQ(buildDeferredRasterHiZPassPlan(inputs).readiness.blockingResource,
            RenderResourceId::SceneDepth);

  inputs = readyInputs();
  inputs.depthStencilImageReady = false;
  EXPECT_EQ(buildDeferredRasterHiZPassPlan(inputs).readiness.blockingResource,
            RenderResourceId::SceneDepth);
}

TEST(DeferredRasterHiZPassPlannerTests, ReadyInputsProduceActivePlan) {
  const auto plan = buildDeferredRasterHiZPassPlan(readyInputs());

  EXPECT_TRUE(plan.active);
  EXPECT_TRUE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::None);
}
