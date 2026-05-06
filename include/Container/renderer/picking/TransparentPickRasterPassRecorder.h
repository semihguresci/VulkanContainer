#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/bim/BimSurfaceRasterPassRecorder.h"
#include "Container/renderer/picking/TransparentPickPassRecorder.h"

namespace container::renderer {

struct TransparentPickRasterPassRecordInputs {
  bool active{false};
  VkRenderPass renderPass{VK_NULL_HANDLE};
  VkFramebuffer framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  TransparentPickPassRecordInputs pass{};
};

struct TransparentPickFramePassRecordInputs {
  bool scenePassReady{false};
  bool bimPassReady{false};
  VkRenderPass renderPass{VK_NULL_HANDLE};
  VkFramebuffer framebuffer{VK_NULL_HANDLE};
  VkExtent2D extent{};
  VkImage sourceDepthStencilImage{VK_NULL_HANDLE};
  VkImage pickDepthImage{VK_NULL_HANDLE};
  VkImage pickIdImage{VK_NULL_HANDLE};
  SceneTransparentDrawLists sceneDraws{};
  BimSurfaceFramePassDrawSources bimDraws{};
  TransparentPickPassGeometryBinding scene{};
  TransparentPickPassGeometryBinding bim{};
  TransparentPickPassPipelineHandles pipelines{};
  VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
  const container::gpu::BindlessPushConstants *pushConstants{nullptr};
  uint32_t bimSemanticColorMode{0};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  BimManager *bimManager{nullptr};
};

[[nodiscard]] bool recordTransparentPickRasterPassCommands(
    VkCommandBuffer cmd, const TransparentPickRasterPassRecordInputs &inputs);

[[nodiscard]] bool recordTransparentPickFramePassCommands(
    VkCommandBuffer cmd, const TransparentPickFramePassRecordInputs &inputs);

} // namespace container::renderer
