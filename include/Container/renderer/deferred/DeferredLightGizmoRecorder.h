#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/lighting/LightPushConstants.h"

#include <array>
#include <cstdint>
#include <span>

namespace container::renderer {

struct DeferredLightGizmoRecordInputs {
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  std::array<VkDescriptorSet, 2> descriptorSets{};
  std::span<const LightPushConstants> pushConstants{};
  uint32_t vertexCount{6u};
};

[[nodiscard]] bool recordDeferredLightGizmoCommands(
    VkCommandBuffer cmd, const DeferredLightGizmoRecordInputs &inputs);

} // namespace container::renderer
