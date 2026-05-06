#include "Container/renderer/deferred/DeferredRasterSceneColorReadBarrierRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::DeferredRasterSceneColorReadBarrierInputs;
using container::renderer::buildDeferredRasterSceneColorReadBarrierPlan;
using container::renderer::recordDeferredRasterSceneColorReadBarrierCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

} // namespace

TEST(DeferredRasterSceneColorReadBarrierRecorderTests,
     NullSceneColorImageReturnsInactivePlan) {
  const auto plan = buildDeferredRasterSceneColorReadBarrierPlan(
      DeferredRasterSceneColorReadBarrierInputs{});

  EXPECT_FALSE(plan.active);
}

TEST(DeferredRasterSceneColorReadBarrierRecorderTests,
     ActivePlanPreservesSceneColorReadBarrierContract) {
  const VkImage sceneColor = fakeHandle<VkImage>(0x1);

  const auto plan = buildDeferredRasterSceneColorReadBarrierPlan(
      {.sceneColorImage = sceneColor});

  ASSERT_TRUE(plan.active);
  EXPECT_EQ(plan.srcStageMask, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  EXPECT_EQ(plan.dstStageMask, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  EXPECT_EQ(plan.barrier.srcAccessMask, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  EXPECT_EQ(plan.barrier.dstAccessMask, VK_ACCESS_SHADER_READ_BIT);
  EXPECT_EQ(plan.barrier.oldLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EXPECT_EQ(plan.barrier.newLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  EXPECT_EQ(plan.barrier.srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
  EXPECT_EQ(plan.barrier.dstQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
  EXPECT_EQ(plan.barrier.image, sceneColor);
  EXPECT_EQ(plan.barrier.subresourceRange.aspectMask,
            VK_IMAGE_ASPECT_COLOR_BIT);
  EXPECT_EQ(plan.barrier.subresourceRange.levelCount, 1u);
  EXPECT_EQ(plan.barrier.subresourceRange.layerCount, 1u);
}

TEST(DeferredRasterSceneColorReadBarrierRecorderTests,
     RecorderRejectsNullCommandBufferOrInactivePlan) {
  const auto activePlan = buildDeferredRasterSceneColorReadBarrierPlan(
      {.sceneColorImage = fakeHandle<VkImage>(0x1)});

  EXPECT_FALSE(recordDeferredRasterSceneColorReadBarrierCommands(
      VK_NULL_HANDLE, activePlan));
  EXPECT_FALSE(recordDeferredRasterSceneColorReadBarrierCommands(
      fakeHandle<VkCommandBuffer>(0x2), {}));
}
