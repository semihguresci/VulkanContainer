#include "Container/renderer/deferred/DeferredLightGizmoRecorder.h"

namespace container::renderer {

namespace {

constexpr VkShaderStageFlags kLightGizmoPushStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

[[nodiscard]] bool hasReadyDescriptorSets(
    const std::array<VkDescriptorSet, 2> &descriptorSets) {
  for (VkDescriptorSet descriptorSet : descriptorSets) {
    if (descriptorSet == VK_NULL_HANDLE) {
      return false;
    }
  }
  return true;
}

} // namespace

bool recordDeferredLightGizmoCommands(
    VkCommandBuffer cmd, const DeferredLightGizmoRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE || inputs.pipeline == VK_NULL_HANDLE ||
      inputs.pipelineLayout == VK_NULL_HANDLE ||
      !hasReadyDescriptorSets(inputs.descriptorSets) ||
      inputs.pushConstants.empty() || inputs.vertexCount == 0u) {
    return false;
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inputs.pipeline);
  vkCmdBindDescriptorSets(
      cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, inputs.pipelineLayout, 0,
      static_cast<uint32_t>(inputs.descriptorSets.size()),
      inputs.descriptorSets.data(), 0, nullptr);

  for (const LightPushConstants &pushConstants : inputs.pushConstants) {
    vkCmdPushConstants(cmd, inputs.pipelineLayout, kLightGizmoPushStages, 0,
                       sizeof(LightPushConstants), &pushConstants);
    vkCmdDraw(cmd, inputs.vertexCount, 1, 0, 0);
  }
  return true;
}

} // namespace container::renderer
