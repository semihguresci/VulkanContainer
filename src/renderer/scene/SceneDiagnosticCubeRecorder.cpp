#include "Container/renderer/scene/SceneDiagnosticCubeRecorder.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasReadyGeometry(
    const SceneDiagnosticCubeGeometry &geometry) {
  return geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexCount > 0u;
}

[[nodiscard]] bool hasRequiredInputs(
    VkCommandBuffer cmd, const SceneDiagnosticCubeRecordInputs &inputs) {
  return cmd != VK_NULL_HANDLE && inputs.pipeline != VK_NULL_HANDLE &&
         inputs.pipelineLayout != VK_NULL_HANDLE &&
         inputs.descriptorSet != VK_NULL_HANDLE &&
         inputs.objectIndex != std::numeric_limits<uint32_t>::max() &&
         hasReadyGeometry(inputs.geometry);
}

void bindDiagnosticCubeGeometry(VkCommandBuffer cmd,
                                const SceneDiagnosticCubeGeometry &geometry) {
  const VkDeviceSize offsets[] = {geometry.vertexSlice.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, &geometry.vertexSlice.buffer, offsets);
  vkCmdBindIndexBuffer(cmd, geometry.indexSlice.buffer,
                       geometry.indexSlice.offset, geometry.indexType);
}

void pushDiagnosticCubeObjectIndex(
    VkCommandBuffer cmd, VkPipelineLayout pipelineLayout,
    container::gpu::BindlessPushConstants &pushConstants,
    uint32_t objectIndex) {
  pushConstants.objectIndex = objectIndex;
  vkCmdPushConstants(cmd, pipelineLayout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(container::gpu::BindlessPushConstants),
                     &pushConstants);
}

} // namespace

bool recordSceneDiagnosticCubeCommands(
    VkCommandBuffer cmd, const SceneDiagnosticCubeRecordInputs &inputs) {
  if (!hasRequiredInputs(cmd, inputs)) {
    return false;
  }

  container::gpu::BindlessPushConstants pushConstants = inputs.pushConstants;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inputs.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          inputs.pipelineLayout, 0, 1, &inputs.descriptorSet,
                          0, nullptr);
  bindDiagnosticCubeGeometry(cmd, inputs.geometry);
  pushDiagnosticCubeObjectIndex(cmd, inputs.pipelineLayout, pushConstants,
                                inputs.objectIndex);
  vkCmdDrawIndexed(cmd, inputs.geometry.indexCount, 1, 0, 0,
                   inputs.objectIndex);
  return true;
}

} // namespace container::renderer
