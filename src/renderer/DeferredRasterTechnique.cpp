#include "Container/renderer/DeferredRasterTechnique.h"

#include "Container/renderer/BloomManager.h"
#include "Container/renderer/DeferredRasterFrameState.h"
#include "Container/renderer/EnvironmentManager.h"
#include "Container/renderer/ExposureManager.h"
#include "Container/renderer/FrameRecorder.h"
#include "Container/renderer/GpuCullManager.h"
#include "Container/renderer/LightingManager.h"
#include "Container/renderer/OitManager.h"
#include "Container/renderer/ShadowCullManager.h"
#include "Container/renderer/ShadowManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SwapChainManager.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <vector>

namespace container::renderer {

using container::gpu::kShadowCascadeCount;

std::string_view DeferredRasterTechnique::name() const {
  return renderTechniqueName(id());
}

std::string_view DeferredRasterTechnique::displayName() const {
  return renderTechniqueDisplayName(id());
}

RenderTechniqueAvailability DeferredRasterTechnique::availability(
    const RenderSystemContext& context) const {
  if (context.frameRecorder == nullptr) {
    return RenderTechniqueAvailability::unavailable(
        "deferred raster requires a frame recorder");
  }
  return RenderTechniqueAvailability::availableNow();
}

TechniqueDebugModel DeferredRasterTechnique::debugModel() const {
  TechniqueDebugModel model{};
  model.techniqueName = std::string(name());
  model.displayName = std::string(displayName());
  model.panels.push_back(TechniqueDebugPanel{
      .id = "deferred-frame",
      .title = "Deferred Frame",
      .controls =
          {
              TechniqueDebugControl{
                  .id = "render-graph",
                  .label = "Render graph",
                  .kind = TechniqueDebugControlKind::Action},
              TechniqueDebugControl{
                  .id = "gbuffer-view",
                  .label = "G-buffer view",
                  .kind = TechniqueDebugControlKind::Enum,
                  .options =
                      {
                          {.id = "lit", .label = "Lit"},
                          {.id = "overview", .label = "Overview"},
                          {.id = "transparency", .label = "Transparency"},
                      }},
              TechniqueDebugControl{
                  .id = "wireframe-overlay",
                  .label = "Wireframe overlay",
                  .kind = TechniqueDebugControlKind::Toggle},
          }});
  return model;
}

void DeferredRasterTechnique::buildFrameGraph(RenderSystemContext& context) {
  if (context.frameRecorder == nullptr) {
    return;
  }

  FrameRecorder& recorder = *context.frameRecorder;
  RenderGraphBuilder graph = recorder.graphBuilder();
  graph.clear();

  // CPU registration order is the frame schedule. Later passes rely on the
  // image layouts and indirect-count buffers produced by earlier nodes.
  graph.addPass(RenderPassId::FrustumCull, [&recorder](VkCommandBuffer cmd,
                                                   const FrameRecordParams &p) {
    if (recorder.gpuCullManager_ && recorder.gpuCullManager_->isReady() &&
        p.draws.opaqueSingleSidedDrawCommands &&
        !p.draws.opaqueSingleSidedDrawCommands->empty()) {
      recorder.gpuCullManager_->ensureBufferCapacity(
          static_cast<uint32_t>(p.draws.opaqueSingleSidedDrawCommands->size()));
      if (p.scene.objectBuffer != VK_NULL_HANDLE &&
          p.scene.objectBufferSize > 0)
        recorder.gpuCullManager_->updateObjectSsboDescriptor(p.scene.objectBuffer,
                                                    p.scene.objectBufferSize);
      recorder.gpuCullManager_->uploadDrawCommands(
          *p.draws.opaqueSingleSidedDrawCommands);

      // Handle freeze-culling: snapshot camera on first frozen frame.
      if (p.debug.debugFreezeCulling && !recorder.gpuCullManager_->cullingFrozen())
        recorder.gpuCullManager_->freezeCulling(cmd, p.camera.cameraBuffer,
                                       p.camera.cameraBufferSize);
      else if (!p.debug.debugFreezeCulling && recorder.gpuCullManager_->cullingFrozen())
        recorder.gpuCullManager_->unfreezeCulling();

      recorder.gpuCullManager_->dispatchFrustumCull(
          cmd, p.camera.cameraBuffer, p.camera.cameraBufferSize,
          static_cast<uint32_t>(p.draws.opaqueSingleSidedDrawCommands->size()));
    }
  });

  graph.addPass(RenderPassId::DepthPrepass,
                 [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
                   recorder.recordDepthPrepass(cmd, p, p.descriptors.sceneDescriptorSet);
                 });

  graph.addPass(RenderPassId::BimDepthPrepass,
                 [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
                   recorder.recordBimDepthPrepass(cmd, p);
                 });

  graph.addPass(RenderPassId::HiZGenerate, [&recorder](VkCommandBuffer cmd,
                                                   const FrameRecordParams &p) {
    if (!recorder.gpuCullManager_ || !recorder.gpuCullManager_->isReady() || !p.runtime.frame ||
        p.runtime.frame->depthSamplingView == VK_NULL_HANDLE ||
        p.camera.gBufferSampler == VK_NULL_HANDLE)
      return;

    const auto extent = recorder.swapChainManager_.extent();
    recorder.gpuCullManager_->ensureHiZImage(extent.width, extent.height);

    // Transition depth from attachment to shader-readable for Hi-Z sampling.
    VkImageMemoryBarrier depthBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.newLayout =
        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.image = p.runtime.frame->depthStencil.image;
    depthBarrier.subresourceRange = {
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &depthBarrier);

    recorder.gpuCullManager_->dispatchHiZGenerate(
        cmd, p.runtime.frame->depthSamplingView, p.camera.gBufferSampler,
        extent.width, extent.height);

    // Transition depth back to attachment for G-Buffer pass.
    depthBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.oldLayout =
        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &depthBarrier);
  });

