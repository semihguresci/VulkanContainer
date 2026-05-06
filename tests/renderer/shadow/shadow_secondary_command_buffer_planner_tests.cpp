#include "Container/renderer/shadow/ShadowSecondaryCommandBufferPlanner.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::ShadowSecondaryCommandBufferPlanInputs;
using container::renderer::buildShadowSecondaryCommandBufferPlan;
using container::renderer::kMinShadowSecondaryCommandBufferCpuCommands;

[[nodiscard]] ShadowSecondaryCommandBufferPlanInputs readyInputs() {
  return {.secondaryCommandBuffersEnabled = true,
          .shadowPassRecordable = true,
          .secondaryCommandBufferAvailable = true,
          .cpuCommandCount = kMinShadowSecondaryCommandBufferCpuCommands};
}

TEST(ShadowSecondaryCommandBufferPlannerTests,
     GpuFilteredBimMeshShadowPathSuppressesSecondaryUse) {
  auto inputs = readyInputs();
  inputs.usesGpuFilteredBimMeshShadowPath = true;

  EXPECT_FALSE(
      buildShadowSecondaryCommandBufferPlan(inputs).useSecondaryCommandBuffer);
}

TEST(ShadowSecondaryCommandBufferPlannerTests,
     DisabledSecondaryCommandBuffersSuppressSecondaryUse) {
  auto inputs = readyInputs();
  inputs.secondaryCommandBuffersEnabled = false;

  EXPECT_FALSE(
      buildShadowSecondaryCommandBufferPlan(inputs).useSecondaryCommandBuffer);
}

TEST(ShadowSecondaryCommandBufferPlannerTests,
     NonRecordableShadowPassSuppressesSecondaryUse) {
  auto inputs = readyInputs();
  inputs.shadowPassRecordable = false;

  EXPECT_FALSE(
      buildShadowSecondaryCommandBufferPlan(inputs).useSecondaryCommandBuffer);
}

TEST(ShadowSecondaryCommandBufferPlannerTests,
     MissingSecondaryCommandBufferSuppressesSecondaryUse) {
  auto inputs = readyInputs();
  inputs.secondaryCommandBufferAvailable = false;

  EXPECT_FALSE(
      buildShadowSecondaryCommandBufferPlan(inputs).useSecondaryCommandBuffer);
}

TEST(ShadowSecondaryCommandBufferPlannerTests,
     CpuCommandCountBelowThresholdSuppressesSecondaryUse) {
  auto inputs = readyInputs();
  inputs.cpuCommandCount = kMinShadowSecondaryCommandBufferCpuCommands - 1u;

  EXPECT_FALSE(
      buildShadowSecondaryCommandBufferPlan(inputs).useSecondaryCommandBuffer);
}

TEST(ShadowSecondaryCommandBufferPlannerTests,
     CpuCommandCountAtThresholdEnablesSecondaryUseWhenReady) {
  EXPECT_TRUE(buildShadowSecondaryCommandBufferPlan(readyInputs())
                  .useSecondaryCommandBuffer);
}

} // namespace
