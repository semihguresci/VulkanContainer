#include "Container/renderer/BimManager.h"
#include "Container/renderer/bim/BimLightingOverlayPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::BimLightingOverlayInputs;
using container::renderer::BimLightingOverlayKind;
using container::renderer::BimLightingOverlayPipeline;
using container::renderer::BimLightingOverlayPipelineReadiness;
using container::renderer::buildBimLightingOverlayPlan;
using container::renderer::DrawCommand;

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t firstIndex) {
  return {DrawCommand{.objectIndex = firstIndex,
                      .firstIndex = firstIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

[[nodiscard]] BimLightingOverlayPipelineReadiness readyPipelines() {
  return {.wireframeDepth = true,
          .wireframeNoDepth = true,
          .bimFloorPlanDepth = true,
          .bimFloorPlanNoDepth = true,
          .bimPointCloudDepth = true,
          .bimCurveDepth = true,
          .selectionMask = true,
          .selectionOutline = true};
}

[[nodiscard]] BimLightingOverlayInputs readyInputs() {
  return {.bimGeometryReady = true,
          .wireframeLayoutReady = true,
          .wireframePushConstantsReady = true,
          .wideLinesSupported = true,
          .framebufferWidth = 1280u,
          .framebufferHeight = 720u,
          .pipelines = readyPipelines()};
}

TEST(BimLightingOverlayPlannerTests, StyleRoutesPreserveSplitDrawOrder) {
  const auto opaqueSingle = drawCommands(1u);
  const auto transparentSingle = drawCommands(2u);
  const auto opaqueWinding = drawCommands(3u);
  const auto transparentWinding = drawCommands(4u);
  const auto opaqueDouble = drawCommands(5u);
  const auto transparentDouble = drawCommands(6u);
  auto inputs = readyInputs();
  inputs.points = {
      .enabled = true,
      .depthTest = true,
      .opacity = 1.4f,
      .lineWidth = 0.25f,
      .draws = {.opaqueSingleSidedDrawCommands = &opaqueSingle,
                .transparentSingleSidedDrawCommands = &transparentSingle,
                .opaqueWindingFlippedDrawCommands = &opaqueWinding,
                .transparentWindingFlippedDrawCommands = &transparentWinding,
                .opaqueDoubleSidedDrawCommands = &opaqueDouble,
                .transparentDoubleSidedDrawCommands = &transparentDouble}};

  const auto plan = buildBimLightingOverlayPlan(inputs);

  ASSERT_TRUE(plan.pointStyle.active);
  EXPECT_EQ(plan.pointStyle.kind, BimLightingOverlayKind::PointStyle);
  EXPECT_FLOAT_EQ(plan.pointStyle.opacity, 1.0f);
  EXPECT_FLOAT_EQ(plan.pointStyle.drawLineWidth, 1.0f);
  ASSERT_EQ(plan.pointStyle.routeCount, 6u);
  EXPECT_EQ(plan.pointStyle.routes[0].commands, &opaqueSingle);
  EXPECT_EQ(plan.pointStyle.routes[0].pipeline,
            BimLightingOverlayPipeline::WireframeDepth);
  EXPECT_EQ(plan.pointStyle.routes[1].commands, &transparentSingle);
  EXPECT_EQ(plan.pointStyle.routes[1].pipeline,
            BimLightingOverlayPipeline::WireframeDepth);
  EXPECT_EQ(plan.pointStyle.routes[2].commands, &opaqueWinding);
  EXPECT_EQ(plan.pointStyle.routes[2].pipeline,
            BimLightingOverlayPipeline::WireframeDepthFrontCull);
  EXPECT_EQ(plan.pointStyle.routes[3].commands, &transparentWinding);
  EXPECT_EQ(plan.pointStyle.routes[3].pipeline,
            BimLightingOverlayPipeline::WireframeDepthFrontCull);
  EXPECT_EQ(plan.pointStyle.routes[4].commands, &opaqueDouble);
  EXPECT_EQ(plan.pointStyle.routes[4].pipeline,
            BimLightingOverlayPipeline::WireframeDepth);
  EXPECT_EQ(plan.pointStyle.routes[5].commands, &transparentDouble);
  EXPECT_EQ(plan.pointStyle.routes[5].pipeline,
            BimLightingOverlayPipeline::WireframeDepth);
  EXPECT_FALSE(plan.curveStyle.active);
}

TEST(BimLightingOverlayPlannerTests, CurveStyleCanSelectNoDepthPipeline) {
  const auto winding = drawCommands(7u);
  auto inputs = readyInputs();
  inputs.curves = {
      .enabled = true,
      .depthTest = false,
      .opacity = 0.65f,
      .lineWidth = 2.5f,
      .draws = {.transparentWindingFlippedDrawCommands = &winding}};

  const auto plan = buildBimLightingOverlayPlan(inputs);

  ASSERT_TRUE(plan.curveStyle.active);
  ASSERT_EQ(plan.curveStyle.routeCount, 1u);
  EXPECT_EQ(plan.curveStyle.routes[0].commands, &winding);
  EXPECT_EQ(plan.curveStyle.routes[0].pipeline,
            BimLightingOverlayPipeline::WireframeNoDepthFrontCull);
  EXPECT_FLOAT_EQ(plan.curveStyle.drawLineWidth, 2.5f);
  EXPECT_FLOAT_EQ(plan.curveStyle.routes[0].rasterLineWidth, 2.5f);
}

TEST(BimLightingOverlayPlannerTests,
     MissingCommonPrerequisitesDisableAllWireframePlans) {
  const auto commands = drawCommands(8u);
  auto inputs = readyInputs();
  inputs.wireframeLayoutReady = false;
  inputs.points = {.enabled = true,
                   .draws = {.opaqueSingleSidedDrawCommands = &commands}};
  inputs.floorPlan = {.enabled = true, .commands = &commands};
  inputs.sceneHoverCommands = &commands;
  inputs.sceneSelectionCommands = &commands;
  inputs.nativePointHoverCommands = &commands;

  const auto plan = buildBimLightingOverlayPlan(inputs);

  EXPECT_FALSE(plan.pointStyle.active);
  EXPECT_FALSE(plan.floorPlan.active);
  EXPECT_FALSE(plan.sceneHover.active);
  EXPECT_FALSE(plan.sceneSelectionOutline.active);
  EXPECT_FALSE(plan.nativePointHover.active);
}

TEST(BimLightingOverlayPlannerTests,
     MissingBimGeometryOnlyDisablesBimScopedPlans) {
  const auto commands = drawCommands(9u);
  auto inputs = readyInputs();
  inputs.bimGeometryReady = false;
  inputs.points = {.enabled = true,
                   .draws = {.opaqueSingleSidedDrawCommands = &commands}};
  inputs.floorPlan = {.enabled = true, .commands = &commands};
  inputs.sceneHoverCommands = &commands;
  inputs.bimHoverCommands = &commands;
  inputs.sceneSelectionCommands = &commands;
  inputs.bimSelectionCommands = &commands;

  const auto plan = buildBimLightingOverlayPlan(inputs);

  EXPECT_FALSE(plan.pointStyle.active);
  EXPECT_FALSE(plan.floorPlan.active);
  EXPECT_TRUE(plan.sceneHover.active);
  EXPECT_FALSE(plan.bimHover.active);
  EXPECT_TRUE(plan.sceneSelectionOutline.active);
  EXPECT_FALSE(plan.bimSelectionOutline.active);
}

TEST(BimLightingOverlayPlannerTests,
     MissingSelectedPipelinesDisableOnlyTheirOverlay) {
  const auto commands = drawCommands(10u);
  auto inputs = readyInputs();
  inputs.pipelines.wireframeNoDepth = false;
  inputs.pipelines.bimFloorPlanNoDepth = false;
  inputs.curves = {.enabled = true,
                   .depthTest = false,
                   .draws = {.opaqueSingleSidedDrawCommands = &commands}};
  inputs.floorPlan = {.enabled = true,
                      .depthTest = false,
                      .commands = &commands};
  inputs.sceneHoverCommands = &commands;

  const auto plan = buildBimLightingOverlayPlan(inputs);

  EXPECT_FALSE(plan.curveStyle.active);
  EXPECT_FALSE(plan.floorPlan.active);
  EXPECT_TRUE(plan.sceneHover.active);
}

TEST(BimLightingOverlayPlannerTests,
     FloorPlanSanitizesOpacityAndWideLineFallback) {
  const auto commands = drawCommands(11u);
  auto inputs = readyInputs();
  inputs.wideLinesSupported = false;
  inputs.floorPlan = {.enabled = true,
                      .depthTest = false,
                      .opacity = -0.5f,
                      .lineWidth = 3.0f,
                      .commands = &commands};

  const auto plan = buildBimLightingOverlayPlan(inputs);

  ASSERT_TRUE(plan.floorPlan.active);
  EXPECT_EQ(plan.floorPlan.kind, BimLightingOverlayKind::FloorPlan);
  EXPECT_EQ(plan.floorPlan.pipeline,
            BimLightingOverlayPipeline::BimFloorPlanNoDepth);
  EXPECT_FLOAT_EQ(plan.floorPlan.opacity, 0.0f);
  EXPECT_FLOAT_EQ(plan.floorPlan.drawLineWidth, 3.0f);
  EXPECT_FLOAT_EQ(plan.floorPlan.rasterLineWidth, 1.0f);
}

TEST(BimLightingOverlayPlannerTests,
     InteractionAndNativeHighlightStylesMatchRendererPolicy) {
  const auto sceneHover = drawCommands(12u);
  const auto bimHover = drawCommands(13u);
  const auto nativePointHover = drawCommands(14u);
  const auto nativeCurveHover = drawCommands(15u);
  const auto nativePointSelection = drawCommands(16u);
  const auto nativeCurveSelection = drawCommands(17u);
  auto inputs = readyInputs();
  inputs.sceneHoverCommands = &sceneHover;
  inputs.bimHoverCommands = &bimHover;
  inputs.nativePointHoverCommands = &nativePointHover;
  inputs.nativeCurveHoverCommands = &nativeCurveHover;
  inputs.nativePointSelectionCommands = &nativePointSelection;
  inputs.nativeCurveSelectionCommands = &nativeCurveSelection;
  inputs.nativePointSize = 4.0f;
  inputs.nativeCurveLineWidth = 2.5f;

  const auto plan = buildBimLightingOverlayPlan(inputs);

  ASSERT_TRUE(plan.sceneHover.active);
  EXPECT_FLOAT_EQ(plan.sceneHover.opacity, 0.7f);
  EXPECT_FLOAT_EQ(plan.sceneHover.drawLineWidth, 1.5f);
  ASSERT_TRUE(plan.bimHover.active);
  EXPECT_FLOAT_EQ(plan.bimHover.drawLineWidth, 1.5f);
  ASSERT_TRUE(plan.nativePointHover.active);
  EXPECT_FLOAT_EQ(plan.nativePointHover.opacity, 0.85f);
  EXPECT_FLOAT_EQ(plan.nativePointHover.drawLineWidth, 6.0f);
  EXPECT_FALSE(plan.nativePointHover.rasterLineWidthApplies);
  ASSERT_TRUE(plan.nativeCurveHover.active);
  EXPECT_FLOAT_EQ(plan.nativeCurveHover.drawLineWidth, 3.5f);
  EXPECT_TRUE(plan.nativeCurveHover.rasterLineWidthApplies);
  ASSERT_TRUE(plan.nativePointSelection.active);
  EXPECT_FLOAT_EQ(plan.nativePointSelection.drawLineWidth, 7.0f);
  ASSERT_TRUE(plan.nativeCurveSelection.active);
  EXPECT_FLOAT_EQ(plan.nativeCurveSelection.drawLineWidth, 4.5f);
}

TEST(BimLightingOverlayPlannerTests,
     SelectionOutlineRequiresBothPipelinesAndSanitizesExtent) {
  const auto selected = drawCommands(18u);
  auto inputs = readyInputs();
  inputs.framebufferWidth = 0u;
  inputs.framebufferHeight = 0u;
  inputs.sceneSelectionCommands = &selected;
  inputs.pipelines.selectionOutline = false;

  EXPECT_FALSE(buildBimLightingOverlayPlan(inputs).sceneSelectionOutline.active);

  inputs.pipelines.selectionOutline = true;
  const auto plan = buildBimLightingOverlayPlan(inputs);

  ASSERT_TRUE(plan.sceneSelectionOutline.active);
  EXPECT_EQ(plan.sceneSelectionOutline.commands, &selected);
  EXPECT_FLOAT_EQ(plan.sceneSelectionOutline.maskLineWidth, 1.0f);
  EXPECT_FLOAT_EQ(plan.sceneSelectionOutline.outlineLineWidth, 5.0f);
  EXPECT_EQ(plan.sceneSelectionOutline.framebufferWidth, 1u);
  EXPECT_EQ(plan.sceneSelectionOutline.framebufferHeight, 1u);
}

} // namespace
