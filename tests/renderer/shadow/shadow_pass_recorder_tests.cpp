#include "Container/renderer/shadow/ShadowPassRecorder.h"

#include "Container/renderer/scene/DrawCommand.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::gpu::ShadowPushConstants;
using container::renderer::DrawCommand;
using container::renderer::ShadowCascadePassRecordInputs;
using container::renderer::ShadowCascadeSecondaryPassRecordInputs;
using container::renderer::ShadowPassDrawPlan;
using container::renderer::ShadowPassPipeline;
using container::renderer::ShadowPassRecordInputs;
using container::renderer::recordShadowCascadePassCommands;
using container::renderer::recordShadowCascadeSecondaryPassCommands;
using container::renderer::recordShadowPassCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::vector<DrawCommand> oneDrawCommand() {
  return {DrawCommand{.objectIndex = 3u, .firstIndex = 0u, .indexCount = 3u}};
}

ShadowPassRecordInputs requiredInputs(const ShadowPassDrawPlan &plan) {
  return {.plan = &plan,
          .shadowDescriptorSet = fakeHandle<VkDescriptorSet>(0x1),
          .pipelineLayout = fakeHandle<VkPipelineLayout>(0x2),
          .pushConstants = ShadowPushConstants{}};
}

} // namespace

TEST(ShadowPassRecorderTests, NullPlanReturnsFalse) {
  EXPECT_FALSE(recordShadowPassCommands(VK_NULL_HANDLE, {}));
}

TEST(ShadowPassRecorderTests, MissingShadowDescriptorSetReturnsFalse) {
  ShadowPassDrawPlan plan{};
  ShadowPassRecordInputs inputs = requiredInputs(plan);
  inputs.shadowDescriptorSet = VK_NULL_HANDLE;

  EXPECT_FALSE(recordShadowPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(ShadowPassRecorderTests, MissingPipelineLayoutReturnsFalse) {
  ShadowPassDrawPlan plan{};
  ShadowPassRecordInputs inputs = requiredInputs(plan);
  inputs.pipelineLayout = VK_NULL_HANDLE;

  EXPECT_FALSE(recordShadowPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(ShadowPassRecorderTests, EmptyPlanWithoutGeometryReturnsFalse) {
  ShadowPassDrawPlan plan{};

  EXPECT_FALSE(
      recordShadowPassCommands(VK_NULL_HANDLE, requiredInputs(plan)));
}

TEST(ShadowPassRecorderTests, MissingSceneGeometryReturnsFalseBeforeRecording) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  ShadowPassDrawPlan plan{};
  plan.sceneCpuRouteCount = 1u;
  plan.sceneCpuRoutes[0].commands = &commands;

  ShadowPassRecordInputs inputs = requiredInputs(plan);
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0x3);

  EXPECT_FALSE(recordShadowPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(ShadowPassRecorderTests,
     MissingSelectedScenePipelineSkipsRouteAndReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  ShadowPassDrawPlan plan{};
  plan.sceneCpuRouteCount = 1u;
  plan.sceneCpuRoutes[0].pipeline = ShadowPassPipeline::FrontCull;
  plan.sceneCpuRoutes[0].commands = &commands;

  ShadowPassRecordInputs inputs = requiredInputs(plan);
  inputs.scene.sceneDescriptorSet = fakeHandle<VkDescriptorSet>(0x3);
  inputs.scene.vertexSlice.buffer = fakeHandle<VkBuffer>(0x4);
  inputs.scene.indexSlice.buffer = fakeHandle<VkBuffer>(0x5);

  EXPECT_FALSE(recordShadowPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(ShadowPassRecorderTests,
     EmptySceneCommandRouteSkipsAndReturnsFalse) {
  const std::vector<DrawCommand> commands{};
  ShadowPassDrawPlan plan{};
  plan.sceneCpuRouteCount = 1u;
  plan.sceneCpuRoutes[0].commands = &commands;

  ShadowPassRecordInputs inputs = requiredInputs(plan);
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0x3);
  inputs.scene.sceneDescriptorSet = fakeHandle<VkDescriptorSet>(0x4);
  inputs.scene.vertexSlice.buffer = fakeHandle<VkBuffer>(0x5);
  inputs.scene.indexSlice.buffer = fakeHandle<VkBuffer>(0x6);

  EXPECT_FALSE(recordShadowPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(ShadowPassRecorderTests,
     GpuSceneRouteRequiresIndirectBuffersBeforeRecording) {
  ShadowPassDrawPlan plan{};
  plan.sceneGpuRoute.active = true;

  ShadowPassRecordInputs inputs = requiredInputs(plan);
  inputs.pipelines.primary = fakeHandle<VkPipeline>(0x3);
  inputs.scene.sceneDescriptorSet = fakeHandle<VkDescriptorSet>(0x4);
  inputs.scene.vertexSlice.buffer = fakeHandle<VkBuffer>(0x5);
  inputs.scene.indexSlice.buffer = fakeHandle<VkBuffer>(0x6);

  EXPECT_FALSE(recordShadowPassCommands(VK_NULL_HANDLE, inputs));
}

TEST(ShadowPassRecorderTests, InactiveCascadePassReturnsFalse) {
  ShadowCascadePassRecordInputs inputs{};

  EXPECT_FALSE(recordShadowCascadePassCommands(VK_NULL_HANDLE, inputs));
}

TEST(ShadowPassRecorderTests, SecondaryCascadeRecordingIgnoresEmptyPlan) {
  ShadowCascadeSecondaryPassRecordInputs inputs{};
  inputs.secondaryCommandBuffersEnabled = true;

  EXPECT_NO_THROW(recordShadowCascadeSecondaryPassCommands(inputs));
}
