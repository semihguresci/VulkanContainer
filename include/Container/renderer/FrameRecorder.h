#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/DebugOverlayRenderer.h"
#include "Container/renderer/FrameResources.h"
#include "Container/renderer/PipelineTypes.h"
#include "Container/renderer/RenderGraph.h"
#include "Container/utility/SceneData.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace container::gpu {
class SwapChainManager;
class VulkanDevice;
}  // namespace container::gpu

// Forward declarations — full headers only needed in FrameRecorder.cpp.
namespace container::renderer {
class BloomManager;
class EnvironmentManager;
class GpuCullManager;
class LightingManager;
class OitManager;
class SceneController;
class ShadowCullManager;
class ShadowManager;
struct LightPushConstants;
}  // namespace container::renderer

namespace container::ui {
class GuiManager;
}

namespace container::scene {
class BaseCamera;
}

namespace container::renderer {

// All per-frame render state needed by FrameRecorder::record().
struct FrameRecordParams {
  // Per-frame GPU resources
  const FrameResources*             frame{nullptr};
  uint32_t                          imageIndex{0};

  // Scene geometry
  container::gpu::BufferSlice      vertexSlice{};
  container::gpu::BufferSlice      indexSlice{};
  VkIndexType                       indexType{VK_INDEX_TYPE_UINT32};
  const std::vector<DrawCommand>*   opaqueDrawCommands{nullptr};
  const std::vector<DrawCommand>*   transparentDrawCommands{nullptr};
  const std::vector<DrawCommand>*   opaqueSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand>*   opaqueDoubleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand>*   transparentSingleSidedDrawCommands{nullptr};
  const std::vector<DrawCommand>*   transparentDoubleSidedDrawCommands{nullptr};
  const std::vector<container::gpu::ObjectData>* objectData{nullptr};

  // Descriptor sets
  VkDescriptorSet                   sceneDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet                   lightDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet                   shadowDescriptorSet{VK_NULL_HANDLE};
  VkDescriptorSet                   tiledDescriptorSet{VK_NULL_HANDLE};

  // Tiled culling resources
  VkBuffer                          cameraBuffer{VK_NULL_HANDLE};
  VkDeviceSize                      cameraBufferSize{0};
  VkSampler                         gBufferSampler{VK_NULL_HANDLE};

  // Render passes
  struct RenderPassHandles {
    VkRenderPass depthPrepass{VK_NULL_HANDLE};
    VkRenderPass gBuffer{VK_NULL_HANDLE};
    VkRenderPass shadow{VK_NULL_HANDLE};
    VkRenderPass lighting{VK_NULL_HANDLE};
    VkRenderPass postProcess{VK_NULL_HANDLE};
  };
  RenderPassHandles                 renderPasses{};

  // Pipelines
  PipelineLayouts                   layouts{};
  GraphicsPipelines                 pipelines{};

  // Debug flags
  bool                              debugDirectionalOnly{false};
  bool                              debugVisualizePointLightStencil{false};
  bool                              debugFreezeCulling{false};
  bool                              wireframeRasterModeSupported{false};
  bool                              wireframeWideLinesSupported{false};

  // Push constant state (mutated during recording)
  struct PushConstantState {
    container::gpu::BindlessPushConstants*            bindless{nullptr};
    LightPushConstants*               light{nullptr};
    WireframePushConstants*           wireframe{nullptr};
    NormalValidationPushConstants*    normalValidation{nullptr};
    SurfaceNormalPushConstants*       surfaceNormal{nullptr};
  };
  PushConstantState                 pushConstants{};

  // Camera near/far planes (for depth linearization)
  float                             cameraNear{0.1f};
  float                             cameraFar{100.0f};

  // Shadow cascade framebuffers (kShadowCascadeCount entries)
  const VkFramebuffer*              shadowFramebuffers{nullptr};
  container::gpu::ShadowPushConstants*  shadowPushConstants{nullptr};
  const container::gpu::ShadowData*     shadowData{nullptr};
  bool                                 useGpuShadowCull{false};
  ShadowCullManager*                   shadowCullManager{nullptr};
  std::array<VkBuffer, container::gpu::kShadowCascadeCount> shadowCullIndirectBuffers{};
  std::array<VkBuffer, container::gpu::kShadowCascadeCount> shadowCullCountBuffers{};
  uint32_t                             shadowCullMaxDrawCount{0};
  const ShadowManager*                  shadowManager{nullptr};

