#include "Container/renderer/deferred/DeferredRasterGuiPassRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using container::renderer::DeferredRasterGuiPassRecordInputs;
using container::renderer::recordDeferredRasterGuiPass;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

} // namespace

TEST(DeferredRasterGuiPassRecorderTests, RejectsMissingCommandBuffer) {
  EXPECT_FALSE(recordDeferredRasterGuiPass(
      {.commandBuffer = VK_NULL_HANDLE,
       .guiManager = reinterpret_cast<container::ui::GuiManager *>(0x1)}));
}

TEST(DeferredRasterGuiPassRecorderTests, RejectsMissingGuiManager) {
  EXPECT_FALSE(recordDeferredRasterGuiPass(
      DeferredRasterGuiPassRecordInputs{
          .commandBuffer = fakeHandle<VkCommandBuffer>(0x2)}));
}