  graph.addPass(RenderPassId::OcclusionCull,
                 [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
                   if (recorder.gpuCullManager_ && recorder.gpuCullManager_->isReady() &&
                       p.draws.opaqueSingleSidedDrawCommands &&
                       !p.draws.opaqueSingleSidedDrawCommands->empty()) {
                     recorder.gpuCullManager_->dispatchOcclusionCull(
                         cmd, p.camera.cameraBuffer, p.camera.cameraBufferSize,
                         static_cast<uint32_t>(
                             p.draws.opaqueSingleSidedDrawCommands->size()));
                   }
                 });

  graph.addPass(RenderPassId::CullStatsReadback,
                 [&recorder](VkCommandBuffer cmd, const FrameRecordParams &) {
                   if (recorder.gpuCullManager_ && recorder.gpuCullManager_->isReady())
                     recorder.gpuCullManager_->scheduleStatsReadback(cmd);
                 });

  graph.addPass(RenderPassId::GBuffer,
                 [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
                   recorder.recordGBufferPass(cmd, p, p.descriptors.sceneDescriptorSet);
                 });

  graph.addPass(RenderPassId::BimGBuffer,
                 [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
                   recorder.recordBimGBufferPass(cmd, p);
                 });

  graph.addPass(RenderPassId::TransparentPick,
                 [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
                   recorder.recordTransparentPickPass(cmd, p);
                 });

  graph.addPass(RenderPassId::OitClear, [&recorder](VkCommandBuffer cmd,
                                                const FrameRecordParams &p) {
    if (!shouldRecordTransparentOit(p, recorder.guiManager_))
      return;
    recorder.oitManager_.clearResources(cmd, *p.runtime.frame,
                               std::numeric_limits<uint32_t>::max());
  });

  const auto shadowCullIds = shadowCullPassIds();
  const auto shadowPassIds = shadowCascadePassIds();
  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    graph.addPass(shadowCullIds[i], [&recorder, i](VkCommandBuffer cmd,
                                               const FrameRecordParams &p) {
      if (!displayModeRecordsShadowAtlas(currentDisplayMode(recorder.guiManager_))) {
        return;
      }
      if (!p.shadows.useGpuShadowCull ||
          p.shadows.shadowCullManager == nullptr ||
          !p.shadows.shadowCullManager->isReady() ||
          p.draws.opaqueSingleSidedDrawCommands == nullptr) {
        return;
      }

      const uint32_t drawCount =
          static_cast<uint32_t>(p.draws.opaqueSingleSidedDrawCommands->size());
      if (drawCount == 0u)
        return;

      p.shadows.shadowCullManager->dispatchCascadeCull(
          cmd, p.runtime.imageIndex, i, drawCount);
    });

