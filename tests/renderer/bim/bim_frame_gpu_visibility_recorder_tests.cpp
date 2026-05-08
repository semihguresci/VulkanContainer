#include "Container/renderer/bim/BimFrameGpuVisibilityRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::BimFrameGpuVisibilityRecordInputs;
using container::renderer::prepareBimFrameGpuVisibility;
using container::renderer::recordBimFrameGpuVisibilityCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

} // namespace

TEST(BimFrameGpuVisibilityRecorderTests,
     PrepareWithMissingManagerIsNoOp) {
  EXPECT_NO_THROW(prepareBimFrameGpuVisibility(nullptr));
}

TEST(BimFrameGpuVisibilityRecorderTests,
     RecordWithMissingManagerReturnsFalse) {
  EXPECT_FALSE(recordBimFrameGpuVisibilityCommands(
      {.commandBuffer = fakeHandle<VkCommandBuffer>(0x1)}));
}

TEST(BimFrameGpuVisibilityRecorderTests,
     RecordWithMissingCommandBufferReturnsFalse) {
  EXPECT_FALSE(recordBimFrameGpuVisibilityCommands(
      BimFrameGpuVisibilityRecordInputs{}));
}
