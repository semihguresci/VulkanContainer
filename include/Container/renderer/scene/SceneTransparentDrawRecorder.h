#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/renderer/scene/SceneTransparentDrawPlanner.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <span>

namespace container::renderer {

struct SceneTransparentDrawGeometryBinding {
  std::span<const VkDescriptorSet> descriptorSets{};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
};

struct SceneTransparentDrawPipelineHandles {
  VkPipeline primary{VK_NULL_HANDLE};
  VkPipeline frontCull{VK_NULL_HANDLE};
  VkPipeline noCull{VK_NULL_HANDLE};
};

struct SceneTransparentDrawRecordInputs {
  const SceneTransparentDrawPlan *plan{nullptr};
  SceneTransparentDrawGeometryBinding geometry{};
  SceneTransparentDrawPipelineHandles pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  container::gpu::BindlessPushConstants pushConstants{};
  const DebugOverlayRenderer *debugOverlay{nullptr};
};

[[nodiscard]] bool recordSceneTransparentDrawCommands(
    VkCommandBuffer cmd, const SceneTransparentDrawRecordInputs &inputs);

} // namespace container::renderer
