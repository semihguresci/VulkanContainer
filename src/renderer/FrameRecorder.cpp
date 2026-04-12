#include "Container/renderer/FrameRecorder.h"
#include "Container/renderer/LightingManager.h"
#include "Container/renderer/OitManager.h"
#include "Container/renderer/SceneController.h"
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
using container::gpu::LightingData;
using container::gpu::PostProcessPushConstants;

FrameRecorder::FrameRecorder(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::SwapChainManager& swapChainManager,
    const OitManager& oitManager,
    const LightingManager* lightingManager,
    const SceneController* sceneController,
    const container::scene::BaseCamera* camera,
    container::ui::GuiManager* guiManager)
    : device_(std::move(device))
    , swapChainManager_(swapChainManager)
    , oitManager_(oitManager)
    , lightingManager_(lightingManager)
    , sceneController_(sceneController)
    , camera_(camera)
    , guiManager_(guiManager) {
  buildGraph();
}
// ---------------------------------------------------------------------------
// Graph construction
// ---------------------------------------------------------------------------

void FrameRecorder::buildGraph() {
  graph_.clear();

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
  viewport.y        = 0.0f;
  viewport.width    = static_cast<float>(swapChainManager_.extent().width);
  viewport.height   = static_cast<float>(swapChainManager_.extent().height);
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
  vkCmdDrawIndexed(cmd, sceneController_->diagCubeIndexCount(), 1, 0, 0, 0);
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
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.depthPrepass);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.layouts.scene, 0, 1, &sceneSet, 0, nullptr);
  setViewportAndScissor(cmd);
  bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
  debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueDrawCommands,
                          *p.pushConstants.bindless);
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
  std::array<VkClearValue, 6> clearValues{};
  clearValues[0].color        = {{0.0f, 0.0f, 0.0f, 1.0f}};
  clearValues[1].color        = {{0.5f, 0.5f, 1.0f, 1.0f}};
  clearValues[2].color        = {{0.0f, 1.0f, 1.0f, 1.0f}};
  clearValues[3].color        = {{0.0f, 0.0f, 0.0f, 0.0f}};
  clearValues[4].color        = {{0.0f, 0.0f, 0.0f, 1.0f}};
  clearValues[5].depthStencil = {0.0f, 0};
  info.clearValueCount        = static_cast<uint32_t>(clearValues.size());
  info.pClearValues           = clearValues.data();

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.gBuffer);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.layouts.scene, 0, 1, &sceneSet, 0, nullptr);
  setViewportAndScissor(cmd);
  bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
  debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueDrawCommands,
                          *p.pushConstants.bindless);
  drawDiagnosticCube(cmd, p.layouts.scene, p.diagCubeObjectIndex,
                     *p.pushConstants.bindless);
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

  if (!wireframeFullMode && !showObjectSpaceNormals && !p.transparentDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.transparent);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layouts.transparent, 0,
                            static_cast<uint32_t>(transparentDescriptorSets.size()),
                            transparentDescriptorSets.data(), 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    debugOverlay_.drawScene(cmd, p.layouts.transparent,
                            *p.transparentDrawCommands, *p.pushConstants.bindless);
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
  vkCmdPushConstants(cmd, p.layouts.postProcess, VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(PostProcessPushConstants), &ppPc);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  if (guiManager_) guiManager_->render(cmd);

  vkCmdEndRenderPass(cmd);
}

}  // namespace container::renderer