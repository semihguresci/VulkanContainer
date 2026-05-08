#include "Container/renderer/deferred/DeferredTransparentOitFramePassRecorder.h"

#include "Container/renderer/core/FrameRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::DeferredTransparentOitFramePassRecorder;
using container::renderer::FrameRecordParams;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

} // namespace

TEST(DeferredTransparentOitFramePassRecorderTests,
     EmptyFrameIsDisabledAndNotNeeded) {
  DeferredTransparentOitFramePassRecorder recorder({});
  const FrameRecordParams params{};

  EXPECT_FALSE(recorder.enabled(params));
  EXPECT_FALSE(recorder.readiness(params).ready);
}

TEST(DeferredTransparentOitFramePassRecorderTests,
     ClearAndResolveRejectDisabledFrame) {
  DeferredTransparentOitFramePassRecorder recorder({});
  const FrameRecordParams params{};

  EXPECT_FALSE(recorder.recordClear(fakeHandle<VkCommandBuffer>(0x1), params));
  EXPECT_FALSE(
      recorder.recordResolvePreparation(fakeHandle<VkCommandBuffer>(0x2),
                                        params));
}
