#include "Container/renderer/shadow/ShadowCullPassRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::RenderPassReadiness;
using container::renderer::ShadowCullPassPlan;
using container::renderer::ShadowCullPassRecordInputs;
using container::renderer::recordShadowCullPassCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

ShadowCullPassPlan activePlan() {
  return {.active = true, .readiness = RenderPassReadiness{}, .drawCount = 4u};
}

} // namespace

TEST(ShadowCullPassRecorderTests, NullCommandBufferReturnsFalse) {
  EXPECT_FALSE(recordShadowCullPassCommands(
      VK_NULL_HANDLE, {.shadowCullManager =
                           reinterpret_cast<container::renderer::ShadowCullManager *>(0x1),
                       .plan = activePlan()}));
}

TEST(ShadowCullPassRecorderTests, NullManagerReturnsFalse) {
  EXPECT_FALSE(recordShadowCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x2), {.plan = activePlan()}));
}

TEST(ShadowCullPassRecorderTests, InactivePlanReturnsFalse) {
  EXPECT_FALSE(recordShadowCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x2),
      {.shadowCullManager =
           reinterpret_cast<container::renderer::ShadowCullManager *>(0x1),
       .plan = {}}));
}

TEST(ShadowCullPassRecorderTests, NotReadyPlanReturnsFalse) {
  ShadowCullPassPlan plan = activePlan();
  plan.readiness.ready = false;

  EXPECT_FALSE(recordShadowCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x2),
      {.shadowCullManager =
           reinterpret_cast<container::renderer::ShadowCullManager *>(0x1),
       .plan = plan}));
}

TEST(ShadowCullPassRecorderTests, ZeroDrawCountReturnsFalse) {
  ShadowCullPassPlan plan = activePlan();
  plan.drawCount = 0u;

  EXPECT_FALSE(recordShadowCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x2),
      {.shadowCullManager =
           reinterpret_cast<container::renderer::ShadowCullManager *>(0x1),
       .plan = plan}));
}
