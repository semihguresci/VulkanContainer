#include "Container/renderer/FrameRecorder.h"
#include "Container/renderer/BloomManager.h"
#include "Container/renderer/EnvironmentManager.h"
#include "Container/renderer/GpuCullManager.h"
#include "Container/renderer/LightingManager.h"
#include "Container/renderer/OitManager.h"
#include "Container/renderer/SceneController.h"
#include "Container/renderer/ShadowManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/VulkanDevice.h"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace container::renderer {

using container::gpu::BindlessPushConstants;
using container::gpu::kMaxDeferredPointLights;
using container::gpu::kTileSize;
using container::gpu::kShadowCascadeCount;
using container::gpu::kShadowMapResolution;
using container::gpu::LightingData;
using container::gpu::ShadowPushConstants;
using container::gpu::PostProcessPushConstants;
using container::gpu::TiledLightingPushConstants;

FrameRecorder::FrameRecorder(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::SwapChainManager& swapChainManager,
    const OitManager& oitManager,
    const LightingManager* lightingManager,
    const EnvironmentManager* environmentManager,
    const SceneController* sceneController,
    GpuCullManager* gpuCullManager,
    BloomManager* bloomManager,
    const container::scene::BaseCamera* camera,
    container::ui::GuiManager* guiManager)
    : device_(std::move(device))
    , swapChainManager_(swapChainManager)
    , oitManager_(oitManager)
    , lightingManager_(lightingManager)
    , environmentManager_(environmentManager)
    , sceneController_(sceneController)
    , gpuCullManager_(gpuCullManager)
    , bloomManager_(bloomManager)
    , camera_(camera)
    , guiManager_(guiManager) {
  buildGraph();
}
// ---------------------------------------------------------------------------
// Graph construction
// ---------------------------------------------------------------------------

