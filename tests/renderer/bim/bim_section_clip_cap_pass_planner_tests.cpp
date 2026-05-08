#include "Container/renderer/bim/BimSectionClipCapPassPlanner.h"
#include "Container/renderer/bim/BimSectionClipCapPassRecorder.h"
#include "Container/renderer/scene/DrawCommand.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::renderer::BimSectionClipCapFramePassRecordInputs;
using container::renderer::BimSectionClipCapPassGeometryBinding;
using container::renderer::BimSectionClipCapPassInputs;
using container::renderer::BimSectionClipCapPassPipeline;
using container::renderer::buildBimSectionClipCapFramePassPlanInputs;
using container::renderer::buildBimSectionClipCapPassPlan;
using container::renderer::DrawCommand;
using container::renderer::hasBimSectionClipCapFramePassGeometry;
using container::renderer::rasterBimSectionClipCapLineWidth;
using container::renderer::recordBimSectionClipCapFramePassCommands;
using container::renderer::sanitizeBimSectionClipCapLineWidth;
using container::renderer::WireframePushConstants;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t firstIndex) {
  return {DrawCommand{.objectIndex = firstIndex,
                      .firstIndex = firstIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

[[nodiscard]] BimSectionClipCapPassInputs readyInputs() {
  return {.enabled = true,
          .fillEnabled = true,
          .hatchEnabled = true,
          .geometryReady = true,
          .wireframeLayoutReady = true,
          .wireframePushConstantsReady = true,
          .wideLinesSupported = true,
          .fillPipelineReady = true,
          .hatchPipelineReady = true};
}

TEST(BimSectionClipCapPassPlannerTests,
     FillAndHatchRoutesPreserveOrderAndStyle) {
  const auto fill = drawCommands(1u);
  const auto hatch = drawCommands(2u);
  auto inputs = readyInputs();
  inputs.fillColor = {0.1f, 0.2f, 0.3f, 0.4f};
  inputs.hatchColor = {0.5f, 0.6f, 0.7f, 0.8f};
  inputs.hatchLineWidth = 2.5f;
  inputs.fillDrawCommands = &fill;
  inputs.hatchDrawCommands = &hatch;

  const auto plan = buildBimSectionClipCapPassPlan(inputs);

  ASSERT_TRUE(plan.active);
  ASSERT_EQ(plan.routeCount, 2u);
  EXPECT_EQ(plan.routes[0].pipeline, BimSectionClipCapPassPipeline::Fill);
  EXPECT_EQ(plan.routes[0].commands, &fill);
  EXPECT_FLOAT_EQ(plan.routes[0].color.x, 0.1f);
  EXPECT_FLOAT_EQ(plan.routes[0].color.y, 0.2f);
  EXPECT_FLOAT_EQ(plan.routes[0].color.z, 0.3f);
  EXPECT_FLOAT_EQ(plan.routes[0].opacity, 0.4f);
  EXPECT_FALSE(plan.routes[0].rasterLineWidthApplies);
  EXPECT_FALSE(plan.routes[0].resetRasterLineWidth);

  EXPECT_EQ(plan.routes[1].pipeline, BimSectionClipCapPassPipeline::Hatch);
  EXPECT_EQ(plan.routes[1].commands, &hatch);
  EXPECT_FLOAT_EQ(plan.routes[1].color.x, 0.5f);
  EXPECT_FLOAT_EQ(plan.routes[1].color.y, 0.6f);
  EXPECT_FLOAT_EQ(plan.routes[1].color.z, 0.7f);
  EXPECT_FLOAT_EQ(plan.routes[1].opacity, 0.8f);
  EXPECT_FLOAT_EQ(plan.routes[1].drawLineWidth, 2.5f);
  EXPECT_FLOAT_EQ(plan.routes[1].rasterLineWidth, 2.5f);
  EXPECT_TRUE(plan.routes[1].rasterLineWidthApplies);
  EXPECT_TRUE(plan.routes[1].resetRasterLineWidth);
}

TEST(BimSectionClipCapPassPlannerTests, CommonPrerequisitesSuppressAllRoutes) {
  const auto fill = drawCommands(3u);
  auto inputs = readyInputs();
  inputs.fillDrawCommands = &fill;

  inputs.enabled = false;
  EXPECT_FALSE(buildBimSectionClipCapPassPlan(inputs).active);

  inputs = readyInputs();
  inputs.fillDrawCommands = &fill;
  inputs.geometryReady = false;
  EXPECT_FALSE(buildBimSectionClipCapPassPlan(inputs).active);

  inputs = readyInputs();
  inputs.fillDrawCommands = &fill;
  inputs.wireframeLayoutReady = false;
  EXPECT_FALSE(buildBimSectionClipCapPassPlan(inputs).active);

  inputs = readyInputs();
  inputs.fillDrawCommands = &fill;
  inputs.wireframePushConstantsReady = false;
  EXPECT_FALSE(buildBimSectionClipCapPassPlan(inputs).active);
}

TEST(BimSectionClipCapPassPlannerTests,
     FillAndHatchReadinessGateRoutesIndependently) {
  const auto fill = drawCommands(4u);
  const auto hatch = drawCommands(5u);
  auto inputs = readyInputs();
  inputs.fillPipelineReady = false;
  inputs.fillDrawCommands = &fill;
  inputs.hatchDrawCommands = &hatch;

  const auto hatchOnly = buildBimSectionClipCapPassPlan(inputs);

  ASSERT_TRUE(hatchOnly.active);
  ASSERT_EQ(hatchOnly.routeCount, 1u);
  EXPECT_EQ(hatchOnly.routes[0].pipeline, BimSectionClipCapPassPipeline::Hatch);

  inputs = readyInputs();
  inputs.hatchEnabled = false;
  inputs.fillDrawCommands = &fill;
  inputs.hatchDrawCommands = &hatch;

  const auto fillOnly = buildBimSectionClipCapPassPlan(inputs);

  ASSERT_TRUE(fillOnly.active);
  ASSERT_EQ(fillOnly.routeCount, 1u);
  EXPECT_EQ(fillOnly.routes[0].pipeline, BimSectionClipCapPassPipeline::Fill);
}

TEST(BimSectionClipCapPassPlannerTests, EmptyCommandsDoNotProduceRoutes) {
  const auto empty = std::vector<DrawCommand>{};
  auto inputs = readyInputs();
  inputs.fillDrawCommands = &empty;
  inputs.hatchDrawCommands = &empty;

  const auto plan = buildBimSectionClipCapPassPlan(inputs);

  EXPECT_FALSE(plan.active);
  EXPECT_EQ(plan.routeCount, 0u);
}

TEST(BimSectionClipCapPassPlannerTests,
     HatchLineWidthSanitizesAndFallsBackWithoutWideLines) {
  EXPECT_FLOAT_EQ(sanitizeBimSectionClipCapLineWidth(0.25f), 1.0f);
  EXPECT_FLOAT_EQ(sanitizeBimSectionClipCapLineWidth(3.0f), 3.0f);
  EXPECT_FLOAT_EQ(rasterBimSectionClipCapLineWidth(3.0f, false), 1.0f);
  EXPECT_FLOAT_EQ(rasterBimSectionClipCapLineWidth(3.0f, true), 3.0f);

  const auto hatch = drawCommands(6u);
  auto inputs = readyInputs();
  inputs.wideLinesSupported = false;
  inputs.fillEnabled = false;
  inputs.hatchLineWidth = 3.0f;
  inputs.hatchDrawCommands = &hatch;

  const auto plan = buildBimSectionClipCapPassPlan(inputs);

  ASSERT_TRUE(plan.active);
  ASSERT_EQ(plan.routeCount, 1u);
  EXPECT_FLOAT_EQ(plan.routes[0].drawLineWidth, 1.0f);
  EXPECT_FLOAT_EQ(plan.routes[0].rasterLineWidth, 1.0f);
}

TEST(BimSectionClipCapPassPlannerTests,
     FramePassInputsDeriveReadinessFromRecordingHandles) {
  const auto fill = drawCommands(7u);
  const BimSectionClipCapPassGeometryBinding geometry{
      .sceneDescriptorSet = fakeHandle<VkDescriptorSet>(0x1),
      .vertexSlice = {.buffer = fakeHandle<VkBuffer>(0x2)},
      .indexSlice = {.buffer = fakeHandle<VkBuffer>(0x3)}};
  const WireframePushConstants pushConstants{};
  const BimSectionClipCapFramePassRecordInputs inputs{
      .style = {.enabled = true,
                .fillEnabled = true,
                .wideLinesSupported = true,
                .fillDrawCommands = &fill},
      .geometry = geometry,
      .fillPipeline = fakeHandle<VkPipeline>(0x4),
      .wireframeLayout = fakeHandle<VkPipelineLayout>(0x5),
      .pushConstants = &pushConstants};

  const BimSectionClipCapPassInputs planInputs =
      buildBimSectionClipCapFramePassPlanInputs(inputs);

  EXPECT_TRUE(hasBimSectionClipCapFramePassGeometry(geometry));
  EXPECT_TRUE(planInputs.enabled);
  EXPECT_TRUE(planInputs.fillEnabled);
  EXPECT_TRUE(planInputs.geometryReady);
  EXPECT_TRUE(planInputs.wireframeLayoutReady);
  EXPECT_TRUE(planInputs.wireframePushConstantsReady);
  EXPECT_TRUE(planInputs.fillPipelineReady);
  EXPECT_FALSE(planInputs.hatchPipelineReady);
  EXPECT_EQ(planInputs.fillDrawCommands, &fill);
}

TEST(BimSectionClipCapPassPlannerTests,
     FramePassRejectsNullCommandBufferBeforeRecording) {
  const auto fill = drawCommands(8u);
  const BimSectionClipCapFramePassRecordInputs inputs{
      .style = {
          .enabled = true, .fillEnabled = true, .fillDrawCommands = &fill}};

  EXPECT_FALSE(
      recordBimSectionClipCapFramePassCommands(VK_NULL_HANDLE, inputs));
}

} // namespace
