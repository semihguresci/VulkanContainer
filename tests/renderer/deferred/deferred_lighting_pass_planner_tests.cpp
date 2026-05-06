#include "Container/renderer/deferred/DeferredLightingPassPlanner.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::buildDeferredLightingPassPlan;

TEST(DeferredLightingPassPlannerTests,
     RenderAreaPreservesInputExtentAndZeroOffset) {
  const VkExtent2D extent{.width = 1920u, .height = 1080u};

  const auto plan = buildDeferredLightingPassPlan(extent);

  EXPECT_EQ(plan.renderArea.offset.x, 0);
  EXPECT_EQ(plan.renderArea.offset.y, 0);
  EXPECT_EQ(plan.renderArea.extent.width, extent.width);
  EXPECT_EQ(plan.renderArea.extent.height, extent.height);
}

TEST(DeferredLightingPassPlannerTests,
     LightingPassClearValuesPreserveColorAndReverseZDepth) {
  const auto plan = buildDeferredLightingPassPlan({.width = 1u, .height = 1u});

  ASSERT_EQ(plan.clearValues.size(), 2u);
  EXPECT_FLOAT_EQ(plan.clearValues[0].color.float32[0], 0.0f);
  EXPECT_FLOAT_EQ(plan.clearValues[0].color.float32[1], 0.0f);
  EXPECT_FLOAT_EQ(plan.clearValues[0].color.float32[2], 0.0f);
  EXPECT_FLOAT_EQ(plan.clearValues[0].color.float32[3], 1.0f);
  EXPECT_FLOAT_EQ(plan.clearValues[1].depthStencil.depth, 0.0f);
  EXPECT_EQ(plan.clearValues[1].depthStencil.stencil, 0u);
}

TEST(DeferredLightingPassPlannerTests,
     SelectionStencilClearPreservesStencilOnlyContract) {
  const auto plan = buildDeferredLightingPassPlan({.width = 1u, .height = 1u});

  EXPECT_EQ(plan.selectionStencilClearAttachment.aspectMask,
            VK_IMAGE_ASPECT_STENCIL_BIT);
  EXPECT_FLOAT_EQ(
      plan.selectionStencilClearAttachment.clearValue.depthStencil.depth,
      0.0f);
  EXPECT_EQ(plan.selectionStencilClearAttachment.clearValue.depthStencil.stencil,
            0u);
}

TEST(DeferredLightingPassPlannerTests,
     SelectionStencilClearRectPreservesInputExtentAndLayerContract) {
  const VkExtent2D extent{.width = 640u, .height = 480u};

  const auto plan = buildDeferredLightingPassPlan(extent);

  EXPECT_EQ(plan.selectionStencilClearRect.rect.offset.x, 0);
  EXPECT_EQ(plan.selectionStencilClearRect.rect.offset.y, 0);
  EXPECT_EQ(plan.selectionStencilClearRect.rect.extent.width, extent.width);
  EXPECT_EQ(plan.selectionStencilClearRect.rect.extent.height, extent.height);
  EXPECT_EQ(plan.selectionStencilClearRect.baseArrayLayer, 0u);
  EXPECT_EQ(plan.selectionStencilClearRect.layerCount, 1u);
}

TEST(DeferredLightingPassPlannerTests, ZeroExtentIsPreserved) {
  const VkExtent2D extent{.width = 0u, .height = 0u};

  const auto plan = buildDeferredLightingPassPlan(extent);

  EXPECT_EQ(plan.renderArea.extent.width, 0u);
  EXPECT_EQ(plan.renderArea.extent.height, 0u);
  EXPECT_EQ(plan.selectionStencilClearRect.rect.extent.width, 0u);
  EXPECT_EQ(plan.selectionStencilClearRect.rect.extent.height, 0u);
}

} // namespace