void FrameRecorder::buildGraph() {
  graph_.clear();

  graph_.addPass("FrustumCull", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (gpuCullManager_ && gpuCullManager_->isReady() &&
        p.opaqueSingleSidedDrawCommands &&
        !p.opaqueSingleSidedDrawCommands->empty()) {
      gpuCullManager_->ensureBufferCapacity(
          static_cast<uint32_t>(p.opaqueSingleSidedDrawCommands->size()));
      if (p.objectBuffer != VK_NULL_HANDLE && p.objectBufferSize > 0)
        gpuCullManager_->updateObjectSsboDescriptor(p.objectBuffer, p.objectBufferSize);
      gpuCullManager_->uploadDrawCommands(*p.opaqueSingleSidedDrawCommands);

      // Handle freeze-culling: snapshot camera on first frozen frame.
      if (p.debugFreezeCulling && !gpuCullManager_->cullingFrozen())
        gpuCullManager_->freezeCulling(cmd, p.cameraBuffer, p.cameraBufferSize);
      else if (!p.debugFreezeCulling && gpuCullManager_->cullingFrozen())
        gpuCullManager_->unfreezeCulling();

      gpuCullManager_->dispatchFrustumCull(cmd, p.cameraBuffer, p.cameraBufferSize,
                                static_cast<uint32_t>(p.opaqueSingleSidedDrawCommands->size()));
    }
  });

  graph_.addPass("DepthPrepass", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    const auto ws = guiManager_ ? guiManager_->wireframeSettings()
                                : container::ui::WireframeSettings{};
    const bool wfEnabled =
        guiManager_ && guiManager_->wireframeSupported() && ws.enabled &&
        p.pipelines.wireframeDepth != VK_NULL_HANDLE &&
        p.pipelines.wireframeNoDepth != VK_NULL_HANDLE;
    const bool wfFull = wfEnabled && ws.mode == container::ui::WireframeMode::Full;
    if (!wfFull || ws.depthTest) {
      recordDepthPrepass(cmd, p, p.sceneDescriptorSet);
    }
  });

  graph_.addPass("HiZGenerate", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!gpuCullManager_ || !gpuCullManager_->isReady() ||
        !p.frame || p.frame->depthSamplingView == VK_NULL_HANDLE ||
        p.gBufferSampler == VK_NULL_HANDLE) return;

    const auto extent = swapChainManager_.extent();
    gpuCullManager_->ensureHiZImage(extent.width, extent.height);

    // Transition depth from attachment to shader-readable for Hi-Z sampling.
    VkImageMemoryBarrier depthBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthBarrier.oldLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.newLayout     = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.image         = p.frame->depthStencil.image;
    depthBarrier.subresourceRange = {
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &depthBarrier);

    gpuCullManager_->dispatchHiZGenerate(cmd, p.frame->depthSamplingView,
                                         p.gBufferSampler,
                                         extent.width, extent.height);

    // Transition depth back to attachment for G-Buffer pass.
    depthBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.oldLayout     = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &depthBarrier);
  });

  graph_.addPass("OcclusionCull", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (gpuCullManager_ && gpuCullManager_->isReady() &&
        p.opaqueSingleSidedDrawCommands &&
        !p.opaqueSingleSidedDrawCommands->empty()) {
      gpuCullManager_->dispatchOcclusionCull(cmd, p.cameraBuffer, p.cameraBufferSize,
                                             static_cast<uint32_t>(p.opaqueSingleSidedDrawCommands->size()));
    }
  });

  graph_.addPass("CullStatsReadback", [this](VkCommandBuffer cmd, const FrameRecordParams&) {
    if (gpuCullManager_ && gpuCullManager_->isReady())
      gpuCullManager_->scheduleStatsReadback(cmd);
  });

  graph_.addPass("GBuffer", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    const auto ws = guiManager_ ? guiManager_->wireframeSettings()
                                : container::ui::WireframeSettings{};
    const bool wfEnabled =
        guiManager_ && guiManager_->wireframeSupported() && ws.enabled &&
        p.pipelines.wireframeDepth != VK_NULL_HANDLE &&
        p.pipelines.wireframeNoDepth != VK_NULL_HANDLE;
    const bool wfFull = wfEnabled && ws.mode == container::ui::WireframeMode::Full;
    if (!wfFull) {
      recordGBufferPass(cmd, p, p.sceneDescriptorSet);
    }
  });

  graph_.addPass("OitClear", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    oitManager_.clearResources(cmd, *p.frame, std::numeric_limits<uint32_t>::max());
  });

  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    graph_.addPass("ShadowCascade" + std::to_string(i),
        [this, i](VkCommandBuffer cmd, const FrameRecordParams& p) {
      recordShadowPass(cmd, p, i);
    });
  }

  // Transition depth from attachment-writable to read-only for the compute
  // passes (TileCull, GTAO) that sample depth.  The lighting render pass
  // initialLayout also expects DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL.
  graph_.addPass("DepthToReadOnly", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!p.frame || p.frame->depthStencil.image == VK_NULL_HANDLE) return;

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.image               = p.frame->depthStencil.image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  });

  graph_.addPass("TileCull", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (lightingManager_ && lightingManager_->isTiledLightingReady() &&
        p.frame && p.frame->depthSamplingView != VK_NULL_HANDLE) {
      lightingManager_->resetGpuTimers(cmd, p.imageIndex);
      lightingManager_->beginClusterCullTimer(cmd);
      lightingManager_->dispatchTileCull(
          cmd, swapChainManager_.extent(),
          p.cameraBuffer, p.cameraBufferSize,
          p.frame->depthSamplingView, p.gBufferSampler,
          p.cameraNear, p.cameraFar);
      lightingManager_->endClusterCullTimer(cmd);
    }
  });

  graph_.addPass("GTAO", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (environmentManager_ && environmentManager_->isGtaoReady() &&
        p.frame && p.frame->depthSamplingView != VK_NULL_HANDLE) {
      environmentManager_->dispatchGtao(
          cmd, swapChainManager_.extent().width, swapChainManager_.extent().height,
          p.cameraBuffer, p.cameraBufferSize,
          p.frame->depthSamplingView, p.gBufferSampler,
          p.frame->normal.view, p.gBufferSampler);
      environmentManager_->dispatchGtaoBlur(cmd);
    }
  });

  graph_.addPass("Lighting", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    const std::array<VkDescriptorSet, 2> lightingSets = {
        p.frame->lightingDescriptorSet, p.lightDescriptorSet};
    const std::array<VkDescriptorSet, 3> transparentSets = {
        p.sceneDescriptorSet, p.lightDescriptorSet, p.frame->oitDescriptorSet};
    recordLightingPass(cmd, p, p.sceneDescriptorSet, lightingSets, transparentSets);
  });

  graph_.addPass("OitResolve", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    oitManager_.prepareResolve(cmd, *p.frame);
  });

  graph_.addPass("Bloom", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!bloomManager_ || !bloomManager_->isReady()) return;
    if (!p.frame || p.frame->sceneColor.view == VK_NULL_HANDLE) return;

    // Barrier: scene color attachment write → compute shader read.
    VkImageMemoryBarrier sceneBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    sceneBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sceneBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sceneBarrier.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.image         = p.frame->sceneColor.image;
    sceneBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    sceneBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sceneBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &sceneBarrier);

    const auto extent = swapChainManager_.extent();
    bloomManager_->dispatch(cmd, p.frame->sceneColor.view, extent.width, extent.height);
  });

  graph_.addPass("PostProcess", [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    const std::array<VkDescriptorSet, 2> ppSets = {
        p.frame->postProcessDescriptorSet, p.frame->oitDescriptorSet};
    recordPostProcessPass(cmd, p, ppSets);
  });
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