    graph.addPass(shadowPassIds[i],
                   [&recorder, i](VkCommandBuffer cmd, const FrameRecordParams &p) {
                     recorder.recordShadowPass(cmd, p, i);
                   });
  }

  // Transition depth from attachment-writable to read-only for the compute
  // passes (TileCull, GTAO) that sample depth.  The lighting render pass
  // initialLayout also expects DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL.
  graph.addPass(
      RenderPassId::DepthToReadOnly,
      [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
        if (!p.runtime.frame ||
            p.runtime.frame->depthStencil.image == VK_NULL_HANDLE)
          return;

        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.newLayout =
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
        barrier.image = p.runtime.frame->depthStencil.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT |
                                        VK_IMAGE_ASPECT_STENCIL_BIT,
                                    0, 1, 0, 1};
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        if (!displayModeRecordsShadowAtlas(currentDisplayMode(recorder.guiManager_)) &&
            p.shadows.shadowManager != nullptr &&
            p.shadows.shadowManager->shadowAtlasImage() != VK_NULL_HANDLE) {
          VkImageMemoryBarrier shadowBarrier{
              VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          shadowBarrier.srcAccessMask = 0;
          shadowBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
          shadowBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          shadowBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          shadowBarrier.image = p.shadows.shadowManager->shadowAtlasImage();
          shadowBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT |
                                                VK_IMAGE_ASPECT_STENCIL_BIT,
                                            0, 1, 0, kShadowCascadeCount};
          shadowBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          shadowBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                               nullptr, 0, nullptr, 1, &shadowBarrier);
        }
      });

  graph.addPass(RenderPassId::TileCull, [&recorder](VkCommandBuffer cmd,
                                                const FrameRecordParams &p) {
    if (!displayModeRecordsTileCull(currentDisplayMode(recorder.guiManager_)))
      return;
    if (recorder.lightingManager_ && recorder.lightingManager_->isTiledLightingReady() &&
        p.runtime.frame &&
        p.runtime.frame->depthSamplingView != VK_NULL_HANDLE) {
      recorder.lightingManager_->beginClusterCullTimer(cmd);
      recorder.lightingManager_->dispatchTileCull(
          cmd, recorder.swapChainManager_.extent(), p.camera.cameraBuffer,
          p.camera.cameraBufferSize, p.runtime.frame->depthSamplingView,
          p.camera.nearPlane, p.camera.farPlane);
      recorder.lightingManager_->endClusterCullTimer(cmd);
    }
  });

  graph.addPass(RenderPassId::GTAO, [&recorder](VkCommandBuffer cmd,
                                            const FrameRecordParams &p) {
    if (!displayModeRecordsGtao(currentDisplayMode(recorder.guiManager_)))
      return;
    if (recorder.environmentManager_ && recorder.environmentManager_->isGtaoReady() &&
        p.runtime.frame &&
        p.runtime.frame->depthSamplingView != VK_NULL_HANDLE) {
      recorder.environmentManager_->dispatchGtao(
          cmd, recorder.swapChainManager_.extent().width,
          recorder.swapChainManager_.extent().height, p.camera.cameraBuffer,
          p.camera.cameraBufferSize, p.runtime.frame->depthSamplingView,
          p.camera.gBufferSampler, p.runtime.frame->normal.view,
          p.camera.gBufferSampler);
      recorder.environmentManager_->dispatchGtaoBlur(
          cmd, p.runtime.frame->depthSamplingView, p.camera.gBufferSampler,
          p.camera.nearPlane, p.camera.farPlane);
    }
  });

  graph.addPass(RenderPassId::Lighting, [&recorder](VkCommandBuffer cmd,
                                                const FrameRecordParams &p) {
    const std::array<VkDescriptorSet, 2> lightingSets = {
        p.runtime.frame->lightingDescriptorSet,
        p.descriptors.lightDescriptorSet};
    const std::array<VkDescriptorSet, 4> transparentSets = {
        p.descriptors.sceneDescriptorSet, p.descriptors.lightDescriptorSet,
        p.runtime.frame->oitDescriptorSet,
        p.runtime.frame->lightingDescriptorSet};
    recorder.recordLightingPass(cmd, p, p.descriptors.sceneDescriptorSet, lightingSets,
                       transparentSets);
  });

  graph.addPass(RenderPassId::TransformGizmos,
                 [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
                   recorder.recordTransformGizmoPass(cmd, p);
                 });

  graph.addPass(
      RenderPassId::ExposureAdaptation,
      [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
        if (!displayModeRecordsExposureAdaptation(
                currentDisplayMode(recorder.guiManager_)))
          return;
        if (!recorder.exposureManager_ || !recorder.exposureManager_->isReady())
          return;
        if (!p.runtime.frame ||
            p.runtime.frame->sceneColor.view == VK_NULL_HANDLE ||
            p.runtime.frame->sceneColor.image == VK_NULL_HANDLE)
          return;

        const container::gpu::ExposureSettings exposureSettings =
            sanitizeExposureSettings(p.postProcess.exposureSettings);
        if (exposureSettings.mode != container::gpu::kExposureModeAuto)
          return;

        VkImageMemoryBarrier sceneBarrier{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        sceneBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        sceneBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sceneBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sceneBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sceneBarrier.image = p.runtime.frame->sceneColor.image;
        sceneBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        sceneBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sceneBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &sceneBarrier);

        const auto extent = recorder.swapChainManager_.extent();
        recorder.exposureManager_->dispatch(cmd, p.runtime.frame->sceneColor.view,
                                   extent.width, extent.height,
                                   exposureSettings);
      });

  graph.addPass(RenderPassId::OitResolve,
                 [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
                   if (!shouldRecordTransparentOit(p, recorder.guiManager_))
                     return;
                   recorder.oitManager_.prepareResolve(cmd, *p.runtime.frame);
                 });

  graph.addPass(RenderPassId::Bloom, [&recorder](VkCommandBuffer cmd,
                                             const FrameRecordParams &p) {
    if (!displayModeRecordsBloom(currentDisplayMode(recorder.guiManager_)))
      return;
    if (!recorder.bloomManager_ || !recorder.bloomManager_->isReady())
      return;
    if (!recorder.bloomManager_->enabled())
      return;
    if (!p.runtime.frame || p.runtime.frame->sceneColor.view == VK_NULL_HANDLE)
      return;

    // Barrier: scene color attachment write to compute shader read.
    VkImageMemoryBarrier sceneBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    sceneBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sceneBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sceneBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.image = p.runtime.frame->sceneColor.image;
    sceneBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    sceneBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sceneBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &sceneBarrier);

    const auto extent = recorder.swapChainManager_.extent();
    recorder.bloomManager_->dispatch(cmd, p.runtime.frame->sceneColor.view, extent.width,
                            extent.height);
  });

  graph.addPass(RenderPassId::PostProcess,
                 [&recorder](VkCommandBuffer cmd, const FrameRecordParams &p) {
                   const std::array<VkDescriptorSet, 2> ppSets = {
                       p.runtime.frame->postProcessDescriptorSet,
                       p.runtime.frame->oitDescriptorSet};
                   recorder.recordPostProcessPass(cmd, p, ppSets);
                 });

  graph.setPassReadiness(RenderPassId::FrustumCull,
                          [&recorder](const FrameRecordParams &p) {
    if (!recorder.gpuCullManager_ || !recorder.gpuCullManager_->isReady() ||
        !hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands)) {
      return renderPassNotNeeded();
    }
    if (p.camera.cameraBuffer == VK_NULL_HANDLE ||
        p.camera.cameraBufferSize == 0) {
      return renderPassMissingResource(RenderResourceId::CameraBuffer);
    }
    if (p.scene.objectBuffer == VK_NULL_HANDLE ||
        p.scene.objectBufferSize == 0) {
      return renderPassMissingResource(RenderResourceId::ObjectBuffer);
    }
    return renderPassReady();
  });

  graph.setPassReadiness(RenderPassId::BimDepthPrepass,
                          [](const FrameRecordParams &p) {
    if (!p.runtime.frame ||
        p.runtime.frame->bimDepthPrepassFramebuffer == VK_NULL_HANDLE ||
        p.renderPasses.bimDepthPrepass == VK_NULL_HANDLE) {
      return renderPassMissingResource(RenderResourceId::SceneDepth);
    }
    if (!hasBimOpaqueDrawCommands(p.bim)) {
      return renderPassNotNeeded();
    }
    if (p.bim.sceneDescriptorSet == VK_NULL_HANDLE ||
        p.bim.scene.vertexSlice.buffer == VK_NULL_HANDLE ||
        p.bim.scene.indexSlice.buffer == VK_NULL_HANDLE) {
      return renderPassMissingResource(RenderResourceId::BimGeometry);
    }
    return renderPassReady();
  });

  graph.setPassReadiness(RenderPassId::HiZGenerate,
                          [&recorder](const FrameRecordParams &p) {
    if (!recorder.gpuCullManager_ || !recorder.gpuCullManager_->isReady()) {
      return renderPassNotNeeded();
    }
    if (!p.runtime.frame ||
        p.runtime.frame->depthSamplingView == VK_NULL_HANDLE ||
        p.camera.gBufferSampler == VK_NULL_HANDLE) {
      return renderPassMissingResource(RenderResourceId::SceneDepth);
    }
    return renderPassReady();
  });

  graph.setPassReadiness(RenderPassId::OcclusionCull,
                          [&recorder](const FrameRecordParams &p) {
    if (!recorder.gpuCullManager_ || !recorder.gpuCullManager_->isReady() ||
        !hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands)) {
      return renderPassNotNeeded();
    }
    return renderPassReady();
  });

  graph.setPassReadiness(RenderPassId::CullStatsReadback,
                          [&recorder](const FrameRecordParams &) {
    return (recorder.gpuCullManager_ && recorder.gpuCullManager_->isReady())
               ? renderPassReady()
               : renderPassNotNeeded();
  });

  graph.setPassReadiness(RenderPassId::BimGBuffer,
                          [](const FrameRecordParams &p) {
    if (!p.runtime.frame ||
        p.runtime.frame->bimGBufferFramebuffer == VK_NULL_HANDLE ||
        p.renderPasses.bimGBuffer == VK_NULL_HANDLE) {
      return renderPassMissingResource(RenderResourceId::GBufferAlbedo);
    }
    if (!hasBimOpaqueDrawCommands(p.bim)) {
      return renderPassNotNeeded();
    }
    if (p.bim.sceneDescriptorSet == VK_NULL_HANDLE ||
        p.bim.scene.vertexSlice.buffer == VK_NULL_HANDLE ||
        p.bim.scene.indexSlice.buffer == VK_NULL_HANDLE) {
      return renderPassMissingResource(RenderResourceId::BimGeometry);
    }
    return renderPassReady();
  });

  graph.setPassReadiness(RenderPassId::TransparentPick,
                          [](const FrameRecordParams &p) {
    if (!p.runtime.frame ||
        p.runtime.frame->transparentPickFramebuffer == VK_NULL_HANDLE ||
        p.runtime.frame->depthStencil.image == VK_NULL_HANDLE ||
        p.runtime.frame->pickDepth.image == VK_NULL_HANDLE ||
        p.runtime.frame->pickId.image == VK_NULL_HANDLE ||
        p.renderPasses.transparentPick == VK_NULL_HANDLE) {
      return renderPassMissingResource(RenderResourceId::PickId);
    }
    if (!hasTransparentDrawCommands(p)) {
      return renderPassNotNeeded();
    }
    const bool sceneDrawable =
        p.descriptors.sceneDescriptorSet != VK_NULL_HANDLE &&
        p.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
        p.scene.indexSlice.buffer != VK_NULL_HANDLE &&
        hasTransparentDrawCommands(p.draws);
    if (!sceneDrawable && !hasBimTransparentGeometry(p)) {
      return renderPassMissingResource(RenderResourceId::SceneGeometry);
    }
    return renderPassReady();
  });

  graph.setPassReadiness(RenderPassId::OitClear,
                          [&recorder](const FrameRecordParams &p) {
    return shouldRecordTransparentOit(p, recorder.guiManager_) ? renderPassReady()
                                                      : renderPassNotNeeded();
  });

  for (uint32_t i = 0; i < shadowCullIds.size(); ++i) {
    graph.setPassReadiness(shadowCullIds[i],
                            [&recorder, i](const FrameRecordParams &p) {
      if (!displayModeRecordsShadowAtlas(currentDisplayMode(recorder.guiManager_)) ||
          !p.shadows.useGpuShadowCull ||
          p.shadows.shadowCullManager == nullptr ||
          !p.shadows.shadowCullManager->isReady() ||
          !hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands)) {
        return renderPassNotNeeded();
      }
      if (p.camera.cameraBuffer == VK_NULL_HANDLE ||
          p.camera.cameraBufferSize == 0) {
        return renderPassMissingResource(RenderResourceId::CameraBuffer);
      }
      return i < kShadowCascadeCount ? renderPassReady()
                                     : renderPassNotNeeded();
    });
  }

  for (uint32_t i = 0; i < shadowPassIds.size(); ++i) {
    graph.setPassReadiness(shadowPassIds[i],
                            [&recorder, i](const FrameRecordParams &p) {
      if (!displayModeRecordsShadowAtlas(currentDisplayMode(recorder.guiManager_))) {
        return renderPassNotNeeded();
      }
      if (!recorder.canRecordShadowPass(p, i)) {
        return renderPassMissingResource(RenderResourceId::ShadowAtlas);
      }
      return renderPassReady();
    });
  }

  graph.setPassReadiness(RenderPassId::TileCull,
                          [&recorder](const FrameRecordParams &p) {
    if (!displayModeRecordsTileCull(currentDisplayMode(recorder.guiManager_))) {
      return renderPassNotNeeded();
    }
    if (!recorder.lightingManager_ || !recorder.lightingManager_->isTiledLightingReady()) {
      return renderPassNotNeeded();
    }
    if (!p.runtime.frame ||
        p.runtime.frame->depthSamplingView == VK_NULL_HANDLE ||
        p.camera.cameraBuffer == VK_NULL_HANDLE ||
        p.camera.cameraBufferSize == 0) {
      return renderPassMissingResource(RenderResourceId::SceneDepth);
    }
    return renderPassReady();
  });

  graph.setPassReadiness(RenderPassId::GTAO,
                          [&recorder](const FrameRecordParams &p) {
    if (!displayModeRecordsGtao(currentDisplayMode(recorder.guiManager_))) {
      return renderPassNotNeeded();
    }
    if (!recorder.environmentManager_ || !recorder.environmentManager_->isGtaoReady() ||
        !recorder.environmentManager_->isAoEnabled()) {
      return renderPassNotNeeded();
    }
    if (!p.runtime.frame ||
        p.runtime.frame->depthSamplingView == VK_NULL_HANDLE ||
        p.runtime.frame->normal.view == VK_NULL_HANDLE ||
        p.camera.cameraBuffer == VK_NULL_HANDLE ||
        p.camera.cameraBufferSize == 0 ||
        p.camera.gBufferSampler == VK_NULL_HANDLE) {
      return renderPassMissingResource(RenderResourceId::SceneDepth);
    }
    return renderPassReady();
  });

  graph.setPassReadiness(RenderPassId::TransformGizmos,
                          [](const FrameRecordParams &) {
    return renderPassNotNeeded();
  });

  graph.setPassReadiness(RenderPassId::ExposureAdaptation,
                          [&recorder](const FrameRecordParams &p) {
    if (!displayModeRecordsExposureAdaptation(
            currentDisplayMode(recorder.guiManager_))) {
      return renderPassNotNeeded();
    }
    if (!recorder.exposureManager_ || !recorder.exposureManager_->isReady()) {
      return renderPassNotNeeded();
    }
    if (sanitizeExposureSettings(p.postProcess.exposureSettings).mode !=
        container::gpu::kExposureModeAuto) {
      return renderPassNotNeeded();
    }
    if (!p.runtime.frame ||
        p.runtime.frame->sceneColor.view == VK_NULL_HANDLE ||
        p.runtime.frame->sceneColor.image == VK_NULL_HANDLE) {
      return renderPassMissingResource(RenderResourceId::SceneColor);
    }
    return renderPassReady();
  });

  graph.setPassReadiness(RenderPassId::OitResolve,
                          [&recorder](const FrameRecordParams &p) {
    return shouldRecordTransparentOit(p, recorder.guiManager_) ? renderPassReady()
                                                      : renderPassNotNeeded();
  });

  graph.setPassReadiness(RenderPassId::Bloom,
                          [&recorder](const FrameRecordParams &p) {
    if (!displayModeRecordsBloom(currentDisplayMode(recorder.guiManager_)) ||
        !recorder.bloomManager_ || !recorder.bloomManager_->isReady() ||
        !recorder.bloomManager_->enabled()) {
      return renderPassNotNeeded();
    }
    if (!p.runtime.frame ||
        p.runtime.frame->sceneColor.view == VK_NULL_HANDLE ||
        p.runtime.frame->sceneColor.image == VK_NULL_HANDLE) {
      return renderPassMissingResource(RenderResourceId::SceneColor);
    }
    return renderPassReady();
  });

  graph.compile();
}

}  // namespace container::renderer

