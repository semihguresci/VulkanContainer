#include "Container/renderer/bim/BimSectionClipCapPassPlanner.h"
#include "Container/renderer/bim/BimSectionClipCapPassRecorder.h"
#include "Container/renderer/bim/BimSectionCapBuilder.h"
#include "Container/renderer/scene/DrawCommand.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::renderer::BimSectionClipCapFramePassRecordInputs;
using container::renderer::BimSectionClipCapPassGeometryBinding;
using container::renderer::BimSectionClipCapPassInputs;
using container::renderer::BimSectionClipCapPassPlan;
using container::renderer::BimSectionClipCapPassPipeline;
using container::renderer::BimSectionClipCapPassRecordInputs;
using container::renderer::BimSectionCapDrawStyle;
using container::renderer::buildBimSectionClipCapFramePassPlanInputs;
using container::renderer::buildBimSectionClipCapPassPlan;
using container::renderer::DebugOverlayRenderer;
using container::renderer::DrawCommand;
using container::renderer::hasBimSectionClipCapFramePassGeometry;
using container::renderer::rasterBimSectionClipCapLineWidth;
using container::renderer::recordBimSectionClipCapPassCommands;
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

[[nodiscard]] std::vector<DrawCommand> twoDrawCommands() {
  return {DrawCommand{.objectIndex = 20u,
                      .firstIndex = 4u,
                      .indexCount = 6u,
                      .instanceCount = 1u},
          DrawCommand{.objectIndex = 21u,
                      .firstIndex = 10u,
                      .indexCount = 2u,
                      .instanceCount = 1u}};
}

struct RecordedWireframePush {
  glm::vec4 colorIntensity{0.0f};
  float lineWidth{1.0f};
  uint32_t sectionPlaneEnabled{0u};
};

struct RecordedIndexedDraw {
  uint32_t indexCount{0u};
  uint32_t instanceCount{0u};
  uint32_t firstIndex{0u};
  uint32_t firstInstance{0u};
};

std::vector<RecordedWireframePush> g_wireframePushes;
std::vector<RecordedIndexedDraw> g_indexedDraws;
std::vector<float> g_rasterLineWidths;

