#include "Container/renderer/shadow/ShadowPassScopePlanner.h"
#include "Container/utility/SceneData.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::buildShadowPassScopePlan;

TEST(ShadowPassScopePlannerTests,
     RenderAreaUsesShadowMapExtentAndZeroOffset) {
  const auto plan = buildShadowPassScopePlan(false);

  EXPECT_EQ(plan.renderArea.offset.x, 0);
  EXPECT_EQ(plan.renderArea.offset.y, 0);
  EXPECT_EQ(plan.renderArea.extent.width, container::gpu::kShadowMapResolution);
  EXPECT_EQ(plan.renderArea.extent.height, container::gpu::kShadowMapResolution);
}

TEST(ShadowPassScopePlannerTests,
     ClearValuePreservesReverseZDepthContract) {
  const auto plan = buildShadowPassScopePlan(false);

  ASSERT_EQ(plan.clearValues.size(), 1u);
  EXPECT_FLOAT_EQ(plan.clearValues[0].depthStencil.depth, 0.0f);
  EXPECT_EQ(plan.clearValues[0].depthStencil.stencil, 0u);
}

TEST(ShadowPassScopePlannerTests, InlinePlanUsesInlineSubpassContents) {
  const auto plan = buildShadowPassScopePlan(false);

  EXPECT_FALSE(plan.executeSecondary);
  EXPECT_EQ(plan.contents, VK_SUBPASS_CONTENTS_INLINE);
}

TEST(ShadowPassScopePlannerTests,
     SecondaryPlanUsesSecondaryCommandBufferSubpassContents) {
  const auto plan = buildShadowPassScopePlan(true);

  EXPECT_TRUE(plan.executeSecondary);
  EXPECT_EQ(plan.contents, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
}

} // namespace
