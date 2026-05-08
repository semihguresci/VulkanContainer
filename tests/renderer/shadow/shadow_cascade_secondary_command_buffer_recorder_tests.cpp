#include "Container/renderer/shadow/ShadowCascadeSecondaryCommandBufferRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::ShadowCascadeSecondaryCommandBufferPlanInputs;
using container::renderer::buildShadowCascadeSecondaryCommandBufferRecordPlan;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

ShadowCascadeSecondaryCommandBufferPlanInputs readyInputs() {
  ShadowCascadeSecondaryCommandBufferPlanInputs inputs{};
  inputs.secondaryCommandBuffersEnabled = true;
  for (uint32_t cascadeIndex = 0u;
       cascadeIndex < container::gpu::kShadowCascadeCount; ++cascadeIndex) {
    inputs.cascadePassActive[cascadeIndex] = true;
    inputs.useSecondaryCommandBuffer[cascadeIndex] = true;
    inputs.commandBuffers[cascadeIndex] =
        fakeHandle<VkCommandBuffer>(0x1000u + cascadeIndex);
  }
  return inputs;
}

} // namespace

TEST(ShadowCascadeSecondaryCommandBufferRecorderTests,
     DisabledSecondaryCommandBuffersBuildsEmptyPlan) {
  auto inputs = readyInputs();
  inputs.secondaryCommandBuffersEnabled = false;

  const auto plan = buildShadowCascadeSecondaryCommandBufferRecordPlan(inputs);

  EXPECT_TRUE(plan.empty());
  EXPECT_EQ(plan.cascadeCount, 0u);
}

TEST(ShadowCascadeSecondaryCommandBufferRecorderTests,
     PlanKeepsOnlyActiveEligibleCascadesWithCommandBuffers) {
  auto inputs = readyInputs();
  inputs.cascadePassActive[1] = false;
  inputs.useSecondaryCommandBuffer[2] = false;
  inputs.commandBuffers[3] = VK_NULL_HANDLE;

  const auto plan = buildShadowCascadeSecondaryCommandBufferRecordPlan(inputs);

  ASSERT_EQ(plan.cascadeCount, 1u);
  EXPECT_EQ(plan.cascadeIndices[0], 0u);
  EXPECT_EQ(plan.commandBuffers[0], inputs.commandBuffers[0]);
}

TEST(ShadowCascadeSecondaryCommandBufferRecorderTests,
     PlanPreservesCascadeOrder) {
  auto inputs = readyInputs();
  inputs.cascadePassActive[0] = false;
  inputs.useSecondaryCommandBuffer[2] = false;

  const auto plan = buildShadowCascadeSecondaryCommandBufferRecordPlan(inputs);

  ASSERT_EQ(plan.cascadeCount, 2u);
  EXPECT_EQ(plan.cascadeIndices[0], 1u);
  EXPECT_EQ(plan.cascadeIndices[1], 3u);
}
