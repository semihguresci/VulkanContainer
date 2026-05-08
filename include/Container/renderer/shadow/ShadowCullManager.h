#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}  // namespace container::gpu

namespace container::renderer {

class ShadowCullManager {
 public:
  ShadowCullManager(
	  std::shared_ptr<container::gpu::VulkanDevice> device,
	  container::gpu::AllocationManager&            allocationManager,
	  container::gpu::PipelineManager&              pipelineManager);

  ~ShadowCullManager();
  ShadowCullManager(const ShadowCullManager&) = delete;
  ShadowCullManager& operator=(const ShadowCullManager&) = delete;

	void createResources(const std::filesystem::path& shaderDir,
					   uint32_t descriptorSetCount);
  void recreatePerFrameResources(uint32_t descriptorSetCount);
  bool ensureBufferCapacity(uint32_t maxDrawCount);
  void uploadDrawCommands(const std::vector<DrawCommand>& commands);

  void updateObjectSsboDescriptor(VkBuffer objectBuffer,
								  VkDeviceSize objectBufferSize);
	void updateShadowCullDescriptor(uint32_t imageIndex,
								  VkBuffer shadowCullBuffer,
								  VkDeviceSize shadowCullBufferSize);

  void dispatchCascadeCull(VkCommandBuffer cmd,
					   uint32_t imageIndex,
						   uint32_t cascadeIndex,
						   uint32_t drawCount,
						   uint32_t outputOffset = 0);

  [[nodiscard]] VkBuffer indirectDrawBuffer(uint32_t cascadeIndex) const {
	return cascadeIndex < container::gpu::kShadowCascadeCount
			   ? indirectDrawBuffers_[cascadeIndex].buffer
			   : VK_NULL_HANDLE;
  }
  [[nodiscard]] VkBuffer drawCountBuffer(uint32_t cascadeIndex) const {
	return cascadeIndex < container::gpu::kShadowCascadeCount
			   ? drawCountBuffers_[cascadeIndex].buffer
			   : VK_NULL_HANDLE;
  }
  [[nodiscard]] uint32_t maxDrawCount() const { return maxDrawCount_; }
  [[nodiscard]] bool isReady() const;

 private:
	void createShadowCullPipeline(const std::filesystem::path& shaderDir);
	void writeDescriptorSets(uint32_t imageIndex);
  [[nodiscard]] size_t descriptorSetIndex(uint32_t imageIndex,
										  uint32_t cascadeIndex) const;

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::PipelineManager&              pipelineManager_;

  uint32_t maxDrawCount_{0};
	uint32_t objectCount_{0};
  VkDeviceSize objectSsboSize_{0};
  VkDeviceSize shadowCullUboSize_{0};
  std::vector<container::gpu::GpuDrawIndexedIndirectCommand> uploadScratch_{};

	std::vector<VkBuffer> shadowCullBuffers_{};
	VkBuffer objectSsboBuffer_{VK_NULL_HANDLE};
	container::gpu::AllocatedBuffer ownedShadowCullUbo_{};
  container::gpu::AllocatedBuffer inputDrawBuffer_{};
  std::array<container::gpu::AllocatedBuffer, container::gpu::kShadowCascadeCount>
	  indirectDrawBuffers_{};
  std::array<container::gpu::AllocatedBuffer, container::gpu::kShadowCascadeCount>
	  drawCountBuffers_{};

  VkPipeline            shadowCullPipeline_{VK_NULL_HANDLE};
  VkPipelineLayout      shadowCullPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout shadowCullSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool      shadowCullPool_{VK_NULL_HANDLE};
	std::vector<VkDescriptorSet> shadowCullSets_{};
};

}  // namespace container::renderer
