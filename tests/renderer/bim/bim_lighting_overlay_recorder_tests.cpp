#include "Container/renderer/bim/BimLightingOverlayRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::renderer::BimLightingOverlayDrawPlan;
using container::renderer::BimLightingOverlayFrameRecordInputs;
using container::renderer::BimLightingOverlayPipeline;
using container::renderer::BimLightingOverlayRecordInputs;
using container::renderer::BimLightingOverlayStylePlan;
using container::renderer::BimLightingSelectionOutlinePlan;
using container::renderer::buildBimLightingOverlayFramePlan;
using container::renderer::buildBimLightingOverlayFramePlanInputs;
using container::renderer::DebugOverlayRenderer;
using container::renderer::DrawCommand;
using container::renderer::recordBimLightingOverlayCommands;
using container::renderer::recordBimLightingOverlayFrameCommands;
using container::renderer::WireframePushConstants;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::vector<DrawCommand> oneDrawCommand() {
  return {DrawCommand{.objectIndex = 7u, .firstIndex = 0u, .indexCount = 3u}};
}

BimLightingOverlayRecordInputs
requiredInputs(const container::renderer::BimLightingOverlayPlan &plan,
               const WireframePushConstants &pushConstants,
               const DebugOverlayRenderer &debugOverlay) {
  return {.plan = &plan,
          .wireframeLayout = fakeHandle<VkPipelineLayout>(0x1),
          .wireframePushConstants = &pushConstants,
          .debugOverlay = &debugOverlay};
}

} // namespace

TEST(BimLightingOverlayRecorderTests, NullOrInactivePlanReturnsFalse) {
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, {}));

  container::renderer::BimLightingOverlayPlan plan{};
  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};
  const BimLightingOverlayRecordInputs inputs =
      requiredInputs(plan, pushConstants, debugOverlay);

  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimLightingOverlayRecorderTests,
     MissingWireframeLayoutPushConstantsOrDebugOverlayReturnsFalse) {
  container::renderer::BimLightingOverlayPlan plan{};
  plan.floorPlan.active = true;

  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};

  BimLightingOverlayRecordInputs inputs =
      requiredInputs(plan, pushConstants, debugOverlay);
  inputs.wireframeLayout = VK_NULL_HANDLE;
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));

  inputs = requiredInputs(plan, pushConstants, debugOverlay);
  inputs.wireframePushConstants = nullptr;
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));

  inputs = requiredInputs(plan, pushConstants, debugOverlay);
  inputs.debugOverlay = nullptr;
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimLightingOverlayRecorderTests,
     ActiveStylePlanWithMissingBimGeometryReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  container::renderer::BimLightingOverlayPlan plan{};
  BimLightingOverlayStylePlan &style = plan.pointStyle;
  style.active = true;
  style.routeCount = 1u;
  style.routes[0].commands = &commands;

  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};
  const BimLightingOverlayRecordInputs inputs =
      requiredInputs(plan, pushConstants, debugOverlay);

  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimLightingOverlayRecorderTests,
     ActiveSceneOverlayWithMissingSceneGeometryReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  container::renderer::BimLightingOverlayPlan plan{};
  BimLightingOverlayDrawPlan &sceneHover = plan.sceneHover;
  sceneHover.active = true;
  sceneHover.pipeline = BimLightingOverlayPipeline::WireframeDepth;
  sceneHover.commands = &commands;

  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};
  BimLightingOverlayRecordInputs inputs =
      requiredInputs(plan, pushConstants, debugOverlay);
  inputs.pipelines.wireframeDepth = fakeHandle<VkPipeline>(0x2);

  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimLightingOverlayRecorderTests,
     SelectionOutlineRequiresMaskAndOutlinePipelinesBeforeRecording) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  container::renderer::BimLightingOverlayPlan plan{};
  BimLightingSelectionOutlinePlan &selection = plan.sceneSelectionOutline;
  selection.active = true;
  selection.commands = &commands;

  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};
  BimLightingOverlayRecordInputs inputs =
      requiredInputs(plan, pushConstants, debugOverlay);
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));

  inputs.pipelines.selectionMask = fakeHandle<VkPipeline>(0x3);
  EXPECT_FALSE(recordBimLightingOverlayCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimLightingOverlayRecorderTests,
     FrameOverlayInputsBuildPlanAndPreserveRecordingHandles) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  WireframePushConstants pushConstants{};
  DebugOverlayRenderer debugOverlay{};
  const BimLightingOverlayFrameRecordInputs inputs{
      .bimGeometryReady = true,
      .floorPlan = {.enabled = true, .depthTest = true, .lineWidth = 2.0f},
      .draws = {.floorPlan = &commands},
      .pipelines = {.bimFloorPlanDepth = fakeHandle<VkPipeline>(0x4)},
      .wireframeLayout = fakeHandle<VkPipelineLayout>(0x5),
      .wireframePushConstants = &pushConstants,
      .debugOverlay = &debugOverlay};

  const auto planInputs = buildBimLightingOverlayFramePlanInputs(inputs);
  const auto plan = buildBimLightingOverlayFramePlan(inputs);

  EXPECT_TRUE(planInputs.wireframeLayoutReady);
  EXPECT_TRUE(planInputs.wireframePushConstantsReady);
  EXPECT_TRUE(planInputs.pipelines.bimFloorPlanDepth);
  EXPECT_TRUE(plan.floorPlan.active);
  EXPECT_EQ(plan.floorPlan.commands, &commands);
  EXPECT_EQ(inputs.pipelines.bimFloorPlanDepth, fakeHandle<VkPipeline>(0x4));
  EXPECT_EQ(inputs.wireframePushConstants, &pushConstants);
  EXPECT_EQ(inputs.debugOverlay, &debugOverlay);
}

TEST(BimLightingOverlayRecorderTests,
     FrameOverlayRejectsNullCommandBufferBeforeRecording) {
  const BimLightingOverlayFrameRecordInputs inputs{
      .wireframeLayout = fakeHandle<VkPipelineLayout>(0x1)};

  EXPECT_FALSE(recordBimLightingOverlayFrameCommands(VK_NULL_HANDLE, inputs));
}
