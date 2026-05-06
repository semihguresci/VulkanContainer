#include "Container/renderer/picking/TransparentPickRasterPassRecorder.h"

#include "Container/renderer/picking/TransparentPickDepthCopyRecorder.h"
#include "Container/renderer/scene/SceneViewport.h"

#include <array>
#include <span>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasTransparentPickRasterPassShell(
    VkCommandBuffer cmd, const TransparentPickRasterPassRecordInputs &inputs) {
  return inputs.active && cmd != VK_NULL_HANDLE &&
         inputs.renderPass != VK_NULL_HANDLE &&
         inputs.framebuffer != VK_NULL_HANDLE && inputs.extent.width > 0u &&
         inputs.extent.height > 0u;
}

[[nodiscard]] bool hasTransparentPickGeometry(
    const TransparentPickPassGeometryBinding &geometry) {
  return geometry.descriptorSet != VK_NULL_HANDLE &&
         geometry.vertexSlice.buffer != VK_NULL_HANDLE &&
         geometry.indexSlice.buffer != VK_NULL_HANDLE;
}

[[nodiscard]] BimSurfacePassGeometryBinding bimSurfacePickGeometry(
    std::span<const VkDescriptorSet> descriptorSets,
    const TransparentPickPassGeometryBinding &geometry) {
  return {.descriptorSets = descriptorSets,
          .vertexSlice = geometry.vertexSlice,
          .indexSlice = geometry.indexSlice,
          .indexType = geometry.indexType};
}

} // namespace

bool recordTransparentPickRasterPassCommands(
    VkCommandBuffer cmd, const TransparentPickRasterPassRecordInputs &inputs) {
  if (!hasTransparentPickRasterPassShell(cmd, inputs)) {
    return false;
  }

  VkRenderPassBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = inputs.renderPass;
  info.framebuffer = inputs.framebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = inputs.extent;

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  recordSceneViewportAndScissor(cmd, inputs.extent);
  static_cast<void>(recordTransparentPickPassCommands(cmd, inputs.pass));
  vkCmdEndRenderPass(cmd);
  return true;
}

bool recordTransparentPickFramePassCommands(
    VkCommandBuffer cmd, const TransparentPickFramePassRecordInputs &inputs) {
  if (cmd == VK_NULL_HANDLE || inputs.extent.width == 0u ||
      inputs.extent.height == 0u || inputs.renderPass == VK_NULL_HANDLE ||
      inputs.framebuffer == VK_NULL_HANDLE ||
      inputs.sourceDepthStencilImage == VK_NULL_HANDLE ||
      inputs.pickDepthImage == VK_NULL_HANDLE ||
      inputs.pickIdImage == VK_NULL_HANDLE ||
      inputs.pipelineLayout == VK_NULL_HANDLE ||
      inputs.pipelines.primary == VK_NULL_HANDLE ||
      inputs.pushConstants == nullptr || inputs.debugOverlay == nullptr) {
    return false;
  }

  const bool sceneRecordReady =
      inputs.scenePassReady && hasTransparentPickGeometry(inputs.scene);
  const SceneTransparentDrawPlan transparentPlan =
      sceneRecordReady ? buildSceneTransparentDrawPlan(inputs.sceneDraws)
                       : SceneTransparentDrawPlan{};

  const std::array<VkDescriptorSet, 1> bimDescriptorSets = {
      inputs.bim.descriptorSet};
  const BimSurfacePassPlan bimTransparentPickPlan =
      buildBimSurfaceFramePassPlan(
          {.kind = BimSurfacePassKind::TransparentPick,
           .passReady =
               inputs.bimPassReady && hasTransparentPickGeometry(inputs.bim),
           .draws = inputs.bimDraws,
           .geometry =
               bimSurfacePickGeometry(bimDescriptorSets, inputs.bim),
           .pipelines = {.singleSided = inputs.pipelines.primary},
           .pushConstants = inputs.pushConstants,
           .semanticColorMode = inputs.bimSemanticColorMode});

  if (transparentPlan.routeCount == 0u && !bimTransparentPickPlan.active) {
    return false;
  }

  const TransparentPickDepthCopyPlan depthCopyPlan =
      buildTransparentPickDepthCopyPlan(
          {.sourceDepthStencilImage = inputs.sourceDepthStencilImage,
           .pickDepthImage = inputs.pickDepthImage,
           .extent = inputs.extent});
  if (!recordTransparentPickDepthCopyCommands(cmd, depthCopyPlan)) {
    return false;
  }

  return recordTransparentPickRasterPassCommands(
      cmd, {.active = true,
            .renderPass = inputs.renderPass,
            .framebuffer = inputs.framebuffer,
            .extent = inputs.extent,
            .pass = {.scenePlan = &transparentPlan,
                     .bimPlan = &bimTransparentPickPlan,
                     .scene = inputs.scene,
                     .bim = inputs.bim,
                     .pipelines = inputs.pipelines,
                     .pipelineLayout = inputs.pipelineLayout,
                     .pushConstants = *inputs.pushConstants,
                     .debugOverlay = inputs.debugOverlay,
                     .bimManager = inputs.bimManager}});
}

} // namespace container::renderer
