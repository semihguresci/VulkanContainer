#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonMath.h"
#include "Container/renderer/DebugOverlayRenderer.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
}  // namespace container::gpu

namespace container::renderer {

// Culling statistics — available one frame after dispatch (no GPU stall).
struct CullStats {
  uint32_t totalInputCount{0};        // Objects submitted for culling.
  uint32_t frustumPassedCount{0};     // Objects that passed frustum culling.
  uint32_t occlusionPassedCount{0};   // Objects that passed occlusion culling.
};

// Manages GPU-driven indirect draw buffers, frustum culling compute
// pipeline, Hi-Z mip chain, and occlusion culling compute pipeline.
class GpuCullManager {
 public:
  GpuCullManager(
      std::shared_ptr<container::gpu::VulkanDevice> device,
      container::gpu::AllocationManager&            allocationManager,
      container::gpu::PipelineManager&            pipelineManager);

  ~GpuCullManager();
  GpuCullManager(const GpuCullManager&) = delete;
  GpuCullManager& operator=(const GpuCullManager&) = delete;

  // Create compute pipelines and descriptor resources.
  void createResources(const std::filesystem::path& shaderDir);

  // Ensure indirect draw buffers are large enough for the given object count.
  // Returns true if buffers were recreated (descriptor sets need re-write).
  bool ensureBufferCapacity(uint32_t maxObjectCount);

  // Upload CPU-side draw commands to the input SSBO and prepare for culling.
  void uploadDrawCommands(const std::vector<DrawCommand>& commands);

  // Dispatch frustum culling compute shader.  After this call, the indirect
  // draw buffer and draw count buffer are ready for vkCmdDrawIndexedIndirect.
  void dispatchFrustumCull(VkCommandBuffer cmd,
                           VkBuffer cameraBuffer,
                           VkDeviceSize cameraBufferSize,
                           uint32_t objectCount);

  // Dispatch Hi-Z mip chain generation from depth image.
  void dispatchHiZGenerate(VkCommandBuffer cmd,
                           VkImageView depthView,
                           VkSampler depthSampler,
                           uint32_t width, uint32_t height);

  // Dispatch occlusion culling against Hi-Z pyramid.
  void dispatchOcclusionCull(VkCommandBuffer cmd,
                             VkBuffer cameraBuffer,
                             VkDeviceSize cameraBufferSize,
                             uint32_t objectCount);

  // Issue a single vkCmdDrawIndexedIndirectCount or equivalent.
  // Uses frustum-culled results (depth prepass + shadow passes).
  void drawIndirect(VkCommandBuffer cmd) const;

  // Issue indirect draw from occlusion-culled results (G-Buffer pass).
  void drawIndirectOccluded(VkCommandBuffer cmd) const;

  // Ensure the Hi-Z image matches the given depth buffer dimensions.
  // Call once per swapchain resize.
  void ensureHiZImage(uint32_t width, uint32_t height);

  // Access the indirect draw count (for secondary passes like shadow that
  // don't do occlusion culling, we use the frustum-culled count).
  VkBuffer indirectDrawBuffer() const { return indirectDrawBuffer_.buffer; }
  VkBuffer drawCountBuffer() const { return drawCountBuffer_.buffer; }
  uint32_t maxDrawCount() const { return maxObjectCount_; }

  bool isReady() const { return frustumCullPipeline_ != VK_NULL_HANDLE; }

  // Update the object SSBO descriptor (binding 1) to point at the scene's
  // object buffer.  Call whenever the object buffer is recreated.
  void updateObjectSsboDescriptor(VkBuffer objectBuffer,
                                  VkDeviceSize objectBufferSize);

  // Retrieve culling statistics from the previous frame (1-frame latency).
  // Returns all zeros until the first readback completes.
  CullStats cullStats() const { return lastStats_; }

  // Schedule a readback of the draw count buffers into staging memory.
  // Call once per frame after all cull dispatches complete.
  void scheduleStatsReadback(VkCommandBuffer cmd);

  // Read back the staged data into lastStats_.  Call at the start of
  // the next frame (after the fence for the previous frame has signaled).
  void collectStats();

