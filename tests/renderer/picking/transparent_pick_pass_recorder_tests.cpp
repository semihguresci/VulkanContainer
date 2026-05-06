#include "Container/renderer/picking/TransparentPickPassRecorder.h"

#include "Container/renderer/scene/DrawCommand.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::gpu::BindlessPushConstants;
using container::renderer::BimSurfaceDrawRouteKind;
using container::renderer::BimSurfacePassPlan;
using container::renderer::DebugOverlayRenderer;
using container::renderer::DrawCommand;
using container::renderer::SceneTransparentDrawPipeline;
using container::renderer::SceneTransparentDrawPlan;
using container::renderer::TransparentPickPassRecordInputs;
using container::renderer::recordTransparentPickPassCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::vector<DrawCommand> oneDrawCommand() {
  return {DrawCommand{.objectIndex = 3u, .firstIndex = 0u, .indexCount = 3u}};
}

TransparentPickPassRecordInputs requiredInputs(
    const DebugOverlayRenderer &debugOverlay) {
  return {.pipelineLayout = fakeHandle<VkPipelineLayout>(0x1),
          .pushConstants = BindlessPushConstants{},
          .debugOverlay = &debugOverlay};
}

void makeSceneGeometryReady(TransparentPickPassRecordInputs &inputs) {
  inputs.scene.descriptorSet = fakeHandle<VkDescriptorSet>(0x2);
  inputs.scene.vertexSlice.buffer = fakeHandle<VkBuffer>(0x3);
  inputs.scene.indexSlice.buffer = fakeHandle<VkBuffer>(0x4);
}

void makeBimGeometryReady(TransparentPickPassRecordInputs &inputs) {
  inputs.bim.descriptorSet = fakeHandle<VkDescriptorSet>(0x5);
  inputs.bim.vertexSlice.buffer = fakeHandle<VkBuffer>(0x6);
  inputs.bim.indexSlice.buffer = fakeHandle<VkBuffer>(0x7);
}

} // namespace

TEST(TransparentPickPassRecorderTests, EmptyInputsReturnFalse) {
  EXPECT_FALSE(recordTransparentPickPassCommands(VK_NULL_HANDLE, {}));
}

TEST(TransparentPickPassRecorderTests, MissingPipelineLayoutReturnsFalse) {
  DebugOverlayRenderer debugOverlay{};
  TransparentPickPassRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.pipelineLayout = VK_NULL_HANDLE;

  EXPECT_FALSE(recordTransparentPickPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(TransparentPickPassRecorderTests, MissingDebugOverlayReturnsFalse) {
  DebugOverlayRenderer debugOverlay{};
  TransparentPickPassRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.debugOverlay = nullptr;

  EXPECT_FALSE(recordTransparentPickPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(TransparentPickPassRecorderTests, EmptyPlansReturnFalse) {
  DebugOverlayRenderer debugOverlay{};

  EXPECT_FALSE(
      recordTransparentPickPassCommands(VK_NULL_HANDLE,
                                        requiredInputs(debugOverlay)));
}

TEST(TransparentPickPassRecorderTests,
     SceneRouteMissingGeometryReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneTransparentDrawPlan plan{};
  plan.routeCount = 1u;
  plan.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  TransparentPickPassRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.scenePlan = &plan;
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0x8);

  EXPECT_FALSE(recordTransparentPickPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(TransparentPickPassRecorderTests,
     SceneRouteMissingSelectedPipelineReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneTransparentDrawPlan plan{};
  plan.routeCount = 1u;
  plan.routes[0].pipeline = SceneTransparentDrawPipeline::FrontCull;
  plan.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  TransparentPickPassRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.scenePlan = &plan;
  makeSceneGeometryReady(inputs);

  EXPECT_FALSE(recordTransparentPickPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(TransparentPickPassRecorderTests,
     BimRouteMissingGeometryReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  BimSurfacePassPlan plan{};
  plan.active = true;
  plan.sourceCount = 1u;
  plan.sources[0].routeCount = 1u;
  plan.sources[0].routes[0].cpuCommands = &commands;

  DebugOverlayRenderer debugOverlay{};
  TransparentPickPassRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.bimPlan = &plan;
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0x8);

  EXPECT_FALSE(recordTransparentPickPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(TransparentPickPassRecorderTests,
     BimRouteMissingSelectedPipelineReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  BimSurfacePassPlan plan{};
  plan.active = true;
  plan.sourceCount = 1u;
  plan.sources[0].routeCount = 1u;
  plan.sources[0].routes[0].kind = BimSurfaceDrawRouteKind::WindingFlipped;
  plan.sources[0].routes[0].cpuCommands = &commands;

  DebugOverlayRenderer debugOverlay{};
  TransparentPickPassRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.bimPlan = &plan;
  makeBimGeometryReady(inputs);

  EXPECT_FALSE(recordTransparentPickPassCommands(VK_NULL_HANDLE, inputs));
}
