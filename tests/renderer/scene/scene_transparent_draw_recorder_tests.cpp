#include "Container/renderer/scene/SceneTransparentDrawRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::gpu::BindlessPushConstants;
using container::renderer::DebugOverlayRenderer;
using container::renderer::DrawCommand;
using container::renderer::SceneTransparentDrawPipeline;
using container::renderer::SceneTransparentDrawPlan;
using container::renderer::SceneTransparentDrawRecordInputs;
using container::renderer::recordSceneTransparentDrawCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::vector<DrawCommand> oneDrawCommand() {
  return {DrawCommand{.objectIndex = 3u, .firstIndex = 0u, .indexCount = 3u}};
}

SceneTransparentDrawRecordInputs requiredInputs(
    const SceneTransparentDrawPlan &plan,
    const DebugOverlayRenderer &debugOverlay) {
  return {.plan = &plan,
          .pipelineLayout = fakeHandle<VkPipelineLayout>(0x1),
          .pushConstants = BindlessPushConstants{},
          .debugOverlay = &debugOverlay};
}

} // namespace

TEST(SceneTransparentDrawRecorderTests, NullPlanReturnsFalse) {
  EXPECT_FALSE(recordSceneTransparentDrawCommands(VK_NULL_HANDLE, {}));
}

TEST(SceneTransparentDrawRecorderTests, EmptyPlanReturnsFalse) {
  SceneTransparentDrawPlan plan{};
  DebugOverlayRenderer debugOverlay{};

  EXPECT_FALSE(recordSceneTransparentDrawCommands(
      VK_NULL_HANDLE, requiredInputs(plan, debugOverlay)));
}

TEST(SceneTransparentDrawRecorderTests, MissingPipelineLayoutReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneTransparentDrawPlan plan{};
  plan.routeCount = 1u;
  plan.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  SceneTransparentDrawRecordInputs inputs =
      requiredInputs(plan, debugOverlay);
  inputs.pipelineLayout = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSceneTransparentDrawCommands(VK_NULL_HANDLE, inputs));
}

TEST(SceneTransparentDrawRecorderTests, MissingDebugOverlayReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneTransparentDrawPlan plan{};
  plan.routeCount = 1u;
  plan.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  SceneTransparentDrawRecordInputs inputs =
      requiredInputs(plan, debugOverlay);
  inputs.debugOverlay = nullptr;

  EXPECT_FALSE(recordSceneTransparentDrawCommands(VK_NULL_HANDLE, inputs));
}

TEST(SceneTransparentDrawRecorderTests, MissingGeometryReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneTransparentDrawPlan plan{};
  plan.routeCount = 1u;
  plan.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  SceneTransparentDrawRecordInputs inputs =
      requiredInputs(plan, debugOverlay);
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0x2);

  EXPECT_FALSE(recordSceneTransparentDrawCommands(VK_NULL_HANDLE, inputs));
}

TEST(SceneTransparentDrawRecorderTests,
     MissingSelectedPipelineSkipsRouteAndReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  SceneTransparentDrawPlan plan{};
  plan.routeCount = 1u;
  plan.routes[0].pipeline = SceneTransparentDrawPipeline::FrontCull;
  plan.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  const SceneTransparentDrawRecordInputs inputs =
      requiredInputs(plan, debugOverlay);

  EXPECT_FALSE(recordSceneTransparentDrawCommands(VK_NULL_HANDLE, inputs));
}

TEST(SceneTransparentDrawRecorderTests,
     EmptyCommandRouteSkipsAndReturnsFalse) {
  const std::vector<DrawCommand> commands{};
  SceneTransparentDrawPlan plan{};
  plan.routeCount = 1u;
  plan.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  SceneTransparentDrawRecordInputs inputs =
      requiredInputs(plan, debugOverlay);
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0x2);

  EXPECT_FALSE(recordSceneTransparentDrawCommands(VK_NULL_HANDLE, inputs));
}
