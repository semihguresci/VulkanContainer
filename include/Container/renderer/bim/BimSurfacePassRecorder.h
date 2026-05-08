#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/renderer/bim/BimSurfacePassPlanner.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <span>

namespace container::renderer {

class BimManager;

struct BimSurfacePassGeometryBinding {
  std::span<const VkDescriptorSet> descriptorSets{};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
};

struct BimSurfacePassRecordInputs {
  const BimSurfacePassPlan *plan{nullptr};
  BimSurfacePassGeometryBinding geometry{};
  VkPipeline singleSidedPipeline{VK_NULL_HANDLE};
  VkPipeline windingFlippedPipeline{VK_NULL_HANDLE};
  VkPipeline doubleSidedPipeline{VK_NULL_HANDLE};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  container::gpu::BindlessPushConstants pushConstants{};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  BimManager *bimManager{nullptr};
};

[[nodiscard]] bool
recordBimSurfacePassCommands(VkCommandBuffer cmd,
                             const BimSurfacePassRecordInputs &inputs);

} // namespace container::renderer
