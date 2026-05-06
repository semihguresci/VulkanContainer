#include "Container/renderer/deferred/DeferredRasterTileCullRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::DeferredRasterTileCullPlan;
using container::renderer::DeferredRasterTileCullRecordInputs;
using container::renderer::RenderPassReadiness;
using container::renderer::recordDeferredRasterTileCullCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

DeferredRasterTileCullPlan activePlan() {
  return {.active = true,
          .readiness = RenderPassReadiness{},
          .screenExtent = {.width = 1280u, .height = 720u},
          .cameraBuffer = fakeHandle<VkBuffer>(0x1),
          .cameraBufferSize = 64u,
          .depthSamplingView = fakeHandle<VkImageView>(0x2),
          .cameraNear = 0.1f,
          .cameraFar = 300.0f};
}

DeferredRasterTileCullRecordInputs readyInputs() {
  return {.lightingManager =
              reinterpret_cast<const container::renderer::LightingManager *>(0x3),
          .plan = activePlan()};
}

} // namespace

TEST(DeferredRasterTileCullRecorderTests, NullCommandBufferReturnsFalse) {
  EXPECT_FALSE(
      recordDeferredRasterTileCullCommands(VK_NULL_HANDLE, readyInputs()));
}

TEST(DeferredRasterTileCullRecorderTests, NullPlanReturnsFalse) {
  auto inputs = readyInputs();
  inputs.plan = {};

  EXPECT_FALSE(recordDeferredRasterTileCullCommands(
      fakeHandle<VkCommandBuffer>(0x4), inputs));
}

TEST(DeferredRasterTileCullRecorderTests, NotReadyPlanReturnsFalse) {
  auto inputs = readyInputs();
  inputs.plan.readiness.ready = false;

  EXPECT_FALSE(recordDeferredRasterTileCullCommands(
      fakeHandle<VkCommandBuffer>(0x4), inputs));
}

TEST(DeferredRasterTileCullRecorderTests, NullLightingManagerReturnsFalse) {
  auto inputs = readyInputs();
  inputs.lightingManager = nullptr;

  EXPECT_FALSE(recordDeferredRasterTileCullCommands(
      fakeHandle<VkCommandBuffer>(0x4), inputs));
}
