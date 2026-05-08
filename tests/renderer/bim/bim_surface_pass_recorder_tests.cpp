#include "Container/renderer/bim/BimSurfacePassRecorder.h"

#include "Container/renderer/scene/DrawCommand.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using container::gpu::BindlessPushConstants;
using container::renderer::BimSurfaceDrawRouteKind;
using container::renderer::BimSurfacePassPlan;
using container::renderer::BimSurfacePassRecordInputs;
using container::renderer::DebugOverlayRenderer;
using container::renderer::DrawCommand;
using container::renderer::recordBimSurfacePassCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

std::vector<DrawCommand> oneDrawCommand() {
  return {DrawCommand{.objectIndex = 3u, .firstIndex = 0u, .indexCount = 3u}};
}

BimSurfacePassPlan oneRoutePlan(const std::vector<DrawCommand> &commands) {
  BimSurfacePassPlan plan{};
  plan.active = true;
  plan.sourceCount = 1u;
  plan.sources[0].routeCount = 1u;
  plan.sources[0].routes[0].cpuCommands = &commands;
  return plan;
}

BimSurfacePassRecordInputs requiredInputs(
    const BimSurfacePassPlan &plan, const DebugOverlayRenderer &debugOverlay,
    std::array<VkDescriptorSet, 1> &descriptorSets) {
  descriptorSets = {fakeHandle<VkDescriptorSet>(0x1)};
  return {.plan = &plan,
          .geometry = {.descriptorSets = descriptorSets,
                       .vertexSlice = {.buffer = fakeHandle<VkBuffer>(0x2)},
                       .indexSlice = {.buffer = fakeHandle<VkBuffer>(0x3)},
                       .indexType = VK_INDEX_TYPE_UINT32},
          .singleSidedPipeline = fakeHandle<VkPipeline>(0x4),
          .pipelineLayout = fakeHandle<VkPipelineLayout>(0x5),
          .pushConstants = BindlessPushConstants{},
          .debugOverlay = &debugOverlay};
}

} // namespace

TEST(BimSurfacePassRecorderTests, EmptyInputsReturnFalse) {
  EXPECT_FALSE(recordBimSurfacePassCommands(VK_NULL_HANDLE, {}));
}

TEST(BimSurfacePassRecorderTests, InactivePlanReturnsFalse) {
  BimSurfacePassPlan plan{};
  DebugOverlayRenderer debugOverlay{};
  std::array<VkDescriptorSet, 1> descriptorSets{};
  BimSurfacePassRecordInputs inputs =
      requiredInputs(plan, debugOverlay, descriptorSets);

  EXPECT_FALSE(recordBimSurfacePassCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimSurfacePassRecorderTests, MissingPipelineLayoutReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  const BimSurfacePassPlan plan = oneRoutePlan(commands);
  DebugOverlayRenderer debugOverlay{};
  std::array<VkDescriptorSet, 1> descriptorSets{};
  BimSurfacePassRecordInputs inputs =
      requiredInputs(plan, debugOverlay, descriptorSets);
  inputs.pipelineLayout = VK_NULL_HANDLE;

  EXPECT_FALSE(recordBimSurfacePassCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimSurfacePassRecorderTests, MissingDebugOverlayReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  const BimSurfacePassPlan plan = oneRoutePlan(commands);
  DebugOverlayRenderer debugOverlay{};
  std::array<VkDescriptorSet, 1> descriptorSets{};
  BimSurfacePassRecordInputs inputs =
      requiredInputs(plan, debugOverlay, descriptorSets);
  inputs.debugOverlay = nullptr;

  EXPECT_FALSE(recordBimSurfacePassCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimSurfacePassRecorderTests, MissingDescriptorSetReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  const BimSurfacePassPlan plan = oneRoutePlan(commands);
  DebugOverlayRenderer debugOverlay{};
  std::array<VkDescriptorSet, 1> descriptorSets{};
  BimSurfacePassRecordInputs inputs =
      requiredInputs(plan, debugOverlay, descriptorSets);
  descriptorSets[0] = VK_NULL_HANDLE;

  EXPECT_FALSE(recordBimSurfacePassCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimSurfacePassRecorderTests, MissingVertexBufferReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  const BimSurfacePassPlan plan = oneRoutePlan(commands);
  DebugOverlayRenderer debugOverlay{};
  std::array<VkDescriptorSet, 1> descriptorSets{};
  BimSurfacePassRecordInputs inputs =
      requiredInputs(plan, debugOverlay, descriptorSets);
  inputs.geometry.vertexSlice.buffer = VK_NULL_HANDLE;

  EXPECT_FALSE(recordBimSurfacePassCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimSurfacePassRecorderTests, MissingIndexBufferReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  const BimSurfacePassPlan plan = oneRoutePlan(commands);
  DebugOverlayRenderer debugOverlay{};
  std::array<VkDescriptorSet, 1> descriptorSets{};
  BimSurfacePassRecordInputs inputs =
      requiredInputs(plan, debugOverlay, descriptorSets);
  inputs.geometry.indexSlice.buffer = VK_NULL_HANDLE;

  EXPECT_FALSE(recordBimSurfacePassCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimSurfacePassRecorderTests,
     MissingSelectedPipelineSkipsRouteAndReturnsFalse) {
  const std::vector<DrawCommand> commands = oneDrawCommand();
  BimSurfacePassPlan plan = oneRoutePlan(commands);
  plan.sources[0].routes[0].kind = BimSurfaceDrawRouteKind::WindingFlipped;
  DebugOverlayRenderer debugOverlay{};
  std::array<VkDescriptorSet, 1> descriptorSets{};
  BimSurfacePassRecordInputs inputs =
      requiredInputs(plan, debugOverlay, descriptorSets);

  EXPECT_FALSE(recordBimSurfacePassCommands(VK_NULL_HANDLE, inputs));
}

TEST(BimSurfacePassRecorderTests, EmptyCommandRouteReturnsFalse) {
  const std::vector<DrawCommand> commands{};
  const BimSurfacePassPlan plan = oneRoutePlan(commands);
  DebugOverlayRenderer debugOverlay{};
  std::array<VkDescriptorSet, 1> descriptorSets{};
  BimSurfacePassRecordInputs inputs =
      requiredInputs(plan, debugOverlay, descriptorSets);

  EXPECT_FALSE(recordBimSurfacePassCommands(VK_NULL_HANDLE, inputs));
}