void FrameRecorder::record(VkCommandBuffer commandBuffer,
                           const FrameRecordParams& p) const {
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    throw std::runtime_error("failed to begin recording command buffer!");
  }

  if (!p.frame) {
    throw std::runtime_error("FrameRecordParams::frame is null");
  }

  graph_.execute(commandBuffer, p);

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to record command buffer!");
  }
}
// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void FrameRecorder::setViewportAndScissor(VkCommandBuffer cmd) const {
  VkViewport viewport{};
  viewport.x        = 0.0f;
  viewport.y        = static_cast<float>(swapChainManager_.extent().height);
  viewport.width    = static_cast<float>(swapChainManager_.extent().width);
  viewport.height   = -static_cast<float>(swapChainManager_.extent().height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = swapChainManager_.extent();
  vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void FrameRecorder::bindSceneGeometryBuffers(
    VkCommandBuffer cmd, container::gpu::BufferSlice vertex,
    container::gpu::BufferSlice index, VkIndexType indexType) const {
  if (vertex.buffer == VK_NULL_HANDLE || index.buffer == VK_NULL_HANDLE) return;
  VkBuffer     vb[]  = {vertex.buffer};
  VkDeviceSize off[] = {vertex.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, vb, off);
  vkCmdBindIndexBuffer(cmd, index.buffer, index.offset, indexType);
}

void FrameRecorder::drawDiagnosticCube(VkCommandBuffer cmd, VkPipelineLayout layout,
                                        uint32_t diagCubeObjectIndex,
                                        BindlessPushConstants& pc) const {
  if (!sceneController_) return;
  if (diagCubeObjectIndex == std::numeric_limits<uint32_t>::max() ||
      sceneController_->diagCubeVertexSlice().buffer == VK_NULL_HANDLE) return;
  const auto diagVtx = sceneController_->diagCubeVertexSlice();
  const auto diagIdx = sceneController_->diagCubeIndexSlice();
  VkBuffer     diagVB[]  = {diagVtx.buffer};
  VkDeviceSize diagOff[] = {diagVtx.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, diagVB, diagOff);
  vkCmdBindIndexBuffer(cmd, diagIdx.buffer, diagIdx.offset, VK_INDEX_TYPE_UINT32);
  pc.objectIndex = diagCubeObjectIndex;
  vkCmdPushConstants(cmd, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(BindlessPushConstants), &pc);
  vkCmdDrawIndexed(cmd, sceneController_->diagCubeIndexCount(), 1, 0, 0, diagCubeObjectIndex);
}

void FrameRecorder::recordDepthPrepass(VkCommandBuffer cmd, const FrameRecordParams& p,
                                        VkDescriptorSet sceneSet) const {
  VkRenderPassBeginInfo info{};
  info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass        = p.renderPasses.depthPrepass;
  info.framebuffer       = p.frame->depthPrepassFramebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = swapChainManager_.extent();
  VkClearValue clearVal{};
  clearVal.depthStencil  = {0.0f, 0};
  info.clearValueCount   = 1;
  info.pClearValues      = &clearVal;

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.layouts.scene, 0, 1, &sceneSet, 0, nullptr);
  setViewportAndScissor(cmd);
  bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
  const bool frustumCullEnabled = graph_.findPass("FrustumCull") != nullptr &&
                                  graph_.findPass("FrustumCull")->enabled;

  const VkPipeline depthNoCullPipeline =
      p.pipelines.depthPrepassNoCull != VK_NULL_HANDLE
          ? p.pipelines.depthPrepassNoCull
          : p.pipelines.depthPrepass;

  if (gpuCullManager_ && gpuCullManager_->isReady() && frustumCullEnabled &&
      p.opaqueSingleSidedDrawCommands &&
      !p.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipelines.depthPrepass);
    gpuCullManager_->drawIndirect(cmd);
  } else if (p.opaqueSingleSidedDrawCommands &&
             !p.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipelines.depthPrepass);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueSingleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  if (p.opaqueDoubleSidedDrawCommands &&
      !p.opaqueDoubleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depthNoCullPipeline);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueDoubleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.depthPrepass);
  drawDiagnosticCube(cmd, p.layouts.scene, p.diagCubeObjectIndex,
                     *p.pushConstants.bindless);
  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordGBufferPass(VkCommandBuffer cmd, const FrameRecordParams& p,
                                       VkDescriptorSet sceneSet) const {
  VkRenderPassBeginInfo info{};
  info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass        = p.renderPasses.gBuffer;
  info.framebuffer       = p.frame->gBufferFramebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = swapChainManager_.extent();
  std::array<VkClearValue, 5> clearValues{};
  clearValues[0].color        = {{0.0f, 0.0f, 0.0f, 1.0f}};
  clearValues[1].color        = {{0.5f, 0.5f, 1.0f, 1.0f}};
  clearValues[2].color        = {{0.0f, 1.0f, 1.0f, 1.0f}};
  clearValues[3].color        = {{0.0f, 0.0f, 0.0f, 0.0f}};
  clearValues[4].depthStencil = {0.0f, 0};
  info.clearValueCount        = static_cast<uint32_t>(clearValues.size());
  info.pClearValues           = clearValues.data();

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.layouts.scene, 0, 1, &sceneSet, 0, nullptr);
  setViewportAndScissor(cmd);
  bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
  const bool occlusionCullEnabled = graph_.findPass("OcclusionCull") != nullptr &&
                                    graph_.findPass("OcclusionCull")->enabled;

  const VkPipeline gBufferNoCullPipeline =
      p.pipelines.gBufferNoCull != VK_NULL_HANDLE
          ? p.pipelines.gBufferNoCull
          : p.pipelines.gBuffer;

  if (gpuCullManager_ && gpuCullManager_->isReady() && occlusionCullEnabled &&
      p.opaqueSingleSidedDrawCommands &&
      !p.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.gBuffer);
    gpuCullManager_->drawIndirectOccluded(cmd);
  } else if (p.opaqueSingleSidedDrawCommands &&
             !p.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.gBuffer);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueSingleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  if (p.opaqueDoubleSidedDrawCommands &&
      !p.opaqueDoubleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gBufferNoCullPipeline);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueDoubleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.gBuffer);
  drawDiagnosticCube(cmd, p.layouts.scene, p.diagCubeObjectIndex,
                     *p.pushConstants.bindless);
  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordShadowPass(VkCommandBuffer cmd,
                                      const FrameRecordParams& p,
                                      uint32_t cascadeIndex) const {
  if (p.pipelines.shadowDepth == VK_NULL_HANDLE ||
      p.shadowFramebuffers == nullptr ||
      p.shadowFramebuffers[cascadeIndex] == VK_NULL_HANDLE)
    return;

  VkRenderPassBeginInfo info{};
  info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass        = p.renderPasses.shadow;
  info.framebuffer       = p.shadowFramebuffers[cascadeIndex];
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = {kShadowMapResolution, kShadowMapResolution};
  VkClearValue clearVal{};
  clearVal.depthStencil  = {0.0f, 0};
  info.clearValueCount   = 1;
  info.pClearValues      = &clearVal;

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{};
  viewport.width    = static_cast<float>(kShadowMapResolution);
  viewport.height   = static_cast<float>(kShadowMapResolution);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.extent = {kShadowMapResolution, kShadowMapResolution};
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  std::array<VkDescriptorSet, 2> shadowSets = {
      p.sceneDescriptorSet, p.shadowDescriptorSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layouts.shadow, 0,
                          static_cast<uint32_t>(shadowSets.size()),
                          shadowSets.data(), 0, nullptr);

  bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);

  ShadowPushConstants spc{};
  spc.cascadeIndex = cascadeIndex;

  const auto drawShadowList = [&](const std::vector<DrawCommand>* commands) {
    if (commands == nullptr) return;
    for (const auto& dc : *commands) {
      spc.objectIndex = dc.objectIndex;
      vkCmdPushConstants(cmd, p.layouts.shadow,
                         VK_SHADER_STAGE_VERTEX_BIT,
                         0, sizeof(ShadowPushConstants), &spc);
      vkCmdDrawIndexed(cmd, dc.indexCount, 1, dc.firstIndex, 0, dc.objectIndex);
    }
  };

  const VkPipeline shadowNoCullPipeline =
      p.pipelines.shadowDepthNoCull != VK_NULL_HANDLE
          ? p.pipelines.shadowDepthNoCull
          : p.pipelines.shadowDepth;

  const bool frustumCullEnabled = graph_.findPass("FrustumCull") != nullptr &&
                                  graph_.findPass("FrustumCull")->enabled;
  if (gpuCullManager_ && gpuCullManager_->isReady() && frustumCullEnabled &&
      p.opaqueSingleSidedDrawCommands &&
      !p.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.shadowDepth);
    spc.objectIndex = 0;
    vkCmdPushConstants(cmd, p.layouts.shadow,
                       VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(ShadowPushConstants), &spc);
    gpuCullManager_->drawIndirect(cmd);
  } else if (p.opaqueSingleSidedDrawCommands &&
             !p.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.shadowDepth);
    drawShadowList(p.opaqueSingleSidedDrawCommands);
  }

  if (p.opaqueDoubleSidedDrawCommands &&
      !p.opaqueDoubleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowNoCullPipeline);
    drawShadowList(p.opaqueDoubleSidedDrawCommands);
  }

  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordLightingPass(
    VkCommandBuffer cmd, const FrameRecordParams& p, VkDescriptorSet sceneSet,
    const std::array<VkDescriptorSet, 2>& lightingDescriptorSets,
    const std::array<VkDescriptorSet, 3>& transparentDescriptorSets) const {
  using container::ui::GBufferViewMode;
  using container::ui::WireframeMode;

  const GBufferViewMode displayMode =
      guiManager_ ? guiManager_->gBufferViewMode() : GBufferViewMode::Overview;
  const bool showObjectSpaceNormals = displayMode == GBufferViewMode::ObjectSpaceNormals;
  const auto wireframeSettings =
      guiManager_ ? guiManager_->wireframeSettings() : container::ui::WireframeSettings{};
  const bool wireframeEnabled =
      guiManager_ && guiManager_->wireframeSupported() && wireframeSettings.enabled &&
      p.pipelines.wireframeDepth != VK_NULL_HANDLE &&
      p.pipelines.wireframeNoDepth != VK_NULL_HANDLE;
  const bool wireframeFullMode =
      wireframeEnabled && wireframeSettings.mode == WireframeMode::Full;
  const bool wireframeOverlayMode =
      wireframeEnabled && wireframeSettings.mode == WireframeMode::Overlay;
  const VkPipeline activeWireframePipeline =
      wireframeSettings.depthTest ? p.pipelines.wireframeDepth : p.pipelines.wireframeNoDepth;
  const bool showNormalValidation =
      guiManager_ && guiManager_->showNormalValidation() &&
      p.pipelines.normalValidation != VK_NULL_HANDLE;

  VkRenderPassBeginInfo lightingPassInfo{};
  lightingPassInfo.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  lightingPassInfo.renderPass  = p.renderPasses.lighting;
  lightingPassInfo.framebuffer = p.frame->lightingFramebuffer;
  lightingPassInfo.renderArea.offset = {0, 0};
  lightingPassInfo.renderArea.extent = swapChainManager_.extent();
  std::array<VkClearValue, 2> lightingClearValues{};
  lightingClearValues[0].color        = {{0.0f, 0.0f, 0.0f, 1.0f}};
  lightingClearValues[1].depthStencil = {0.0f, 0};
  lightingPassInfo.clearValueCount = static_cast<uint32_t>(lightingClearValues.size());
  lightingPassInfo.pClearValues    = lightingClearValues.data();

  vkCmdBeginRenderPass(cmd, &lightingPassInfo, VK_SUBPASS_CONTENTS_INLINE);
  setViewportAndScissor(cmd);

  if (wireframeFullMode) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeWireframePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.layouts.wireframe, 0, 1, &p.sceneDescriptorSet, 0, nullptr);
    if (p.wireframeRasterModeSupported) {
      const float lw = p.wireframeWideLinesSupported ? wireframeSettings.lineWidth : 1.0f;
      vkCmdSetLineWidth(cmd, lw);
    }
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    debugOverlay_.drawWireframe(cmd, p.layouts.wireframe, *p.opaqueDrawCommands,
                                wireframeSettings.color, wireframeSettings.overlayIntensity,
                                wireframeSettings.lineWidth, *p.pushConstants.wireframe);
    debugOverlay_.drawWireframe(cmd, p.layouts.wireframe, *p.transparentDrawCommands,
                                wireframeSettings.color, wireframeSettings.overlayIntensity,
                                wireframeSettings.lineWidth, *p.pushConstants.wireframe);
    drawDiagnosticCube(cmd, p.layouts.wireframe, p.diagCubeObjectIndex,
                       *p.pushConstants.bindless);
  } else if (showObjectSpaceNormals &&
             p.pipelines.objectNormalDebug != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.layouts.scene, 0, 1, &p.sceneDescriptorSet, 0,
                            nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    const VkPipeline objectNormalNoCullPipeline =
        p.pipelines.objectNormalDebugNoCull != VK_NULL_HANDLE
            ? p.pipelines.objectNormalDebugNoCull
            : p.pipelines.objectNormalDebug;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipelines.objectNormalDebug);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueSingleSidedDrawCommands,
                            *p.pushConstants.bindless);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.transparentSingleSidedDrawCommands,
                            *p.pushConstants.bindless);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      objectNormalNoCullPipeline);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueDoubleSidedDrawCommands,
                            *p.pushConstants.bindless);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.transparentDoubleSidedDrawCommands,
                            *p.pushConstants.bindless);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipelines.objectNormalDebug);
    drawDiagnosticCube(cmd, p.layouts.scene, p.diagCubeObjectIndex,
                       *p.pushConstants.bindless);
  } else {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.directionalLight);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layouts.lighting, 0,
                            static_cast<uint32_t>(lightingDescriptorSets.size()),
                            lightingDescriptorSets.data(), 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }

  VkClearAttachment stencilClearAttachment{};
  stencilClearAttachment.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
  stencilClearAttachment.clearValue.depthStencil = {0.0f, 0};
  VkClearRect stencilClearRect{};
  stencilClearRect.rect.offset    = {0, 0};
  stencilClearRect.rect.extent    = swapChainManager_.extent();
  stencilClearRect.baseArrayLayer = 0;
  stencilClearRect.layerCount     = 1;

  if (!wireframeFullMode && !showObjectSpaceNormals && !p.debugDirectionalOnly) {
    const bool tileCullEnabled = graph_.findPass("TileCull") != nullptr &&
                                 graph_.findPass("TileCull")->enabled;
    const bool useTiled =
        tileCullEnabled &&
        lightingManager_ && lightingManager_->isTiledLightingReady() &&
        p.frame && p.frame->depthSamplingView != VK_NULL_HANDLE &&
        p.pipelines.tiledPointLight != VK_NULL_HANDLE &&
        p.tiledDescriptorSet != VK_NULL_HANDLE;

    if (useTiled) {
      // Tiled point light accumulation — single fullscreen triangle.
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        p.pipelines.tiledPointLight);
      const std::array<VkDescriptorSet, 2> tiledSets = {
          p.frame->lightingDescriptorSet, p.tiledDescriptorSet};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              p.layouts.tiledLighting, 0,
                              static_cast<uint32_t>(tiledSets.size()),
                              tiledSets.data(), 0, nullptr);
      const uint32_t tileCountX =
          (swapChainManager_.extent().width + kTileSize - 1) / kTileSize;
      TiledLightingPushConstants tlpc{};
      tlpc.tileCountX = tileCountX;
      tlpc.depthSliceCount = container::gpu::kClusterDepthSlices;
      tlpc.cameraNear = p.cameraNear;
      tlpc.cameraFar = p.cameraFar;
      vkCmdPushConstants(cmd, p.layouts.tiledLighting,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(TiledLightingPushConstants), &tlpc);
      lightingManager_->beginClusteredLightingTimer(cmd);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      lightingManager_->endClusteredLightingTimer(cmd);
    } else {
      // Fallback: per-light stencil loop.
      const VkPipeline activePointPipeline =
          p.debugVisualizePointLightStencil ? p.pipelines.pointLightStencilDebug
                                           : p.pipelines.pointLight;
      const auto& lightingData =
          lightingManager_ ? lightingManager_->lightingData() : LightingData{};
      const uint32_t numLights = std::min(lightingData.pointLightCount, kMaxDeferredPointLights);

      for (uint32_t i = 0; i < numLights; ++i) {
        vkCmdClearAttachments(cmd, 1, &stencilClearAttachment, 1, &stencilClearRect);
        p.pushConstants.light->positionRadius = lightingData.pointLights[i].positionRadius;
        p.pushConstants.light->colorIntensity = lightingData.pointLights[i].colorIntensity;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.stencilVolume);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layouts.lighting, 0,
                                static_cast<uint32_t>(lightingDescriptorSets.size()),
                                lightingDescriptorSets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, p.layouts.lighting,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(LightPushConstants), p.pushConstants.light);
        vkCmdDraw(cmd, lightingManager_ ? lightingManager_->lightVolumeIndexCount() : 0, 1, 0, 0);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePointPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layouts.lighting, 0,
                                static_cast<uint32_t>(lightingDescriptorSets.size()),
                                lightingDescriptorSets.data(), 0, nullptr);
        vkCmdPushConstants(cmd, p.layouts.lighting,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(LightPushConstants), p.pushConstants.light);
        vkCmdDraw(cmd, 3, 1, 0, 0);
      }
    }
  }

  if (!wireframeFullMode && !showObjectSpaceNormals &&
      ((p.transparentSingleSidedDrawCommands &&
        !p.transparentSingleSidedDrawCommands->empty()) ||
       (p.transparentDoubleSidedDrawCommands &&
        !p.transparentDoubleSidedDrawCommands->empty()))) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layouts.transparent, 0,
                            static_cast<uint32_t>(transparentDescriptorSets.size()),
                            transparentDescriptorSets.data(), 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    const VkPipeline transparentNoCullPipeline =
        p.pipelines.transparentNoCull != VK_NULL_HANDLE
            ? p.pipelines.transparentNoCull
            : p.pipelines.transparent;

    if (p.transparentSingleSidedDrawCommands &&
        !p.transparentSingleSidedDrawCommands->empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.transparent);
      debugOverlay_.drawScene(cmd, p.layouts.transparent,
                              *p.transparentSingleSidedDrawCommands,
                              *p.pushConstants.bindless);
    }
    if (p.transparentDoubleSidedDrawCommands &&
        !p.transparentDoubleSidedDrawCommands->empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, transparentNoCullPipeline);
      debugOverlay_.drawScene(cmd, p.layouts.transparent,
                              *p.transparentDoubleSidedDrawCommands,
                              *p.pushConstants.bindless);
    }
  }

  if (guiManager_ && guiManager_->showGeometryOverlay() &&
      p.pipelines.geometryDebug != VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.geometryDebug);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.layouts.scene, 0, 1, &p.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueDrawCommands,
                            *p.pushConstants.bindless);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.transparentDrawCommands,
                            *p.pushConstants.bindless);
  }

  if (showNormalValidation && p.pipelines.normalValidation != VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.normalValidation);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.layouts.normalValidation, 0, 1, &p.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    debugOverlay_.recordNormalValidation(cmd, p.layouts.normalValidation,
                                         *p.opaqueDrawCommands, *p.transparentDrawCommands,
                                         guiManager_->normalValidationSettings(),
                                         *p.pushConstants.normalValidation);
  }

  const bool showSurfaceNormalLines =
      (showNormalValidation && p.pipelines.surfaceNormalLine != VK_NULL_HANDLE) ||
      (guiManager_ && displayMode == GBufferViewMode::SurfaceNormals &&
       p.pipelines.surfaceNormalLine != VK_NULL_HANDLE);
  if (showSurfaceNormalLines) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.surfaceNormalLine);
    if (p.wireframeRasterModeSupported && guiManager_) {
      const auto& nv = guiManager_->normalValidationSettings();
      const float lw = p.wireframeWideLinesSupported ? nv.lineWidth : 1.0f;
      vkCmdSetLineWidth(cmd, lw);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.layouts.surfaceNormal, 0, 1, &p.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    debugOverlay_.recordSurfaceNormals(cmd, p.layouts.surfaceNormal,
                                       *p.opaqueDrawCommands, *p.transparentDrawCommands,
                                       guiManager_->normalValidationSettings(),
                                       *p.pushConstants.surfaceNormal);
  }

  if (wireframeOverlayMode && activeWireframePipeline != VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activeWireframePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.layouts.wireframe, 0, 1, &p.sceneDescriptorSet, 0, nullptr);
    if (p.wireframeRasterModeSupported) {
      const float lw = p.wireframeWideLinesSupported ? wireframeSettings.lineWidth : 1.0f;
      vkCmdSetLineWidth(cmd, lw);
    }
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    debugOverlay_.drawWireframe(cmd, p.layouts.wireframe, *p.opaqueDrawCommands,
                                wireframeSettings.color, wireframeSettings.overlayIntensity,
                                wireframeSettings.lineWidth, *p.pushConstants.wireframe);
    debugOverlay_.drawWireframe(cmd, p.layouts.wireframe, *p.transparentDrawCommands,
                                wireframeSettings.color, wireframeSettings.overlayIntensity,
                                wireframeSettings.lineWidth, *p.pushConstants.wireframe);
  }

  if (guiManager_ && guiManager_->showLightGizmos() &&
      p.pipelines.lightGizmo != VK_NULL_HANDLE && lightingManager_) {
    lightingManager_->drawLightGizmos(cmd, lightingDescriptorSets,
                                      p.pipelines.lightGizmo, p.layouts.lighting, camera_);
  }

  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordPostProcessPass(
    VkCommandBuffer cmd, const FrameRecordParams& p,
    const std::array<VkDescriptorSet, 2>& postProcessSets) const {
  using container::ui::GBufferViewMode;

  if (!p.swapChainFramebuffers || p.imageIndex >= p.swapChainFramebuffers->size()) {
    throw std::runtime_error("invalid swapChainFramebuffers in FrameRecordParams");
  }

  VkRenderPassBeginInfo info{};
  info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass        = p.renderPasses.postProcess;
  info.framebuffer       = (*p.swapChainFramebuffers)[p.imageIndex];
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = swapChainManager_.extent();
  VkClearValue clearVal{};
  clearVal.color       = {{0.0f, 0.0f, 0.0f, 1.0f}};
  info.clearValueCount = 1;
  info.pClearValues    = &clearVal;

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.postProcess);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layouts.postProcess, 0,
                          static_cast<uint32_t>(postProcessSets.size()),
                          postProcessSets.data(), 0, nullptr);
  setViewportAndScissor(cmd);

  const GBufferViewMode displayMode =
      guiManager_ ? guiManager_->gBufferViewMode() : GBufferViewMode::Overview;
  PostProcessPushConstants ppPc{};
  ppPc.outputMode = static_cast<uint32_t>(displayMode);
  const auto isPassEnabled = [this](std::string_view name) {
    const auto* pass = graph_.findPass(std::string{name});
    return pass != nullptr && pass->enabled;
  };
  const bool bloomPassEnabled = isPassEnabled("Bloom");
  const bool gtaoEnabled = isPassEnabled("GTAO");
  const bool shadowEnabled = isPassEnabled("ShadowCascade0") ||
                             isPassEnabled("ShadowCascade1") ||
                             isPassEnabled("ShadowCascade2") ||
                             isPassEnabled("ShadowCascade3");
  const bool tileCullEnabled = isPassEnabled("TileCull") && lightingManager_ &&
                               lightingManager_->isTiledLightingReady();
  ppPc.bloomEnabled = (bloomManager_ && bloomManager_->isReady() &&
                       bloomManager_->enabled() && bloomPassEnabled)
                          ? 1u
                          : 0u;
  ppPc.bloomIntensity = bloomManager_ ? bloomManager_->intensity() : 0.0f;
  ppPc.cameraNear = p.cameraNear;
  ppPc.cameraFar  = p.cameraFar;
  if (shadowEnabled && p.shadowData) {
    for (uint32_t i = 0; i < kShadowCascadeCount; ++i)
      ppPc.cascadeSplits[i] = p.shadowData->cascades[i].splitDepth;
  }
  if (tileCullEnabled) {
    const auto extent = swapChainManager_.extent();
    ppPc.tileCountX  = (extent.width + container::gpu::kTileSize - 1) / container::gpu::kTileSize;
    ppPc.totalLights = static_cast<uint32_t>(lightingManager_->pointLightsSsbo().size());
    ppPc.depthSliceCount = container::gpu::kClusterDepthSlices;
  } else {
    ppPc.tileCountX = 1u;
    ppPc.totalLights = 0u;
    ppPc.depthSliceCount = 1u;
  }
  if (!gtaoEnabled && displayMode == GBufferViewMode::TileLightHeatMap) {
    ppPc.totalLights = 0u;
  }
  vkCmdPushConstants(cmd, p.layouts.postProcess, VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(PostProcessPushConstants), &ppPc);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  if (guiManager_) guiManager_->render(cmd);

  vkCmdEndRenderPass(cmd);
}

}  // namespace container::renderer
