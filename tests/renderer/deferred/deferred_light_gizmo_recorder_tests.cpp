#include "Container/renderer/deferred/DeferredLightGizmoRecorder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using container::renderer::DeferredLightGizmoRecordInputs;
using container::renderer::LightPushConstants;
using container::renderer::recordDeferredLightGizmoCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

DeferredLightGizmoRecordInputs requiredInputs() {
  static const std::array<LightPushConstants, 1> pushConstants = {
      LightPushConstants{}};
  return {.pipeline = fakeHandle<VkPipeline>(0x1),
          .pipelineLayout = fakeHandle<VkPipelineLayout>(0x2),
          .descriptorSets = {fakeHandle<VkDescriptorSet>(0x3),
                             fakeHandle<VkDescriptorSet>(0x4)},
          .pushConstants = pushConstants};
}

} // namespace

TEST(DeferredLightGizmoRecorderTests, MissingCommandBufferReturnsFalse) {
  DeferredLightGizmoRecordInputs inputs = requiredInputs();

  EXPECT_FALSE(recordDeferredLightGizmoCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredLightGizmoRecorderTests, MissingPipelineReturnsFalse) {
  DeferredLightGizmoRecordInputs inputs = requiredInputs();
  inputs.pipeline = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredLightGizmoCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredLightGizmoRecorderTests, MissingPipelineLayoutReturnsFalse) {
  DeferredLightGizmoRecordInputs inputs = requiredInputs();
  inputs.pipelineLayout = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredLightGizmoCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredLightGizmoRecorderTests, MissingLightingDescriptorSetReturnsFalse) {
  DeferredLightGizmoRecordInputs inputs = requiredInputs();
  inputs.descriptorSets[0] = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredLightGizmoCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredLightGizmoRecorderTests, MissingLightDescriptorSetReturnsFalse) {
  DeferredLightGizmoRecordInputs inputs = requiredInputs();
  inputs.descriptorSets[1] = VK_NULL_HANDLE;

  EXPECT_FALSE(recordDeferredLightGizmoCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredLightGizmoRecorderTests, EmptyPushConstantsReturnFalse) {
  DeferredLightGizmoRecordInputs inputs = requiredInputs();
  inputs.pushConstants = {};

  EXPECT_FALSE(recordDeferredLightGizmoCommands(VK_NULL_HANDLE, inputs));
}

TEST(DeferredLightGizmoRecorderTests, ZeroVertexCountReturnsFalse) {
  DeferredLightGizmoRecordInputs inputs = requiredInputs();
  inputs.vertexCount = 0u;

  EXPECT_FALSE(recordDeferredLightGizmoCommands(VK_NULL_HANDLE, inputs));
}
