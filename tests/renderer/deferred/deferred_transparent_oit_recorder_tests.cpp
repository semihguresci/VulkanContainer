#include "Container/renderer/deferred/DeferredTransparentOitRecorder.h"

#include "Container/renderer/scene/DrawCommand.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::gpu::BindlessPushConstants;
using container::renderer::BimSurfaceDrawRouteKind;
using container::renderer::BimSurfacePassPlan;
using container::renderer::DebugOverlayRenderer;
using container::renderer::DeferredTransparentOitFrameResourceInputs;
using container::renderer::DeferredTransparentOitRecordInputs;
using container::renderer::DrawCommand;
using container::renderer::FrameResources;
using container::renderer::SceneTransparentDrawPipeline;
using container::renderer::SceneTransparentDrawPlan;
using container::renderer::recordDeferredTransparentOitClearCommands;
using container::renderer::recordDeferredTransparentOitCommands;
using container::renderer::recordDeferredTransparentOitResolvePreparationCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::vector<DrawCommand> oneDrawCommand() {
  return {DrawCommand{.objectIndex = 3u, .firstIndex = 0u, .indexCount = 3u}};
}

DeferredTransparentOitRecordInputs requiredInputs(
    const DebugOverlayRenderer &debugOverlay) {
  return {.descriptorSets = {fakeHandle<VkDescriptorSet>(0x1),
                             fakeHandle<VkDescriptorSet>(0x2),
                             fakeHandle<VkDescriptorSet>(0x3),
                             fakeHandle<VkDescriptorSet>(0x4)},
          .pipelineLayout = fakeHandle<VkPipelineLayout>(0x5),
          .pushConstants = BindlessPushConstants{},
          .debugOverlay = &debugOverlay};
}

void makeSceneGeometryReady(DeferredTransparentOitRecordInputs &inputs) {
  inputs.scene.descriptorSet = fakeHandle<VkDescriptorSet>(0x6);
  inputs.scene.vertexSlice.buffer = fakeHandle<VkBuffer>(0x7);
  inputs.scene.indexSlice.buffer = fakeHandle<VkBuffer>(0x8);
}

void makeBimGeometryReady(DeferredTransparentOitRecordInputs &inputs) {
  inputs.bim.descriptorSet = fakeHandle<VkDescriptorSet>(0x9);
  inputs.bim.vertexSlice.buffer = fakeHandle<VkBuffer>(0xa);
  inputs.bim.indexSlice.buffer = fakeHandle<VkBuffer>(0xb);
}

} // namespace

TEST(DeferredTransparentOitRecorderTests, EmptyInputsReturnFalse) {
  EXPECT_FALSE(recordDeferredTransparentOitCommands(VK_NULL_HANDLE, {}));
}

TEST(DeferredTransparentOitRecorderTests,
     MissingPipelineLayoutReturnsFalse) {
  DebugOverlayRenderer debugOverlay{};
  DeferredTransparentOitRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.pipelineLayout = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredTransparentOitCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredTransparentOitRecorderTests, MissingDebugOverlayReturnsFalse) {
  DebugOverlayRenderer debugOverlay{};
  DeferredTransparentOitRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.debugOverlay = nullptr;

  EXPECT_FALSE(recordDeferredTransparentOitCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredTransparentOitRecorderTests, EmptyPlansReturnFalse) {
  DebugOverlayRenderer debugOverlay{};

  EXPECT_FALSE(recordDeferredTransparentOitCommands(
      VK_NULL_HANDLE, requiredInputs(debugOverlay)));
}

TEST(DeferredTransparentOitRecorderTests,
     SceneRouteMissingGeometryReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneTransparentDrawPlan plan{};
  plan.routeCount = 1u;
  plan.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  DeferredTransparentOitRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.scenePlan = &plan;
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0xc);

  EXPECT_FALSE(recordDeferredTransparentOitCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredTransparentOitRecorderTests,
     SceneRouteMissingSelectedPipelineReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneTransparentDrawPlan plan{};
  plan.routeCount = 1u;
  plan.routes[0].pipeline = SceneTransparentDrawPipeline::FrontCull;
  plan.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  DeferredTransparentOitRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.scenePlan = &plan;
  makeSceneGeometryReady(inputs);

  EXPECT_FALSE(recordDeferredTransparentOitCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredTransparentOitRecorderTests,
     BimRouteMissingGeometryReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  BimSurfacePassPlan plan{};
  plan.active = true;
  plan.sourceCount = 1u;
  plan.sources[0].routeCount = 1u;
  plan.sources[0].routes[0].cpuCommands = &commands;

  DebugOverlayRenderer debugOverlay{};
  DeferredTransparentOitRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.bimPlan = &plan;
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0xc);

  EXPECT_FALSE(recordDeferredTransparentOitCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredTransparentOitRecorderTests,
     BimRouteMissingSelectedPipelineReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  BimSurfacePassPlan plan{};
  plan.active = true;
  plan.sourceCount = 1u;
  plan.sources[0].routeCount = 1u;
  plan.sources[0].routes[0].kind = BimSurfaceDrawRouteKind::WindingFlipped;
  plan.sources[0].routes[0].cpuCommands = &commands;

  DebugOverlayRenderer debugOverlay{};
  DeferredTransparentOitRecordInputs inputs = requiredInputs(debugOverlay);
  inputs.bimPlan = &plan;
  makeBimGeometryReady(inputs);

  EXPECT_FALSE(recordDeferredTransparentOitCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredTransparentOitRecorderTests,
     OitClearRejectsMissingCommandBufferManagerAndFrame) {
  const auto frame = reinterpret_cast<const FrameResources *>(0x1);

  EXPECT_FALSE(recordDeferredTransparentOitClearCommands(
      VK_NULL_HANDLE, DeferredTransparentOitFrameResourceInputs{.frame = frame}));
  EXPECT_FALSE(recordDeferredTransparentOitClearCommands(
      fakeHandle<VkCommandBuffer>(0x2), {}));
}

TEST(DeferredTransparentOitRecorderTests,
     OitResolvePreparationRejectsMissingCommandBufferManagerAndFrame) {
  const auto frame = reinterpret_cast<const FrameResources *>(0x1);

  EXPECT_FALSE(recordDeferredTransparentOitResolvePreparationCommands(
      VK_NULL_HANDLE, DeferredTransparentOitFrameResourceInputs{.frame = frame}));
  EXPECT_FALSE(recordDeferredTransparentOitResolvePreparationCommands(
      fakeHandle<VkCommandBuffer>(0x3), {}));
}