  // Swapchain framebuffers (for post-process pass)
  const std::vector<VkFramebuffer>* swapChainFramebuffers{nullptr};

  // Diagnostic cube object index (max uint32 = disabled)
  uint32_t                          diagCubeObjectIndex{std::numeric_limits<uint32_t>::max()};

  // GPU-driven culling
  GpuCullManager*                   gpuCullManager{nullptr};
  BloomManager*                      bloomManager{nullptr};
  VkBuffer                          objectBuffer{VK_NULL_HANDLE};
  VkDeviceSize                      objectBufferSize{0};
};

// Records a complete frame into a command buffer.
// All render passes: depth prepass → G-Buffer → lighting → post-process.
// Debug overlays: wireframe, normal validation, surface normals, light gizmos.
class FrameRecorder {
 public:
  FrameRecorder(std::shared_ptr<container::gpu::VulkanDevice> device,
                container::gpu::SwapChainManager&                      swapChainManager,
                const OitManager&                               oitManager,
                const LightingManager*                          lightingManager,
                const EnvironmentManager*                       environmentManager,
                const SceneController*                          sceneController,
                GpuCullManager*                                 gpuCullManager,
                BloomManager*                                   bloomManager,
                const container::scene::BaseCamera*              camera,
                container::ui::GuiManager*                        guiManager);

  // Build the render graph.  Must be called once after construction and
  // again after any structural change (e.g. pipeline rebuild).
  void buildGraph();

  // Records a complete frame into commandBuffer.
  void record(VkCommandBuffer commandBuffer, const FrameRecordParams& params) const;

  // Access the graph for external pass toggling or inspection.
  RenderGraph&       graph()       { return graph_; }
  const RenderGraph& graph() const { return graph_; }

 private:
  void setViewportAndScissor(VkCommandBuffer cmd) const;

  void bindSceneGeometryBuffers(VkCommandBuffer cmd,
                                container::gpu::BufferSlice vertex,
                                container::gpu::BufferSlice index,
                                VkIndexType indexType) const;

  void drawDiagnosticCube(VkCommandBuffer cmd,
                          VkPipelineLayout layout,
                          uint32_t diagCubeObjectIndex,
                          container::gpu::BindlessPushConstants& pc) const;

  void recordDepthPrepass(VkCommandBuffer cmd,
                          const FrameRecordParams& p,
                          VkDescriptorSet sceneSet) const;

  void recordGBufferPass(VkCommandBuffer cmd,
                         const FrameRecordParams& p,
                         VkDescriptorSet sceneSet) const;

  void recordShadowPass(VkCommandBuffer cmd,
                        const FrameRecordParams& p,
                        uint32_t cascadeIndex) const;

  void prepareShadowCascadeDrawCommands(const FrameRecordParams& p) const;

  void recordLightingPass(VkCommandBuffer cmd,
                          const FrameRecordParams& p,
                          VkDescriptorSet sceneSet,
                          const std::array<VkDescriptorSet, 2>& lightingSets,
                          const std::array<VkDescriptorSet, 3>& transparentSets) const;

  void recordPostProcessPass(VkCommandBuffer cmd,
                              const FrameRecordParams& p,
                              const std::array<VkDescriptorSet, 2>& postProcessSets) const;

  std::shared_ptr<container::gpu::VulkanDevice> device_;
  container::gpu::SwapChainManager&                      swapChainManager_;
  const OitManager&                               oitManager_;
  const LightingManager*                          lightingManager_;
  const EnvironmentManager*                       environmentManager_;
  const SceneController*                          sceneController_;
  GpuCullManager*                                 gpuCullManager_;
  BloomManager*                                    bloomManager_;
  const container::scene::BaseCamera*              camera_;
  container::ui::GuiManager*                  guiManager_;

  DebugOverlayRenderer debugOverlay_;
  RenderGraph          graph_;
  mutable std::array<std::vector<DrawCommand>, container::gpu::kShadowCascadeCount>
      shadowCascadeSingleSidedDrawCommands_;
  mutable std::array<std::vector<DrawCommand>, container::gpu::kShadowCascadeCount>
      shadowCascadeDoubleSidedDrawCommands_;
};

}  // namespace container::renderer
