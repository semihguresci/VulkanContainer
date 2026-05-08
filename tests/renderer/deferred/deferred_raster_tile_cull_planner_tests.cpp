#include "Container/renderer/deferred/DeferredRasterTileCullPlanner.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::DeferredRasterTileCullPlanInputs;
using container::renderer::RenderPassSkipReason;
using container::renderer::RenderResourceId;
using container::renderer::buildDeferredRasterTileCullPlan;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

DeferredRasterTileCullPlanInputs readyInputs() {
  return {.tileCullDisplayMode = true,
          .tiledLightingReady = true,
          .frameAvailable = true,
          .depthSamplingView = fakeHandle<VkImageView>(0x1),
          .cameraBuffer = fakeHandle<VkBuffer>(0x2),
          .cameraBufferSize = 64u,
          .screenExtent = {.width = 1920u, .height = 1080u},
          .cameraNear = 0.1f,
          .cameraFar = 500.0f};
}

} // namespace

TEST(DeferredRasterTileCullPlannerTests,
     HiddenTileCullDisplayModeReturnsNotNeeded) {
  auto inputs = readyInputs();
  inputs.tileCullDisplayMode = false;

  const auto plan = buildDeferredRasterTileCullPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(DeferredRasterTileCullPlannerTests,
     UnreadyTiledLightingReturnsNotNeeded) {
  auto inputs = readyInputs();
  inputs.tiledLightingReady = false;

  const auto plan = buildDeferredRasterTileCullPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_FALSE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::NotNeeded);
}

TEST(DeferredRasterTileCullPlannerTests,
     MissingFrameOrDepthReturnsMissingSceneDepth) {
  auto inputs = readyInputs();
  inputs.frameAvailable = false;
  EXPECT_EQ(buildDeferredRasterTileCullPlan(inputs).readiness.skipReason,
            RenderPassSkipReason::MissingResource);
  EXPECT_EQ(buildDeferredRasterTileCullPlan(inputs).readiness.blockingResource,
            RenderResourceId::SceneDepth);

  inputs = readyInputs();
  inputs.depthSamplingView = VK_NULL_HANDLE;
  EXPECT_EQ(buildDeferredRasterTileCullPlan(inputs).readiness.blockingResource,
            RenderResourceId::SceneDepth);
}

TEST(DeferredRasterTileCullPlannerTests,
     MissingCameraReturnsMissingSceneDepth) {
  auto inputs = readyInputs();
  inputs.cameraBuffer = VK_NULL_HANDLE;
  EXPECT_EQ(buildDeferredRasterTileCullPlan(inputs).readiness.blockingResource,
            RenderResourceId::SceneDepth);

  inputs = readyInputs();
  inputs.cameraBufferSize = 0u;
  EXPECT_EQ(buildDeferredRasterTileCullPlan(inputs).readiness.blockingResource,
            RenderResourceId::SceneDepth);
}

TEST(DeferredRasterTileCullPlannerTests, ReadyInputsProduceActivePlan) {
  const auto inputs = readyInputs();
  const auto plan = buildDeferredRasterTileCullPlan(inputs);

  EXPECT_TRUE(plan.active);
  EXPECT_TRUE(plan.readiness.ready);
  EXPECT_EQ(plan.readiness.skipReason, RenderPassSkipReason::None);
  EXPECT_EQ(plan.screenExtent.width, inputs.screenExtent.width);
  EXPECT_EQ(plan.screenExtent.height, inputs.screenExtent.height);
  EXPECT_EQ(plan.cameraBuffer, inputs.cameraBuffer);
  EXPECT_EQ(plan.cameraBufferSize, inputs.cameraBufferSize);
  EXPECT_EQ(plan.depthSamplingView, inputs.depthSamplingView);
  EXPECT_FLOAT_EQ(plan.cameraNear, inputs.cameraNear);
  EXPECT_FLOAT_EQ(plan.cameraFar, inputs.cameraFar);
}
