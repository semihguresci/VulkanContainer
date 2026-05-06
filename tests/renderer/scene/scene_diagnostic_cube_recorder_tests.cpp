#include "Container/renderer/scene/SceneDiagnosticCubeRecorder.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using container::gpu::BindlessPushConstants;
using container::renderer::SceneDiagnosticCubeRecordInputs;
using container::renderer::recordSceneDiagnosticCubeCommands;

template <typename Handle> Handle fakeHandle(uintptr_t value) {
  return reinterpret_cast<Handle>(value);
}

VkCommandBuffer commandBuffer() {
  return fakeHandle<VkCommandBuffer>(0x1);
}

SceneDiagnosticCubeRecordInputs requiredInputs() {
  SceneDiagnosticCubeRecordInputs inputs{};
  inputs.pipeline = fakeHandle<VkPipeline>(0x2);
  inputs.pipelineLayout = fakeHandle<VkPipelineLayout>(0x3);
  inputs.descriptorSet = fakeHandle<VkDescriptorSet>(0x4);
  inputs.objectIndex = 7u;
  inputs.pushConstants = BindlessPushConstants{};
  inputs.geometry.vertexSlice.buffer = fakeHandle<VkBuffer>(0x5);
  inputs.geometry.indexSlice.buffer = fakeHandle<VkBuffer>(0x6);
  inputs.geometry.indexCount = 36u;
  return inputs;
}

} // namespace

TEST(SceneDiagnosticCubeRecorderTests, EmptyInputsReturnFalse) {
  EXPECT_FALSE(recordSceneDiagnosticCubeCommands(VK_NULL_HANDLE, {}));
}

TEST(SceneDiagnosticCubeRecorderTests, MissingCommandBufferReturnsFalse) {
  EXPECT_FALSE(recordSceneDiagnosticCubeCommands(VK_NULL_HANDLE,
                                                requiredInputs()));
}

TEST(SceneDiagnosticCubeRecorderTests, MissingPipelineReturnsFalse) {
  SceneDiagnosticCubeRecordInputs inputs = requiredInputs();
  inputs.pipeline = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSceneDiagnosticCubeCommands(commandBuffer(), inputs));
}

TEST(SceneDiagnosticCubeRecorderTests, MissingPipelineLayoutReturnsFalse) {
  SceneDiagnosticCubeRecordInputs inputs = requiredInputs();
  inputs.pipelineLayout = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSceneDiagnosticCubeCommands(commandBuffer(), inputs));
}

TEST(SceneDiagnosticCubeRecorderTests, MissingDescriptorSetReturnsFalse) {
  SceneDiagnosticCubeRecordInputs inputs = requiredInputs();
  inputs.descriptorSet = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSceneDiagnosticCubeCommands(commandBuffer(), inputs));
}

TEST(SceneDiagnosticCubeRecorderTests, MissingObjectIndexReturnsFalse) {
  SceneDiagnosticCubeRecordInputs inputs = requiredInputs();
  inputs.objectIndex = std::numeric_limits<uint32_t>::max();

  EXPECT_FALSE(recordSceneDiagnosticCubeCommands(commandBuffer(), inputs));
}

TEST(SceneDiagnosticCubeRecorderTests, MissingVertexBufferReturnsFalse) {
  SceneDiagnosticCubeRecordInputs inputs = requiredInputs();
  inputs.geometry.vertexSlice.buffer = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSceneDiagnosticCubeCommands(commandBuffer(), inputs));
}

TEST(SceneDiagnosticCubeRecorderTests, MissingIndexBufferReturnsFalse) {
  SceneDiagnosticCubeRecordInputs inputs = requiredInputs();
  inputs.geometry.indexSlice.buffer = VK_NULL_HANDLE;

  EXPECT_FALSE(recordSceneDiagnosticCubeCommands(commandBuffer(), inputs));
}

TEST(SceneDiagnosticCubeRecorderTests, MissingIndexCountReturnsFalse) {
  SceneDiagnosticCubeRecordInputs inputs = requiredInputs();
  inputs.geometry.indexCount = 0u;

  EXPECT_FALSE(recordSceneDiagnosticCubeCommands(commandBuffer(), inputs));
}
