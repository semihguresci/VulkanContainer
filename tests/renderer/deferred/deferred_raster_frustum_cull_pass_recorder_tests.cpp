#include "Container/renderer/deferred/DeferredRasterFrustumCullPassRecorder.h"

#include "Container/renderer/scene/DrawCommand.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using container::renderer::DeferredRasterFrustumCullPassPlan;
using container::renderer::DeferredRasterFrustumCullPassRecordInputs;
using container::renderer::DrawCommand;
using container::renderer::RenderPassReadiness;
using container::renderer::recordDeferredRasterFrustumCullPassCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

DeferredRasterFrustumCullPassPlan activePlan() {
  return {.active = true,
          .readiness = RenderPassReadiness{},
          .drawCount = 1u,
          .updateObjectDescriptor = true};
}

std::vector<DrawCommand> readyDraws() {
  return {{.objectIndex = 7u, .firstIndex = 3u, .indexCount = 9u}};
}

DeferredRasterFrustumCullPassRecordInputs readyInputs() {
  static std::vector<DrawCommand> draws = readyDraws();
  return {.gpuCullManager =
              reinterpret_cast<container::renderer::GpuCullManager *>(0x1),
          .plan = activePlan(),
          .drawCommands = &draws,
          .cameraBuffer = fakeHandle<VkBuffer>(0x2),
          .cameraBufferSize = 64u,
          .objectBuffer = fakeHandle<VkBuffer>(0x3),
          .objectBufferSize = 128u};
}

} // namespace

TEST(DeferredRasterFrustumCullPassRecorderTests,
     NullCommandBufferReturnsFalse) {
  EXPECT_FALSE(recordDeferredRasterFrustumCullPassCommands(VK_NULL_HANDLE,
                                                          readyInputs()));
}

TEST(DeferredRasterFrustumCullPassRecorderTests, NullManagerReturnsFalse) {
  auto inputs = readyInputs();
  inputs.gpuCullManager = nullptr;

  EXPECT_FALSE(recordDeferredRasterFrustumCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x4), inputs));
}

TEST(DeferredRasterFrustumCullPassRecorderTests,
     NullDrawCommandsReturnsFalse) {
  auto inputs = readyInputs();
  inputs.drawCommands = nullptr;

  EXPECT_FALSE(recordDeferredRasterFrustumCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x4), inputs));
}

TEST(DeferredRasterFrustumCullPassRecorderTests, InactivePlanReturnsFalse) {
  auto inputs = readyInputs();
  inputs.plan = {};

  EXPECT_FALSE(recordDeferredRasterFrustumCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x4), inputs));
}

TEST(DeferredRasterFrustumCullPassRecorderTests, NotReadyPlanReturnsFalse) {
  auto inputs = readyInputs();
  inputs.plan.readiness.ready = false;

  EXPECT_FALSE(recordDeferredRasterFrustumCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x4), inputs));
}

TEST(DeferredRasterFrustumCullPassRecorderTests,
     EmptyDrawCommandsReturnFalse) {
  const std::vector<DrawCommand> emptyDraws{};
  auto inputs = readyInputs();
  inputs.drawCommands = &emptyDraws;

  EXPECT_FALSE(recordDeferredRasterFrustumCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x4), inputs));
}

TEST(DeferredRasterFrustumCullPassRecorderTests, ZeroDrawCountReturnsFalse) {
  auto inputs = readyInputs();
  inputs.plan.drawCount = 0u;

  EXPECT_FALSE(recordDeferredRasterFrustumCullPassCommands(
      fakeHandle<VkCommandBuffer>(0x4), inputs));
}
