#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/scene/SceneTransparentDrawRecorder.h"
#include "Container/renderer/bim/BimSurfacePassRecorder.h"
#include "Container/renderer/effects/OitManager.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <cstdint>
#include <limits>

namespace container::renderer {

struct DeferredTransparentOitGeometryBinding {
  VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
};

struct DeferredTransparentOitPipelineHandles {
  VkPipeline primary{VK_NULL_HANDLE};
  VkPipeline frontCull{VK_NULL_HANDLE};
  VkPipeline noCull{VK_NULL_HANDLE};
};

struct DeferredTransparentOitRecordInputs {
  const SceneTransparentDrawPlan *scenePlan{nullptr};
  const BimSurfacePassPlan *bimPlan{nullptr};
  std::array<VkDescriptorSet, 4> descriptorSets{};
  DeferredTransparentOitGeometryBinding scene{};
  DeferredTransparentOitGeometryBinding bim{};
  DeferredTransparentOitPipelineHandles pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  container::gpu::BindlessPushConstants pushConstants{};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  BimManager *bimManager{nullptr};
};

struct DeferredTransparentOitFrameResourceInputs {
  const OitManager *oitManager{nullptr};
  OitFrameResources resources{};
  uint32_t invalidNodeIndex{std::numeric_limits<uint32_t>::max()};
};

[[nodiscard]] bool recordDeferredTransparentOitCommands(
    VkCommandBuffer cmd, const DeferredTransparentOitRecordInputs &inputs);

[[nodiscard]] bool recordDeferredTransparentOitClearCommands(
    VkCommandBuffer cmd,
    const DeferredTransparentOitFrameResourceInputs &inputs);

[[nodiscard]] bool recordDeferredTransparentOitResolvePreparationCommands(
    VkCommandBuffer cmd,
    const DeferredTransparentOitFrameResourceInputs &inputs);

} // namespace container::renderer
