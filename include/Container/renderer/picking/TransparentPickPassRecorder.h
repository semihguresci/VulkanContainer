#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/scene/SceneTransparentDrawRecorder.h"
#include "Container/renderer/bim/BimSurfacePassRecorder.h"
#include "Container/utility/VulkanMemoryManager.h"

namespace container::renderer {

struct TransparentPickPassGeometryBinding {
  VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
};

struct TransparentPickPassPipelineHandles {
  VkPipeline primary{VK_NULL_HANDLE};
  VkPipeline frontCull{VK_NULL_HANDLE};
  VkPipeline noCull{VK_NULL_HANDLE};
};

struct TransparentPickPassRecordInputs {
  const SceneTransparentDrawPlan *scenePlan{nullptr};
  const BimSurfacePassPlan *bimPlan{nullptr};
  TransparentPickPassGeometryBinding scene{};
  TransparentPickPassGeometryBinding bim{};
  TransparentPickPassPipelineHandles pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  container::gpu::BindlessPushConstants pushConstants{};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  BimManager *bimManager{nullptr};
};

[[nodiscard]] bool recordTransparentPickPassCommands(
    VkCommandBuffer cmd, const TransparentPickPassRecordInputs &inputs);

} // namespace container::renderer
