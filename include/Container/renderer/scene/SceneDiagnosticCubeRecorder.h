#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>
#include <limits>

namespace container::renderer {

struct SceneDiagnosticCubeGeometry {
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
  uint32_t indexCount{0};
};

struct SceneDiagnosticCubeRecordInputs {
  SceneDiagnosticCubeGeometry geometry{};
  VkPipeline pipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  container::gpu::BindlessPushConstants pushConstants{};
};

[[nodiscard]] bool recordSceneDiagnosticCubeCommands(
    VkCommandBuffer cmd, const SceneDiagnosticCubeRecordInputs &inputs);

} // namespace container::renderer
