#include "Container/renderer/scene/SceneOpaqueDrawRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::gpu::BindlessPushConstants;
using container::renderer::DebugOverlayRenderer;
using container::renderer::DrawCommand;
using container::renderer::SceneOpaqueDrawPipeline;
using container::renderer::SceneOpaqueDrawPlan;
using container::renderer::SceneOpaqueDrawRecordInputs;
using container::renderer::recordSceneOpaqueDrawCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::vector<DrawCommand> oneDrawCommand() {
  return {DrawCommand{.objectIndex = 3u, .firstIndex = 0u, .indexCount = 3u}};
}

SceneOpaqueDrawRecordInputs requiredInputs(
    const SceneOpaqueDrawPlan &plan,
    const DebugOverlayRenderer &debugOverlay) {
  return {.plan = &plan,
          .pipelineLayout = fakeHandle<VkPipelineLayout>(0x1),
          .pushConstants = BindlessPushConstants{},
          .debugOverlay = &debugOverlay};
}

} // namespace

TEST(SceneOpaqueDrawRecorderTests, NullPlanReturnsFalse) {
  EXPECT_FALSE(recordSceneOpaqueDrawCommands(VK_NULL_HANDLE, {}));
}

TEST(SceneOpaqueDrawRecorderTests, EmptyPlanReturnsFalse) {
  SceneOpaqueDrawPlan plan{};
  DebugOverlayRenderer debugOverlay{};

  EXPECT_FALSE(recordSceneOpaqueDrawCommands(
      VK_NULL_HANDLE, requiredInputs(plan, debugOverlay)));
}

TEST(SceneOpaqueDrawRecorderTests, MissingPipelineLayoutReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneOpaqueDrawPlan plan{};
  plan.cpuRouteCount = 1u;
  plan.cpuRoutes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  SceneOpaqueDrawRecordInputs inputs = requiredInputs(plan, debugOverlay);
  inputs.pipelineLayout = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSceneOpaqueDrawCommands(VK_NULL_HANDLE, inputs));
}

TEST(SceneOpaqueDrawRecorderTests, MissingDebugOverlayReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneOpaqueDrawPlan plan{};
  plan.cpuRouteCount = 1u;
  plan.cpuRoutes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  SceneOpaqueDrawRecordInputs inputs = requiredInputs(plan, debugOverlay);
  inputs.debugOverlay = nullptr;

  EXPECT_FALSE(recordSceneOpaqueDrawCommands(VK_NULL_HANDLE, inputs));
}

TEST(SceneOpaqueDrawRecorderTests, MissingGeometryReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneOpaqueDrawPlan plan{};
  plan.cpuRouteCount = 1u;
  plan.cpuRoutes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  SceneOpaqueDrawRecordInputs inputs = requiredInputs(plan, debugOverlay);
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0x2);

  EXPECT_FALSE(recordSceneOpaqueDrawCommands(VK_NULL_HANDLE, inputs));
}

TEST(SceneOpaqueDrawRecorderTests,
     MissingSelectedPipelineSkipsRouteAndReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneOpaqueDrawPlan plan{};
  plan.cpuRouteCount = 1u;
  plan.cpuRoutes[0].pipeline = SceneOpaqueDrawPipeline::FrontCull;
  plan.cpuRoutes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  const SceneOpaqueDrawRecordInputs inputs =
      requiredInputs(plan, debugOverlay);

  EXPECT_FALSE(recordSceneOpaqueDrawCommands(VK_NULL_HANDLE, inputs));
}

TEST(SceneOpaqueDrawRecorderTests,
     EmptyCommandRouteSkipsAndReturnsFalse) {
  const std::vector<DrawCommand> commands{};
  SceneOpaqueDrawPlan plan{};
  plan.cpuRouteCount = 1u;
  plan.cpuRoutes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  SceneOpaqueDrawRecordInputs inputs = requiredInputs(plan, debugOverlay);
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0x2);

  EXPECT_FALSE(recordSceneOpaqueDrawCommands(VK_NULL_HANDLE, inputs));
}

TEST(SceneOpaqueDrawRecorderTests,
     GpuIndirectRouteRequiresCullManagerBeforeRecording) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneOpaqueDrawPlan plan{};
  plan.useGpuIndirectSingleSided = true;
  plan.gpuIndirectRoute.commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  SceneOpaqueDrawRecordInputs inputs = requiredInputs(plan, debugOverlay);
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0x2);
  inputs.geometry.descriptorSet = fakeHandle<VkDescriptorSet>(0x3);
  inputs.geometry.vertexSlice.buffer = fakeHandle<VkBuffer>(0x4);
  inputs.geometry.indexSlice.buffer = fakeHandle<VkBuffer>(0x5);

  EXPECT_FALSE(recordSceneOpaqueDrawCommands(VK_NULL_HANDLE, inputs));
}
