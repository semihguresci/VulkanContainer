#include "Container/renderer/deferred/DeferredRasterHiZDepthTransitionRecorder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using container::renderer::DeferredRasterHiZDepthTransitionInputs;
using container::renderer::buildDeferredRasterHiZDepthTransitionPlan;
using container::renderer::
    recordDeferredRasterHiZDepthToAttachmentTransitionCommands;
using container::renderer::
    recordDeferredRasterHiZDepthToSamplingTransitionCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

DeferredRasterHiZDepthTransitionInputs readyInputs() {
  return {.depthStencilImage = fakeHandle<VkImage>(0x1)};
}

} // namespace

TEST(DeferredRasterHiZDepthTransitionRecorderTests,
     NullDepthImageProducesInactivePlan) {
  const auto plan = buildDeferredRasterHiZDepthTransitionPlan({});

  EXPECT_FALSE(plan.active);
  EXPECT_EQ(plan.depthToSampling.barrier.image, VK_NULL_HANDLE);
  EXPECT_EQ(plan.depthToAttachment.barrier.image, VK_NULL_HANDLE);
}

TEST(DeferredRasterHiZDepthTransitionRecorderTests,
     ActivePlanBuildsDepthToSamplingTransition) {
  const auto inputs = readyInputs();
  const auto plan = buildDeferredRasterHiZDepthTransitionPlan(inputs);

  ASSERT_TRUE(plan.active);
  const auto &step = plan.depthToSampling;
  EXPECT_EQ(step.srcStageMask,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
  EXPECT_EQ(step.dstStageMask, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  EXPECT_EQ(step.barrier.srcAccessMask,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
  EXPECT_EQ(step.barrier.dstAccessMask, VK_ACCESS_SHADER_READ_BIT);
  EXPECT_EQ(step.barrier.oldLayout,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(step.barrier.newLayout,
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(step.barrier.image, inputs.depthStencilImage);
}

TEST(DeferredRasterHiZDepthTransitionRecorderTests,
     ActivePlanBuildsDepthToAttachmentTransition) {
  const auto inputs = readyInputs();
  const auto plan = buildDeferredRasterHiZDepthTransitionPlan(inputs);

  ASSERT_TRUE(plan.active);
  const auto &step = plan.depthToAttachment;
  EXPECT_EQ(step.srcStageMask, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  EXPECT_EQ(step.dstStageMask,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
  EXPECT_EQ(step.barrier.srcAccessMask, VK_ACCESS_SHADER_READ_BIT);
  EXPECT_EQ(step.barrier.dstAccessMask,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
  EXPECT_EQ(step.barrier.oldLayout,
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(step.barrier.newLayout,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(step.barrier.image, inputs.depthStencilImage);
}

TEST(DeferredRasterHiZDepthTransitionRecorderTests,
     BothStepsUseDepthStencilSingleMipLayerAndIgnoredQueues) {
  const auto plan = buildDeferredRasterHiZDepthTransitionPlan(readyInputs());

  ASSERT_TRUE(plan.active);
  const std::array steps{plan.depthToSampling, plan.depthToAttachment};
  for (const auto &step : steps) {
    EXPECT_EQ(step.barrier.subresourceRange.aspectMask,
              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    EXPECT_EQ(step.barrier.subresourceRange.baseMipLevel, 0u);
    EXPECT_EQ(step.barrier.subresourceRange.levelCount, 1u);
    EXPECT_EQ(step.barrier.subresourceRange.baseArrayLayer, 0u);
    EXPECT_EQ(step.barrier.subresourceRange.layerCount, 1u);
    EXPECT_EQ(step.barrier.srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
    EXPECT_EQ(step.barrier.dstQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
  }
}

TEST(DeferredRasterHiZDepthTransitionRecorderTests,
     RecordersRejectNullCommandBufferOrInactivePlan) {
  const auto activePlan = buildDeferredRasterHiZDepthTransitionPlan(readyInputs());
  const auto cmd = fakeHandle<VkCommandBuffer>(0x2);

  EXPECT_FALSE(recordDeferredRasterHiZDepthToSamplingTransitionCommands(
      VK_NULL_HANDLE, activePlan));
  EXPECT_FALSE(recordDeferredRasterHiZDepthToAttachmentTransitionCommands(
      VK_NULL_HANDLE, activePlan));
  EXPECT_FALSE(recordDeferredRasterHiZDepthToSamplingTransitionCommands(cmd,
                                                                        {}));
  EXPECT_FALSE(recordDeferredRasterHiZDepthToAttachmentTransitionCommands(cmd,
                                                                          {}));
}
