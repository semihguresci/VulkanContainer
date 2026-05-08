#include "Container/renderer/deferred/DeferredRasterDebugOverlayRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::gpu::BindlessPushConstants;
using container::renderer::DebugOverlayRenderer;
using container::renderer::DeferredDebugOverlayPlan;
using container::renderer::DeferredDebugOverlayRecordInputs;
using container::renderer::DeferredDebugOverlaySource;
using container::renderer::DeferredDebugOverlaySourcePlan;
using container::renderer::DrawCommand;
using container::renderer::WireframePushConstants;
using container::renderer::recordDeferredDebugOverlayGeometryCommands;
using container::renderer::recordDeferredDebugOverlayNormalValidationCommands;
using container::renderer::recordDeferredDebugOverlayObjectNormalCommands;
using container::renderer::recordDeferredDebugOverlaySurfaceNormalCommands;
using container::renderer::recordDeferredDebugOverlayWireframeFullCommands;
using container::renderer::recordDeferredDebugOverlayWireframeOverlayCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::vector<DrawCommand> oneDrawCommand() {
  return {DrawCommand{.objectIndex = 1u, .firstIndex = 0u, .indexCount = 3u}};
}

DeferredDebugOverlayRecordInputs requiredInputs(
    const DeferredDebugOverlayPlan &plan,
    const DebugOverlayRenderer &debugOverlay) {
  return {.plan = &plan,
          .sceneLayout = fakeHandle<VkPipelineLayout>(0x1),
          .wireframeLayout = fakeHandle<VkPipelineLayout>(0x2),
          .normalValidationLayout = fakeHandle<VkPipelineLayout>(0x3),
          .surfaceNormalLayout = fakeHandle<VkPipelineLayout>(0x4),
          .debugOverlay = &debugOverlay};
}

} // namespace

TEST(DeferredRasterDebugOverlayRecorderTests, NullPlanReturnsFalse) {
  EXPECT_FALSE(
      recordDeferredDebugOverlayWireframeFullCommands(VK_NULL_HANDLE, {}));
  EXPECT_FALSE(
      recordDeferredDebugOverlayObjectNormalCommands(VK_NULL_HANDLE, {}));
  EXPECT_FALSE(recordDeferredDebugOverlayGeometryCommands(VK_NULL_HANDLE, {}));
  EXPECT_FALSE(
      recordDeferredDebugOverlayNormalValidationCommands(VK_NULL_HANDLE, {}));
  EXPECT_FALSE(
      recordDeferredDebugOverlaySurfaceNormalCommands(VK_NULL_HANDLE, {}));
  EXPECT_FALSE(
      recordDeferredDebugOverlayWireframeOverlayCommands(VK_NULL_HANDLE, {}));
}

TEST(DeferredRasterDebugOverlayRecorderTests, EmptyPlanReturnsFalse) {
  DeferredDebugOverlayPlan plan{};
  DebugOverlayRenderer debugOverlay{};
  const DeferredDebugOverlayRecordInputs inputs =
      requiredInputs(plan, debugOverlay);

  EXPECT_FALSE(
      recordDeferredDebugOverlayWireframeFullCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredRasterDebugOverlayRecorderTests,
     MissingDebugOverlayReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  DeferredDebugOverlayPlan plan{};
  plan.wireframeFullSourceCount = 1u;
  DeferredDebugOverlaySourcePlan &source = plan.wireframeFullSources[0];
  source.source = DeferredDebugOverlaySource::Scene;
  source.routeCount = 1u;
  source.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  DeferredDebugOverlayRecordInputs inputs =
      requiredInputs(plan, debugOverlay);
  inputs.debugOverlay = nullptr;

  EXPECT_FALSE(
      recordDeferredDebugOverlayWireframeFullCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredRasterDebugOverlayRecorderTests,
     MissingWireframePushConstantsReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  DeferredDebugOverlayPlan plan{};
  plan.wireframeFullSourceCount = 1u;
  DeferredDebugOverlaySourcePlan &source = plan.wireframeFullSources[0];
  source.source = DeferredDebugOverlaySource::Scene;
  source.routeCount = 1u;
  source.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  DeferredDebugOverlayRecordInputs inputs =
      requiredInputs(plan, debugOverlay);
  inputs.pipelines.wireframeDepth = fakeHandle<VkPipeline>(0x5);

  EXPECT_FALSE(
      recordDeferredDebugOverlayWireframeFullCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredRasterDebugOverlayRecorderTests,
     MissingGeometryReturnsFalseBeforeRecording) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  DeferredDebugOverlayPlan plan{};
  plan.wireframeFullSourceCount = 1u;
  DeferredDebugOverlaySourcePlan &source = plan.wireframeFullSources[0];
  source.source = DeferredDebugOverlaySource::Scene;
  source.routeCount = 1u;
  source.routes[0].commands = &commands;

  WireframePushConstants wireframePushConstants{};
  DebugOverlayRenderer debugOverlay{};
  DeferredDebugOverlayRecordInputs inputs =
      requiredInputs(plan, debugOverlay);
  inputs.pipelines.wireframeDepth = fakeHandle<VkPipeline>(0x5);
  inputs.wireframePushConstants = &wireframePushConstants;

  EXPECT_FALSE(
      recordDeferredDebugOverlayWireframeFullCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredRasterDebugOverlayRecorderTests,
     MissingBindlessConstantsStopsSceneDebugRecording) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  DeferredDebugOverlayPlan plan{};
  plan.geometryOverlaySourceCount = 1u;
  DeferredDebugOverlaySourcePlan &source = plan.geometryOverlaySources[0];
  source.source = DeferredDebugOverlaySource::Scene;
  source.routeCount = 1u;
  source.routes[0].commands = &commands;

  DebugOverlayRenderer debugOverlay{};
  DeferredDebugOverlayRecordInputs inputs =
      requiredInputs(plan, debugOverlay);
  inputs.pipelines.geometryDebug = fakeHandle<VkPipeline>(0x6);

  EXPECT_FALSE(recordDeferredDebugOverlayGeometryCommands(VK_NULL_HANDLE,
                                                         inputs));
}
