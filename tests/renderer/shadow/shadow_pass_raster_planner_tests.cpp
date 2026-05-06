#include "Container/renderer/shadow/ShadowPassRasterPlanner.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::ShadowPassRasterPlanInputs;
using container::renderer::buildShadowPassRasterPlan;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

ShadowPassRasterPlanInputs readyInputs() {
  return {.shadowAtlasVisible = true,
          .shadowPassRecordable = true,
          .secondaryCommandBuffer = fakeHandle<VkCommandBuffer>(0x1)};
}

} // namespace

TEST(ShadowPassRasterPlannerTests, HiddenAtlasReturnsInactive) {
  auto inputs = readyInputs();
  inputs.shadowAtlasVisible = false;

  EXPECT_FALSE(buildShadowPassRasterPlan(inputs).active);
}

TEST(ShadowPassRasterPlannerTests, NonRecordablePassReturnsInactive) {
  auto inputs = readyInputs();
  inputs.shadowPassRecordable = false;

  EXPECT_FALSE(buildShadowPassRasterPlan(inputs).active);
}

TEST(ShadowPassRasterPlannerTests, InlinePlanWhenSecondaryDisabled) {
  auto inputs = readyInputs();
  inputs.useSecondaryCommandBuffer = false;

  const auto plan = buildShadowPassRasterPlan(inputs);

  ASSERT_TRUE(plan.active);
  EXPECT_FALSE(plan.scope.executeSecondary);
  EXPECT_EQ(plan.scope.contents, VK_SUBPASS_CONTENTS_INLINE);
  EXPECT_EQ(plan.secondaryCommandBuffer, VK_NULL_HANDLE);
}

TEST(ShadowPassRasterPlannerTests,
     SecondaryPlanWhenSecondaryEnabledAndAvailable) {
  auto inputs = readyInputs();
  inputs.useSecondaryCommandBuffer = true;

  const auto plan = buildShadowPassRasterPlan(inputs);

  ASSERT_TRUE(plan.active);
  EXPECT_TRUE(plan.scope.executeSecondary);
  EXPECT_EQ(plan.scope.contents, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
  EXPECT_EQ(plan.secondaryCommandBuffer, inputs.secondaryCommandBuffer);
}

TEST(ShadowPassRasterPlannerTests,
     NullSecondaryCommandBufferFallsBackToInlinePlan) {
  auto inputs = readyInputs();
  inputs.useSecondaryCommandBuffer = true;
  inputs.secondaryCommandBuffer = VK_NULL_HANDLE;

  const auto plan = buildShadowPassRasterPlan(inputs);

  ASSERT_TRUE(plan.active);
  EXPECT_FALSE(plan.scope.executeSecondary);
  EXPECT_EQ(plan.scope.contents, VK_SUBPASS_CONTENTS_INLINE);
  EXPECT_EQ(plan.secondaryCommandBuffer, VK_NULL_HANDLE);
}
