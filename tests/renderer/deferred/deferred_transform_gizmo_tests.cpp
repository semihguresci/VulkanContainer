#include "Container/renderer/deferred/DeferredRasterTransformGizmo.h"

#include "Container/utility/GuiManager.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::FrameTransformGizmoState;
using container::renderer::TransformGizmoPushConstants;
using container::renderer::deferredTransformGizmoUsesSolidArrowheads;
using container::renderer::deferredTransformGizmoVertexCount;
using container::renderer::updateDeferredTransformGizmoPushConstants;

TEST(DeferredTransformGizmoTests, VertexCountsTrackToolTopology) {
  EXPECT_EQ(deferredTransformGizmoVertexCount(
                container::ui::ViewportTool::Select),
            30u);
  EXPECT_EQ(deferredTransformGizmoVertexCount(
                container::ui::ViewportTool::Translate),
            30u);
  EXPECT_EQ(deferredTransformGizmoVertexCount(
                container::ui::ViewportTool::Scale),
            102u);
  EXPECT_EQ(deferredTransformGizmoVertexCount(
                container::ui::ViewportTool::Rotate),
            408u);
}

TEST(DeferredTransformGizmoTests, SolidArrowheadsOnlyApplyToTranslateTool) {
  EXPECT_TRUE(deferredTransformGizmoUsesSolidArrowheads(
      container::ui::ViewportTool::Translate));
  EXPECT_FALSE(deferredTransformGizmoUsesSolidArrowheads(
      container::ui::ViewportTool::Select));
  EXPECT_FALSE(deferredTransformGizmoUsesSolidArrowheads(
      container::ui::ViewportTool::Rotate));
  EXPECT_FALSE(deferredTransformGizmoUsesSolidArrowheads(
      container::ui::ViewportTool::Scale));
}

TEST(DeferredTransformGizmoTests, PushConstantsMirrorFrameState) {
  FrameTransformGizmoState gizmo{};
  gizmo.tool = container::ui::ViewportTool::Rotate;
  gizmo.transformSpace = container::ui::TransformSpace::Local;
  gizmo.activeAxis = container::ui::TransformAxis::Y;
  gizmo.origin = {1.0f, 2.0f, 3.0f};
  gizmo.scale = 4.0f;
  gizmo.axisX = {1.0f, 0.0f, 0.0f};
  gizmo.axisY = {0.0f, 0.5f, 0.0f};
  gizmo.axisZ = {0.0f, 0.0f, 0.25f};

  TransformGizmoPushConstants pc{};
  pc.padding0 = 99u;
  updateDeferredTransformGizmoPushConstants(pc, gizmo);

  EXPECT_FLOAT_EQ(pc.originScale.x, 1.0f);
  EXPECT_FLOAT_EQ(pc.originScale.y, 2.0f);
  EXPECT_FLOAT_EQ(pc.originScale.z, 3.0f);
  EXPECT_FLOAT_EQ(pc.originScale.w, 4.0f);
  EXPECT_FLOAT_EQ(pc.axisY.y, 0.5f);
  EXPECT_FLOAT_EQ(pc.axisZ.z, 0.25f);
  EXPECT_EQ(pc.tool, static_cast<uint32_t>(container::ui::ViewportTool::Rotate));
  EXPECT_EQ(pc.activeAxis,
            static_cast<uint32_t>(container::ui::TransformAxis::Y));
  EXPECT_EQ(pc.transformSpace,
            static_cast<uint32_t>(container::ui::TransformSpace::Local));
  EXPECT_EQ(pc.padding0, 0u);
}

}  // namespace