  // Freeze the culling camera: subsequent cull dispatches will use a
  // snapshot of the current camera data instead of the live camera.
  // Pass the current camera buffer contents to capture.
  void freezeCulling(VkCommandBuffer cmd, VkBuffer liveCameraBuffer,
                     VkDeviceSize cameraBufferSize);

  // Unfreeze: go back to using the live camera for culling.
  void unfreezeCulling();

  // True when culling is using a frozen camera snapshot.
  [[nodiscard]] bool cullingFrozen() const { return cullingFrozen_; }

  // When frozen, returns the frozen camera buffer; otherwise VK_NULL_HANDLE.
  [[nodiscard]] VkBuffer frozenCameraBuffer() const {
    return cullingFrozen_ ? frozenCameraBuffer_.buffer : VK_NULL_HANDLE;
  }

 private:
  void createFrustumCullPipeline(const std::filesystem::path& shaderDir);
  void createHiZPipeline(const std::filesystem::path& shaderDir);
  void createOcclusionCullPipeline(const std::filesystem::path& shaderDir);
  void writeDescriptorSets();
  void destroyHiZImage();

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::AllocationManager&            allocationManager_;
  container::gpu::PipelineManager&            pipelineManager_;

  uint32_t maxObjectCount_{0};

  // Input: draw commands + object bounding spheres (read by cull shaders).
  container::gpu::AllocatedBuffer inputDrawBuffer_{};   // DrawCommand[]
  container::gpu::AllocatedBuffer objectSsbo_{};        // ObjectData[] (ref to scene SSBO)

  // Output: VkDrawIndexedIndirectCommand[] written by cull shader.
  container::gpu::AllocatedBuffer indirectDrawBuffer_{};
  // Output: draw count (single uint32).
  container::gpu::AllocatedBuffer drawCountBuffer_{};

  // Second output: occlusion-culled indirect commands for G-Buffer pass.
  container::gpu::AllocatedBuffer occlusionIndirectBuffer_{};
  container::gpu::AllocatedBuffer occlusionCountBuffer_{};

  // Frustum cull compute pipeline.
  VkPipeline           frustumCullPipeline_{VK_NULL_HANDLE};
  VkPipelineLayout     frustumCullPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout frustumCullSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool     frustumCullPool_{VK_NULL_HANDLE};
  VkDescriptorSet      frustumCullSet_{VK_NULL_HANDLE};

  // Hi-Z generation.
  VkPipeline           hizPipeline_{VK_NULL_HANDLE};
  VkPipelineLayout     hizPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout hizSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool     hizPool_{VK_NULL_HANDLE};
  VkDescriptorSet      hizSet_{VK_NULL_HANDLE};
  VkImage              hizImage_{VK_NULL_HANDLE};
  VmaAllocation        hizAllocation_{nullptr};
  VkImageView          hizFullView_{VK_NULL_HANDLE};     // All mip levels (for sampling).
  std::vector<VkImageView> hizMipViews_;                  // Per-mip views (for storage writes).
  VkSampler            hizSampler_{VK_NULL_HANDLE};
  uint32_t             hizWidth_{0};
  uint32_t             hizHeight_{0};
  uint32_t             hizMipLevels_{0};

  // Occlusion cull compute pipeline.
  VkPipeline           occlusionCullPipeline_{VK_NULL_HANDLE};
  VkPipelineLayout     occlusionCullPipelineLayout_{VK_NULL_HANDLE};
  VkDescriptorSetLayout occlusionCullSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool     occlusionCullPool_{VK_NULL_HANDLE};
  VkDescriptorSet      occlusionCullSet_{VK_NULL_HANDLE};

  // Stats readback (1-frame latency).
  container::gpu::AllocatedBuffer statsReadbackBuffer_{};  // 2 × uint32_t, HOST_VISIBLE
  CullStats lastStats_{};

  // Freeze-culling: snapshot of camera data used for cull dispatches.
  container::gpu::AllocatedBuffer frozenCameraBuffer_{};   // CameraData, GPU-only
  bool cullingFrozen_{false};
};

}  // namespace container::renderer