void resetRecordedVkCommands() {
  g_wireframePushes.clear();
  g_indexedDraws.clear();
  g_rasterLineWidths.clear();
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

[[nodiscard]] BimSectionClipCapPassRecordInputs recordInputs(
    const BimSectionClipCapPassPlan &plan,
    const WireframePushConstants &pushConstants,
    const DebugOverlayRenderer &debugOverlay) {
  return {.plan = &plan,
          .fillPipeline = fakeHandle<VkPipeline>(0x10),
          .hatchPipeline = fakeHandle<VkPipeline>(0x11),
          .wireframeLayout = fakeHandle<VkPipelineLayout>(0x12),
          .sceneDescriptorSet = fakeHandle<VkDescriptorSet>(0x13),
          .vertexSlice = {.buffer = fakeHandle<VkBuffer>(0x14)},
          .indexSlice = {.buffer = fakeHandle<VkBuffer>(0x15)},
          .indexType = VK_INDEX_TYPE_UINT32,
          .pushConstants = &pushConstants,
          .debugOverlay = &debugOverlay};
}

} // namespace

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(
    VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
    const VkDescriptorSet *, uint32_t, const uint32_t *) {}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(
    VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *,
    const VkDeviceSize *) {}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(
    VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType) {}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer,
                                                        float lineWidth) {
  g_rasterLineWidths.push_back(lineWidth);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(
    VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t,
    uint32_t size, const void *values) {
  if (size != sizeof(WireframePushConstants) || values == nullptr) {
    return;
  }
  const auto *pc = static_cast<const WireframePushConstants *>(values);
  g_wireframePushes.push_back(RecordedWireframePush{
      .colorIntensity = pc->colorIntensity,
      .lineWidth = pc->lineWidth,
      .sectionPlaneEnabled = pc->sectionPlaneEnabled,
  });
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(
    VkCommandBuffer, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t, uint32_t firstInstance) {
  g_indexedDraws.push_back(RecordedIndexedDraw{.indexCount = indexCount,
                                               .instanceCount = instanceCount,
                                               .firstIndex = firstIndex,
                                               .firstInstance = firstInstance});
}

namespace {

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
     RoutesExposePerMaterialStylesAndSectionMarkers) {
  const auto fill = drawCommands(9u);
  const auto hatch = drawCommands(10u);
  const std::vector<container::renderer::BimSectionCapDrawStyle> fillStyles{
      {.objectIndex = 9u,
       .materialIndex = 3u,
       .fillColor = {0.1f, 0.2f, 0.3f},
       .fillOpacity = 0.4f,
       .hatchSpacing = 0.5f,
       .hatchAngleRadians = 0.0f,
       .hatchColor = {0.5f, 0.6f, 0.7f}}};
  const std::vector<container::renderer::BimSectionCapDrawStyle> hatchStyles{
      {.objectIndex = 10u,
       .materialIndex = 4u,
       .fillColor = {0.2f, 0.3f, 0.4f},
       .fillOpacity = 0.5f,
       .hatchSpacing = 0.25f,
       .hatchAngleRadians = 0.2f,
       .hatchColor = {0.6f, 0.7f, 0.8f},
       .lineWidth = 3.0f}};
  const std::vector<container::renderer::BimSectionMarkerLine> markers{
      {.a = {-2.0f, 0.0f, 0.0f},
       .b = {2.0f, 0.0f, 0.0f},
       .color = {1.0f, 0.7f, 0.1f},
       .lineWidth = 2.0f,
       .startArrow = true,
       .endArrow = true,
       .sectionPlaneIndex = 0u}};

  auto inputs = readyInputs();
  inputs.fillDrawCommands = &fill;
  inputs.hatchDrawCommands = &hatch;
  inputs.fillDrawStyles = &fillStyles;
  inputs.hatchDrawStyles = &hatchStyles;
  inputs.sectionMarkerLines = &markers;

  const auto plan = buildBimSectionClipCapPassPlan(inputs);

  ASSERT_TRUE(plan.active);
  ASSERT_EQ(plan.routeCount, 2u);
  EXPECT_EQ(plan.routes[0].drawStyles, &fillStyles);
  EXPECT_EQ(plan.routes[1].drawStyles, &hatchStyles);
  EXPECT_EQ(plan.sectionMarkerLines, &markers);
}

TEST(BimSectionClipCapPassPlannerTests,
     MarkerOnlyRouteDrawsOnlyStyledMarkerCommands) {
  const auto hatch = twoDrawCommands();
  const std::vector<container::renderer::BimSectionCapDrawStyle> hatchStyles{
      {.objectIndex = 20u,
       .materialIndex = 4u,
       .fillColor = {0.2f, 0.3f, 0.4f},
       .fillOpacity = 0.5f,
       .hatchSpacing = 0.25f,
       .hatchAngleRadians = 0.2f,
       .hatchColor = {0.6f, 0.7f, 0.8f}},
      {.objectIndex = 21u,
       .materialIndex = container::renderer::kInvalidBimSectionCapMaterialIndex,
       .fillColor = {1.0f, 0.7f, 0.1f},
       .fillOpacity = 1.0f,
       .hatchSpacing = 0.0f,
       .hatchAngleRadians = 0.0f,
       .hatchColor = {1.0f, 0.7f, 0.1f},
       .lineWidth = 3.5f}};
  const std::vector<container::renderer::BimSectionMarkerLine> markers{
      {.a = {-2.0f, 0.0f, 0.0f},
       .b = {2.0f, 0.0f, 0.0f},
       .color = {1.0f, 0.7f, 0.1f},
       .lineWidth = 3.5f,
       .startArrow = true,
       .endArrow = true,
       .sectionPlaneIndex = 0u}};

  auto inputs = readyInputs();
  inputs.fillEnabled = false;
  inputs.hatchEnabled = false;
  inputs.hatchDrawCommands = &hatch;
  inputs.hatchDrawStyles = &hatchStyles;
  inputs.sectionMarkerLines = &markers;

  const auto plan = buildBimSectionClipCapPassPlan(inputs);

  ASSERT_TRUE(plan.active);
  ASSERT_EQ(plan.routeCount, 1u);
  EXPECT_EQ(plan.routes[0].pipeline, BimSectionClipCapPassPipeline::Hatch);
  EXPECT_TRUE(plan.routes[0].markerCommandsOnly);

  resetRecordedVkCommands();
  const WireframePushConstants pushConstants{};
  const DebugOverlayRenderer debugOverlay{};
  EXPECT_TRUE(recordBimSectionClipCapPassCommands(
      fakeHandle<VkCommandBuffer>(0x30),
      recordInputs(plan, pushConstants, debugOverlay)));

  ASSERT_EQ(g_indexedDraws.size(), 1u);
  EXPECT_EQ(g_indexedDraws.front().firstIndex, hatch.back().firstIndex);
  ASSERT_EQ(g_wireframePushes.size(), 1u);
  EXPECT_EQ(g_wireframePushes.front().colorIntensity,
            glm::vec4(1.0f, 0.7f, 0.1f, 0.95f));
  EXPECT_FLOAT_EQ(g_wireframePushes.front().lineWidth, 3.5f);
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
