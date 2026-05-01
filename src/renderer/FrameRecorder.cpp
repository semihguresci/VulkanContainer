#include "Container/renderer/FrameRecorder.h"
#include "Container/renderer/BloomManager.h"
#include "Container/renderer/EnvironmentManager.h"
#include "Container/renderer/ExposureManager.h"
#include "Container/renderer/GpuCullManager.h"
#include "Container/renderer/LightingManager.h"
#include "Container/renderer/OitManager.h"
#include "Container/renderer/SceneController.h"
#include "Container/renderer/ShadowCullManager.h"
#include "Container/renderer/ShadowManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <future>
#include <limits>
#include <stdexcept>

namespace container::renderer {

using container::gpu::BindlessPushConstants;
using container::gpu::kMaxDeferredPointLights;
using container::gpu::kTileSize;
using container::gpu::kShadowCascadeCount;
using container::gpu::kShadowMapResolution;
using container::gpu::ShadowPushConstants;
using container::gpu::PostProcessPushConstants;
using container::gpu::TiledLightingPushConstants;

namespace {

bool hasDrawCommands(const std::vector<DrawCommand>* commands) {
  return commands != nullptr && !commands->empty();
}

bool hasTransparentDrawCommands(const FrameRecordParams& p) {
  return hasDrawCommands(p.transparentSingleSidedDrawCommands) ||
         hasDrawCommands(p.transparentWindingFlippedDrawCommands) ||
         hasDrawCommands(p.transparentDoubleSidedDrawCommands);
}

VkPipeline choosePipeline(VkPipeline preferred, VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

constexpr uint32_t kIndirectObjectIndex =
    std::numeric_limits<uint32_t>::max();

float finiteOr(float value, float fallback) {
  return std::isfinite(value) ? value : fallback;
}

container::gpu::ExposureSettings sanitizeExposureSettings(
    container::gpu::ExposureSettings settings) {
  settings.mode = settings.mode == container::gpu::kExposureModeAuto
                      ? container::gpu::kExposureModeAuto
                      : container::gpu::kExposureModeManual;
  settings.manualExposure =
      std::max(finiteOr(settings.manualExposure, 0.25f), 0.0f);
  settings.targetLuminance =
      std::max(finiteOr(settings.targetLuminance, 0.18f), 0.001f);
  settings.minExposure =
      std::max(finiteOr(settings.minExposure, 0.03125f), 0.0f);
  settings.maxExposure =
      std::max(finiteOr(settings.maxExposure, 8.0f), settings.minExposure);
  settings.adaptationRate =
      std::max(finiteOr(settings.adaptationRate, 1.5f), 0.0f);
  settings.meteringLowPercentile =
      std::clamp(finiteOr(settings.meteringLowPercentile, 0.50f),
                 0.0f, 0.99f);
  settings.meteringHighPercentile =
      std::clamp(finiteOr(settings.meteringHighPercentile, 0.95f),
                 settings.meteringLowPercentile + 0.01f, 1.0f);
  return settings;
}

float resolvePostProcessExposure(
    const container::gpu::ExposureSettings& settings) {
  if (settings.mode == container::gpu::kExposureModeManual) {
    return settings.manualExposure;
  }

  return std::clamp(settings.manualExposure, settings.minExposure,
                    settings.maxExposure);
}

void pushSceneObjectIndex(VkCommandBuffer cmd,
                          VkPipelineLayout layout,
                          BindlessPushConstants& pc,
                          uint32_t objectIndex) {
  pc.objectIndex = objectIndex;
  vkCmdPushConstants(cmd, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(BindlessPushConstants), &pc);
}

bool shouldRecordTransparentOit(const FrameRecordParams& p,
                                const container::ui::GuiManager* guiManager) {
  if (!hasTransparentDrawCommands(p)) return false;

  const auto displayMode =
      guiManager ? guiManager->gBufferViewMode()
                 : container::ui::GBufferViewMode::Overview;
  if (displayMode == container::ui::GBufferViewMode::ObjectSpaceNormals) {
    return false;
  }

  const auto wireframeSettings =
      guiManager ? guiManager->wireframeSettings()
                 : container::ui::WireframeSettings{};
  const bool wireframeFullMode =
      guiManager && guiManager->wireframeSupported() &&
      wireframeSettings.enabled &&
      wireframeSettings.mode == container::ui::WireframeMode::Full &&
      p.pipelines.wireframeDepth != VK_NULL_HANDLE &&
      p.pipelines.wireframeNoDepth != VK_NULL_HANDLE;
  return !wireframeFullMode;
}

}  // namespace

FrameRecorder::FrameRecorder(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::SwapChainManager& swapChainManager,
    const OitManager& oitManager,
    const LightingManager* lightingManager,
    const EnvironmentManager* environmentManager,
    const SceneController* sceneController,
    GpuCullManager* gpuCullManager,
    BloomManager* bloomManager,
    ExposureManager* exposureManager,
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
    , exposureManager_(exposureManager)
    , camera_(camera)
    , guiManager_(guiManager) {
  buildGraph();
}
// ---------------------------------------------------------------------------
// Graph construction
// ---------------------------------------------------------------------------

void FrameRecorder::buildGraph() {
  graph_.clear();

  // CPU registration order is the frame schedule. Later passes rely on the
  // image layouts and indirect-count buffers produced by earlier nodes.
  graph_.addPass(RenderPassId::FrustumCull, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
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

  graph_.addPass(RenderPassId::DepthPrepass, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    recordDepthPrepass(cmd, p, p.sceneDescriptorSet);
  });

  graph_.addPass(RenderPassId::HiZGenerate, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
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
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
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
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &depthBarrier);
  });

  graph_.addPass(RenderPassId::OcclusionCull, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (gpuCullManager_ && gpuCullManager_->isReady() &&
        p.opaqueSingleSidedDrawCommands &&
        !p.opaqueSingleSidedDrawCommands->empty()) {
      gpuCullManager_->dispatchOcclusionCull(cmd, p.cameraBuffer, p.cameraBufferSize,
                                             static_cast<uint32_t>(p.opaqueSingleSidedDrawCommands->size()));
    }
  });

  graph_.addPass(RenderPassId::CullStatsReadback, [this](VkCommandBuffer cmd, const FrameRecordParams&) {
    if (gpuCullManager_ && gpuCullManager_->isReady())
      gpuCullManager_->scheduleStatsReadback(cmd);
  });

  graph_.addPass(RenderPassId::GBuffer, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    recordGBufferPass(cmd, p, p.sceneDescriptorSet);
  });

  graph_.addPass(RenderPassId::OitClear, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!shouldRecordTransparentOit(p, guiManager_)) return;
    oitManager_.clearResources(cmd, *p.frame, std::numeric_limits<uint32_t>::max());
  });

  const auto shadowCullIds = shadowCullPassIds();
  const auto shadowPassIds = shadowCascadePassIds();
  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    graph_.addPass(shadowCullIds[i],
        [this, i](VkCommandBuffer cmd, const FrameRecordParams& p) {
      if (!p.useGpuShadowCull || p.shadowCullManager == nullptr ||
          !p.shadowCullManager->isReady() ||
          p.opaqueSingleSidedDrawCommands == nullptr) {
        return;
      }

      const uint32_t drawCount = static_cast<uint32_t>(
          p.opaqueSingleSidedDrawCommands->size());
      if (drawCount == 0u) return;

      p.shadowCullManager->dispatchCascadeCull(cmd, p.imageIndex, i, drawCount);
    });

    graph_.addPass(shadowPassIds[i],
        [this, i](VkCommandBuffer cmd, const FrameRecordParams& p) {
      recordShadowPass(cmd, p, i);
    });
  }

  // Transition depth from attachment-writable to read-only for the compute
  // passes (TileCull, GTAO) that sample depth.  The lighting render pass
  // initialLayout also expects DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL.
  graph_.addPass(RenderPassId::DepthToReadOnly, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!p.frame || p.frame->depthStencil.image == VK_NULL_HANDLE) return;

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.image               = p.frame->depthStencil.image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
  });

  graph_.addPass(RenderPassId::TileCull, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (lightingManager_ && lightingManager_->isTiledLightingReady() &&
        p.frame && p.frame->depthSamplingView != VK_NULL_HANDLE) {
      lightingManager_->resetGpuTimers(cmd, p.imageIndex);
      lightingManager_->beginClusterCullTimer(cmd);
      lightingManager_->dispatchTileCull(
          cmd, swapChainManager_.extent(),
          p.cameraBuffer, p.cameraBufferSize,
          p.frame->depthSamplingView, p.cameraNear, p.cameraFar);
      lightingManager_->endClusterCullTimer(cmd);
    }
  });

  graph_.addPass(RenderPassId::GTAO, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (environmentManager_ && environmentManager_->isGtaoReady() &&
        p.frame && p.frame->depthSamplingView != VK_NULL_HANDLE) {
      environmentManager_->dispatchGtao(
          cmd, swapChainManager_.extent().width, swapChainManager_.extent().height,
          p.cameraBuffer, p.cameraBufferSize,
          p.frame->depthSamplingView, p.gBufferSampler,
          p.frame->normal.view, p.gBufferSampler);
      environmentManager_->dispatchGtaoBlur(
          cmd, p.frame->depthSamplingView, p.gBufferSampler,
          p.cameraNear, p.cameraFar);
    }
  });

  graph_.addPass(RenderPassId::Lighting, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    const std::array<VkDescriptorSet, 2> lightingSets = {
        p.frame->lightingDescriptorSet, p.lightDescriptorSet};
    const std::array<VkDescriptorSet, 4> transparentSets = {
        p.sceneDescriptorSet, p.lightDescriptorSet, p.frame->oitDescriptorSet,
        p.frame->lightingDescriptorSet};
    recordLightingPass(cmd, p, p.sceneDescriptorSet, lightingSets, transparentSets);
  });

  graph_.addPass(RenderPassId::ExposureAdaptation, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!exposureManager_ || !exposureManager_->isReady()) return;
    if (!p.frame || p.frame->sceneColor.view == VK_NULL_HANDLE ||
        p.frame->sceneColor.image == VK_NULL_HANDLE) return;

    const container::gpu::ExposureSettings exposureSettings =
        sanitizeExposureSettings(p.exposureSettings);
    if (exposureSettings.mode != container::gpu::kExposureModeAuto) return;

    VkImageMemoryBarrier sceneBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    sceneBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sceneBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sceneBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.image = p.frame->sceneColor.image;
    sceneBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    sceneBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sceneBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &sceneBarrier);

    const auto extent = swapChainManager_.extent();
    exposureManager_->dispatch(cmd, p.frame->sceneColor.view,
                               extent.width, extent.height,
                               exposureSettings);
  });

  graph_.addPass(RenderPassId::OitResolve, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!shouldRecordTransparentOit(p, guiManager_)) return;
    oitManager_.prepareResolve(cmd, *p.frame);
  });

  graph_.addPass(RenderPassId::Bloom, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!bloomManager_ || !bloomManager_->isReady()) return;
    if (!bloomManager_->enabled()) return;
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

  graph_.addPass(RenderPassId::PostProcess, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    const std::array<VkDescriptorSet, 2> ppSets = {
        p.frame->postProcessDescriptorSet, p.frame->oitDescriptorSet};
    recordPostProcessPass(cmd, p, ppSets);
  });

  graph_.compile();
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

void FrameRecorder::record(VkCommandBuffer commandBuffer,
                           const FrameRecordParams& p) const {
  if (commandBuffer == VK_NULL_HANDLE) {
    throw std::runtime_error("FrameRecorder::record received a null command buffer");
  }
  if (!p.frame) {
    throw std::runtime_error("FrameRecordParams::frame is null");
  }

  if (gpuCullManager_) {
    gpuCullManager_->beginFrameCulling();
  }

  if (p.useGpuShadowCull && p.shadowCullManager != nullptr &&
      p.shadowCullManager->isReady() &&
      p.opaqueSingleSidedDrawCommands != nullptr) {
    // Shadow culling is per-cascade, but all cascades consume the same source
    // draw list. Upload once before graph execution so each cascade pass can
    // filter into its own indirect buffer.
    p.shadowCullManager->ensureBufferCapacity(static_cast<uint32_t>(
        p.opaqueSingleSidedDrawCommands->size()));
    p.shadowCullManager->uploadDrawCommands(*p.opaqueSingleSidedDrawCommands);
  }

  prepareShadowCascadeDrawCommands(p);
  recordShadowCascadeSecondaryCommandBuffers(p);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    throw std::runtime_error("failed to begin recording command buffer!");
  }

  graph_.execute(commandBuffer, p);
  if (p.screenshot.enabled) {
    recordScreenshotCopy(commandBuffer, p);
  }

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to record command buffer!");
  }
}
// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool FrameRecorder::isPassActive(RenderPassId id) const {
  return graph_.isPassActive(id);
}

void FrameRecorder::prepareShadowCascadeDrawCommands(
    const FrameRecordParams& p) const {
  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    shadowCascadeSingleSidedDrawCommands_[cascadeIndex].clear();
    shadowCascadeWindingFlippedDrawCommands_[cascadeIndex].clear();
    shadowCascadeDoubleSidedDrawCommands_[cascadeIndex].clear();
  }

  const auto distributeCommands = [this](
                                      const std::vector<DrawCommand>* source,
                                      auto& destination) {
    if (source == nullptr) return;

    for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
         ++cascadeIndex) {
      destination[cascadeIndex].reserve(source->size());
    }

    for (const DrawCommand& command : *source) {
      for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
           ++cascadeIndex) {
        destination[cascadeIndex].push_back(command);
      }
    }
  };

  if (p.shadowManager == nullptr || p.objectData == nullptr) {
    // Without cascade/object bounds, use the full draw list for every cascade.
    // This keeps shadows correct and simply gives up CPU-side cascade pruning.
    distributeCommands(p.opaqueSingleSidedDrawCommands,
                       shadowCascadeSingleSidedDrawCommands_);
    distributeCommands(p.opaqueWindingFlippedDrawCommands,
                       shadowCascadeWindingFlippedDrawCommands_);
    distributeCommands(p.opaqueDoubleSidedDrawCommands,
                       shadowCascadeDoubleSidedDrawCommands_);
    return;
  }

  const auto filterCommands = [this, &p](
                                  const std::vector<DrawCommand>* source,
                                  auto& destination) {
    if (source == nullptr) return;

    for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
         ++cascadeIndex) {
      destination[cascadeIndex].reserve(source->size());
    }

    for (const DrawCommand& command : *source) {
      if (command.objectIndex >= p.objectData->size()) {
        for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
             ++cascadeIndex) {
          destination[cascadeIndex].push_back(command);
        }
        continue;
      }

      const glm::vec4 boundingSphere =
          (*p.objectData)[command.objectIndex].boundingSphere;
      const bool hasValidBounds = boundingSphere.w > 0.0f;

      for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
           ++cascadeIndex) {
        if (!hasValidBounds ||
            p.shadowManager->cascadeIntersectsSphere(cascadeIndex,
                                                     boundingSphere)) {
          destination[cascadeIndex].push_back(command);
        }
      }
    }
  };

  filterCommands(p.opaqueSingleSidedDrawCommands,
                 shadowCascadeSingleSidedDrawCommands_);
  filterCommands(p.opaqueWindingFlippedDrawCommands,
                 shadowCascadeWindingFlippedDrawCommands_);
  filterCommands(p.opaqueDoubleSidedDrawCommands,
                 shadowCascadeDoubleSidedDrawCommands_);
}

void FrameRecorder::setViewportAndScissor(VkCommandBuffer cmd) const {
  VkViewport viewport{};
  viewport.x        = 0.0f;
  // Scene passes use a negative-height viewport so NDC +Y maps to the top of
  // the framebuffer while projection matrices remain glTF/right-handed.
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
  const bool frustumCullActive = isPassActive(RenderPassId::FrustumCull);

  const VkPipeline depthFrontCullPipeline =
      choosePipeline(p.pipelines.depthPrepassFrontCull,
                     p.pipelines.depthPrepass);
  const VkPipeline depthNoCullPipeline =
      choosePipeline(p.pipelines.depthPrepassNoCull, p.pipelines.depthPrepass);

  // Single-sided opaque draws can use the GPU-produced indirect list. Double-
  // sided/alpha-special cases stay on explicit draw lists so pipeline variants
  // and material flags remain easy to reason about.
  if (gpuCullManager_ && gpuCullManager_->isReady() && frustumCullActive &&
      gpuCullManager_->frustumDrawsValid() &&
      p.opaqueSingleSidedDrawCommands &&
      !p.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipelines.depthPrepass);
    pushSceneObjectIndex(cmd, p.layouts.scene, *p.pushConstants.bindless,
                         kIndirectObjectIndex);
    gpuCullManager_->drawIndirect(cmd);
  } else if (p.opaqueSingleSidedDrawCommands &&
             !p.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipelines.depthPrepass);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueSingleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  if (p.opaqueWindingFlippedDrawCommands &&
      !p.opaqueWindingFlippedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      depthFrontCullPipeline);
    debugOverlay_.drawScene(cmd, p.layouts.scene,
                            *p.opaqueWindingFlippedDrawCommands,
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
  clearValues[0].color        = {{0.0f, 0.0f, 0.0f, 0.0f}};
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
  const bool frustumCullActive = isPassActive(RenderPassId::FrustumCull);

  const VkPipeline gBufferFrontCullPipeline =
      choosePipeline(p.pipelines.gBufferFrontCull, p.pipelines.gBuffer);
  const VkPipeline gBufferNoCullPipeline =
      choosePipeline(p.pipelines.gBufferNoCull, p.pipelines.gBuffer);

  // The same-frame Hi-Z pyramid contains this geometry from the depth prepass,
  // so consuming the occlusion list for G-buffer visibility can self-occlude
  // wall reliefs and thin fixtures. G-buffer therefore fails open to the
  // frustum list, with CPU draws as the final fallback.
  if (gpuCullManager_ && gpuCullManager_->isReady() && frustumCullActive &&
      gpuCullManager_->frustumDrawsValid() &&
      p.opaqueSingleSidedDrawCommands &&
      !p.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.gBuffer);
    pushSceneObjectIndex(cmd, p.layouts.scene, *p.pushConstants.bindless,
                         kIndirectObjectIndex);
    gpuCullManager_->drawIndirect(cmd);
  } else if (p.opaqueSingleSidedDrawCommands &&
             !p.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.gBuffer);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueSingleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  if (p.opaqueWindingFlippedDrawCommands &&
      !p.opaqueWindingFlippedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      gBufferFrontCullPipeline);
    debugOverlay_.drawScene(cmd, p.layouts.scene,
                            *p.opaqueWindingFlippedDrawCommands,
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

bool FrameRecorder::canRecordShadowPass(const FrameRecordParams& p,
                                         uint32_t cascadeIndex) const {
  return cascadeIndex < kShadowCascadeCount &&
         p.renderPasses.shadow != VK_NULL_HANDLE &&
         p.pipelines.shadowDepth != VK_NULL_HANDLE &&
         p.shadowFramebuffers != nullptr &&
         p.shadowFramebuffers[cascadeIndex] != VK_NULL_HANDLE;
}

bool FrameRecorder::shouldUseShadowSecondaryCommandBuffer(
    const FrameRecordParams& p,
    uint32_t cascadeIndex) const {
  return p.useShadowSecondaryCommandBuffers &&
         canRecordShadowPass(p, cascadeIndex) &&
         cascadeIndex < p.shadowSecondaryCommandBuffers.size() &&
         p.shadowSecondaryCommandBuffers[cascadeIndex] != VK_NULL_HANDLE;
}

void FrameRecorder::recordShadowCascadeSecondaryCommandBuffers(
    const FrameRecordParams& p) const {
  if (!p.useShadowSecondaryCommandBuffers) return;

  const auto shadowPassIds = shadowCascadePassIds();
  std::vector<std::future<void>> workers;
  workers.reserve(kShadowCascadeCount);

  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    if (cascadeIndex >= shadowPassIds.size() ||
        !isPassActive(shadowPassIds[cascadeIndex]) ||
        !shouldUseShadowSecondaryCommandBuffer(p, cascadeIndex)) {
      continue;
    }

    const VkCommandBuffer secondary =
        p.shadowSecondaryCommandBuffers[cascadeIndex];
    workers.emplace_back(std::async(
        std::launch::async,
        [this, &p, secondary, cascadeIndex]() {
          recordShadowCascadeSecondaryCommandBuffer(
              secondary, p, cascadeIndex);
        }));
  }

  for (auto& worker : workers) {
    worker.get();
  }
}

void FrameRecorder::recordShadowCascadeSecondaryCommandBuffer(
    VkCommandBuffer cmd,
    const FrameRecordParams& p,
    uint32_t cascadeIndex) const {
  if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) {
    throw std::runtime_error("failed to reset shadow secondary command buffer!");
  }

  VkCommandBufferInheritanceInfo inheritanceInfo{};
  inheritanceInfo.sType       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
  inheritanceInfo.renderPass  = p.renderPasses.shadow;
  inheritanceInfo.subpass     = 0;
  inheritanceInfo.framebuffer = p.shadowFramebuffers[cascadeIndex];

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags            = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
  beginInfo.pInheritanceInfo = &inheritanceInfo;
  if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
    throw std::runtime_error("failed to begin shadow secondary command buffer!");
  }

  recordShadowPassBody(cmd, p, cascadeIndex);

  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    throw std::runtime_error("failed to record shadow secondary command buffer!");
  }
}

void FrameRecorder::recordShadowPass(VkCommandBuffer cmd,
                                      const FrameRecordParams& p,
                                      uint32_t cascadeIndex) const {
  if (!canRecordShadowPass(p, cascadeIndex)) return;

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

  if (shouldUseShadowSecondaryCommandBuffer(p, cascadeIndex)) {
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    const VkCommandBuffer secondary =
        p.shadowSecondaryCommandBuffers[cascadeIndex];
    vkCmdExecuteCommands(cmd, 1, &secondary);
    vkCmdEndRenderPass(cmd);
    return;
  }

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  recordShadowPassBody(cmd, p, cascadeIndex);
  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordShadowPassBody(VkCommandBuffer cmd,
                                          const FrameRecordParams& p,
                                          uint32_t cascadeIndex) const {
  VkViewport viewport{};
  // Shadow maps intentionally use a positive-height viewport. Their atlas UV
  // mapping therefore differs from scene-buffer UV mapping and does not flip Y.
  viewport.width    = static_cast<float>(kShadowMapResolution);
  viewport.height   = static_cast<float>(kShadowMapResolution);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.extent = {kShadowMapResolution, kShadowMapResolution};
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  // Reverse-Z shadow maps need negative caster bias to push written depth
  // toward far (0.0). Keep this dynamic so tuning does not rebuild pipelines.
  vkCmdSetDepthBias(cmd,
                    p.shadowSettings.rasterConstantBias,
                    0.0f,
                    p.shadowSettings.rasterSlopeBias);

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

  const VkPipeline shadowFrontCullPipeline =
      choosePipeline(p.pipelines.shadowDepthFrontCull, p.pipelines.shadowDepth);
  const VkPipeline shadowNoCullPipeline =
      choosePipeline(p.pipelines.shadowDepthNoCull, p.pipelines.shadowDepth);

  const VkBuffer gpuShadowIndirectBuffer =
      (p.shadowCullManager != nullptr &&
       cascadeIndex < container::gpu::kShadowCascadeCount)
          ? p.shadowCullManager->indirectDrawBuffer(cascadeIndex)
          : VK_NULL_HANDLE;
  const VkBuffer gpuShadowCountBuffer =
      (p.shadowCullManager != nullptr &&
       cascadeIndex < container::gpu::kShadowCascadeCount)
          ? p.shadowCullManager->drawCountBuffer(cascadeIndex)
          : VK_NULL_HANDLE;
  const uint32_t gpuShadowMaxDrawCount =
      p.shadowCullManager != nullptr ? p.shadowCullManager->maxDrawCount() : 0u;
  const auto shadowCullIds = shadowCullPassIds();
  const bool shadowCullPassActive =
      cascadeIndex < shadowCullIds.size() &&
      isPassActive(shadowCullIds[cascadeIndex]);

  const bool useGpuShadowCull = p.useGpuShadowCull &&
                                shadowCullPassActive &&
                                p.shadowCullManager != nullptr &&
                                p.shadowCullManager->isReady() &&
                                p.opaqueSingleSidedDrawCommands != nullptr &&
                                !p.opaqueSingleSidedDrawCommands->empty() &&
                                cascadeIndex < container::gpu::kShadowCascadeCount &&
                                gpuShadowIndirectBuffer != VK_NULL_HANDLE &&
                                gpuShadowCountBuffer != VK_NULL_HANDLE &&
                                gpuShadowMaxDrawCount > 0;

  if (useGpuShadowCull) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.shadowDepth);
    spc.objectIndex = kIndirectObjectIndex;
    vkCmdPushConstants(cmd, p.layouts.shadow,
                       VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(ShadowPushConstants), &spc);
    vkCmdDrawIndexedIndirectCount(
        cmd,
        gpuShadowIndirectBuffer, 0,
        gpuShadowCountBuffer, 0,
        gpuShadowMaxDrawCount,
        sizeof(container::gpu::GpuDrawIndexedIndirectCommand));
  }

  const auto& singleSidedCommands =
      shadowCascadeSingleSidedDrawCommands_[cascadeIndex];
  if (!useGpuShadowCull && !singleSidedCommands.empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.shadowDepth);
    drawShadowList(&singleSidedCommands);
  }

  const auto& windingFlippedCommands =
      shadowCascadeWindingFlippedDrawCommands_[cascadeIndex];
  if (!windingFlippedCommands.empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      shadowFrontCullPipeline);
    drawShadowList(&windingFlippedCommands);
  }

  const auto& doubleSidedCommands =
      shadowCascadeDoubleSidedDrawCommands_[cascadeIndex];
  if (!doubleSidedCommands.empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowNoCullPipeline);
    drawShadowList(&doubleSidedCommands);
  }
}

void FrameRecorder::recordLightingPass(
    VkCommandBuffer cmd, const FrameRecordParams& p, VkDescriptorSet sceneSet,
    const std::array<VkDescriptorSet, 2>& lightingDescriptorSets,
    const std::array<VkDescriptorSet, 4>& transparentDescriptorSets) const {
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
  const float wireframeIntensity = wireframeFullMode
                                       ? 1.0f
                                       : wireframeSettings.overlayIntensity;
  const VkPipeline activeWireframePipeline =
      wireframeSettings.depthTest ? p.pipelines.wireframeDepth : p.pipelines.wireframeNoDepth;
  const VkPipeline activeWireframeFrontCullPipeline =
      wireframeSettings.depthTest
          ? choosePipeline(p.pipelines.wireframeDepthFrontCull,
                           p.pipelines.wireframeDepth)
          : choosePipeline(p.pipelines.wireframeNoDepthFrontCull,
                           p.pipelines.wireframeNoDepth);
  const bool showNormalValidation =
      guiManager_ && guiManager_->showNormalValidation() &&
      p.pipelines.normalValidation != VK_NULL_HANDLE;
  const bool transparentOitEnabled =
      shouldRecordTransparentOit(p, guiManager_);

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

  const auto bindWireframePipeline = [&](VkPipeline pipeline) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (p.wireframeRasterModeSupported) {
      const float lineWidth = p.wireframeWideLinesSupported
                                  ? wireframeSettings.lineWidth
                                  : 1.0f;
      vkCmdSetLineWidth(cmd, lineWidth);
    }
  };
  const auto drawWireframeCommands =
      [&](const std::vector<DrawCommand>* commands, VkPipeline pipeline) {
    if (!hasDrawCommands(commands) || pipeline == VK_NULL_HANDLE) return;
    bindWireframePipeline(pipeline);
    debugOverlay_.drawWireframe(cmd, p.layouts.wireframe, *commands,
                                wireframeSettings.color, wireframeIntensity,
                                wireframeSettings.lineWidth,
                                *p.pushConstants.wireframe);
  };

  if (wireframeFullMode) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.layouts.wireframe, 0, 1, &p.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    drawWireframeCommands(p.opaqueSingleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.transparentSingleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.opaqueWindingFlippedDrawCommands,
                          activeWireframeFrontCullPipeline);
    drawWireframeCommands(p.transparentWindingFlippedDrawCommands,
                          activeWireframeFrontCullPipeline);
    drawWireframeCommands(p.opaqueDoubleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.transparentDoubleSidedDrawCommands,
                          activeWireframePipeline);
    bindWireframePipeline(activeWireframePipeline);
    drawDiagnosticCube(cmd, p.layouts.wireframe, p.diagCubeObjectIndex,
                       *p.pushConstants.bindless);
  } else if (showObjectSpaceNormals &&
             p.pipelines.objectNormalDebug != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.layouts.scene, 0, 1, &p.sceneDescriptorSet, 0,
                            nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    const VkPipeline objectNormalNoCullPipeline =
        choosePipeline(p.pipelines.objectNormalDebugNoCull,
                       p.pipelines.objectNormalDebug);
    const VkPipeline objectNormalFrontCullPipeline =
        choosePipeline(p.pipelines.objectNormalDebugFrontCull,
                       p.pipelines.objectNormalDebug);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipelines.objectNormalDebug);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.opaqueSingleSidedDrawCommands,
                            *p.pushConstants.bindless);
    debugOverlay_.drawScene(cmd, p.layouts.scene, *p.transparentSingleSidedDrawCommands,
                            *p.pushConstants.bindless);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      objectNormalFrontCullPipeline);
    debugOverlay_.drawScene(cmd, p.layouts.scene,
                            *p.opaqueWindingFlippedDrawCommands,
                            *p.pushConstants.bindless);
    debugOverlay_.drawScene(cmd, p.layouts.scene,
                            *p.transparentWindingFlippedDrawCommands,
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
    const std::array<VkDescriptorSet, 3> directionalDescriptorSets = {
        lightingDescriptorSets[0], lightingDescriptorSets[1], sceneSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.directionalLight);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layouts.lighting, 0,
                            static_cast<uint32_t>(directionalDescriptorSets.size()),
                            directionalDescriptorSets.data(), 0, nullptr);
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
    const bool tileCullActive = isPassActive(RenderPassId::TileCull);
    const bool useTiled =
        tileCullActive &&
        lightingManager_ && lightingManager_->isTiledLightingReady() &&
        !lightingManager_->pointLightsSsbo().empty() &&
        p.frame && p.frame->depthSamplingView != VK_NULL_HANDLE &&
        p.pipelines.tiledPointLight != VK_NULL_HANDLE &&
        p.tiledDescriptorSet != VK_NULL_HANDLE;

    if (useTiled) {
      // Tiled point light accumulation — single fullscreen triangle.
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        p.pipelines.tiledPointLight);
      const std::array<VkDescriptorSet, 3> tiledSets = {
          p.frame->lightingDescriptorSet, p.tiledDescriptorSet, sceneSet};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              p.layouts.tiledLighting, 0,
                              static_cast<uint32_t>(tiledSets.size()),
                              tiledSets.data(), 0, nullptr);
      const auto extent = swapChainManager_.extent();
      const uint32_t tileCountX =
          std::max(1u, (extent.width + kTileSize - 1u) / kTileSize);
      const uint32_t tileCountY =
          std::max(1u, (extent.height + kTileSize - 1u) / kTileSize);
      TiledLightingPushConstants tlpc{};
      tlpc.tileCountX = tileCountX;
      tlpc.tileCountY = tileCountY;
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
      const auto* pointLights =
          lightingManager_ ? &lightingManager_->pointLightsSsbo() : nullptr;
      const uint32_t numLights = std::min(
          pointLights ? static_cast<uint32_t>(pointLights->size()) : 0u,
          kMaxDeferredPointLights);

      if (numLights > 0u) {
        const std::array<VkDescriptorSet, 3> pointLightingSets = {
            lightingDescriptorSets[0], lightingDescriptorSets[1], sceneSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                p.layouts.lighting, 0,
                                static_cast<uint32_t>(pointLightingSets.size()),
                                pointLightingSets.data(), 0, nullptr);
      }
      for (uint32_t i = 0; i < numLights; ++i) {
        vkCmdClearAttachments(cmd, 1, &stencilClearAttachment, 1, &stencilClearRect);
        p.pushConstants.light->positionRadius = (*pointLights)[i].positionRadius;
        p.pushConstants.light->colorIntensity = (*pointLights)[i].colorIntensity;
        p.pushConstants.light->directionInnerCos =
            (*pointLights)[i].directionInnerCos;
        p.pushConstants.light->coneOuterCosType =
            (*pointLights)[i].coneOuterCosType;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.stencilVolume);
        vkCmdPushConstants(cmd, p.layouts.lighting,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(LightPushConstants), p.pushConstants.light);
        vkCmdDraw(cmd, lightingManager_ ? lightingManager_->lightVolumeIndexCount() : 0, 1, 0, 0);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePointPipeline);
        vkCmdPushConstants(cmd, p.layouts.lighting,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(LightPushConstants), p.pushConstants.light);
        vkCmdDraw(cmd, 3, 1, 0, 0);
      }
    }
  }

  if (transparentOitEnabled) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layouts.transparent, 0,
                            static_cast<uint32_t>(transparentDescriptorSets.size()),
                            transparentDescriptorSets.data(), 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    const VkPipeline transparentFrontCullPipeline =
        choosePipeline(p.pipelines.transparentFrontCull, p.pipelines.transparent);
    const VkPipeline transparentNoCullPipeline =
        choosePipeline(p.pipelines.transparentNoCull, p.pipelines.transparent);

    if (p.transparentSingleSidedDrawCommands &&
        !p.transparentSingleSidedDrawCommands->empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipelines.transparent);
      debugOverlay_.drawScene(cmd, p.layouts.transparent,
                              *p.transparentSingleSidedDrawCommands,
                              *p.pushConstants.bindless);
    }
    if (p.transparentWindingFlippedDrawCommands &&
        !p.transparentWindingFlippedDrawCommands->empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        transparentFrontCullPipeline);
      debugOverlay_.drawScene(cmd, p.layouts.transparent,
                              *p.transparentWindingFlippedDrawCommands,
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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.layouts.normalValidation, 0, 1, &p.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    const VkPipeline normalValidationFrontCullPipeline =
        choosePipeline(p.pipelines.normalValidationFrontCull,
                       p.pipelines.normalValidation);
    const VkPipeline normalValidationNoCullPipeline =
        choosePipeline(p.pipelines.normalValidationNoCull,
                       p.pipelines.normalValidation);
    static const std::vector<DrawCommand> emptyDrawCommands;
    const auto drawNormalValidationCommands =
        [&](const std::vector<DrawCommand>* opaque,
            const std::vector<DrawCommand>* transparent,
            VkPipeline pipeline,
            uint32_t faceClassificationFlags) {
          if (!hasDrawCommands(opaque) && !hasDrawCommands(transparent)) {
            return;
          }
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          debugOverlay_.recordNormalValidation(
              cmd, p.layouts.normalValidation,
              opaque ? *opaque : emptyDrawCommands,
              transparent ? *transparent : emptyDrawCommands,
              faceClassificationFlags,
              guiManager_->normalValidationSettings(),
              *p.pushConstants.normalValidation);
        };

    drawNormalValidationCommands(p.opaqueSingleSidedDrawCommands,
                                 p.transparentSingleSidedDrawCommands,
                                 p.pipelines.normalValidation,
                                 0u);
    drawNormalValidationCommands(p.opaqueWindingFlippedDrawCommands,
                                 p.transparentWindingFlippedDrawCommands,
                                 normalValidationFrontCullPipeline,
                                 kNormalValidationInvertFaceClassification);
    drawNormalValidationCommands(p.opaqueDoubleSidedDrawCommands,
                                 p.transparentDoubleSidedDrawCommands,
                                 normalValidationNoCullPipeline,
                                 kNormalValidationBothSidesValid);
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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.layouts.wireframe, 0, 1, &p.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.vertexSlice, p.indexSlice, p.indexType);
    drawWireframeCommands(p.opaqueSingleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.transparentSingleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.opaqueWindingFlippedDrawCommands,
                          activeWireframeFrontCullPipeline);
    drawWireframeCommands(p.transparentWindingFlippedDrawCommands,
                          activeWireframeFrontCullPipeline);
    drawWireframeCommands(p.opaqueDoubleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.transparentDoubleSidedDrawCommands,
                          activeWireframePipeline);
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
  const bool bloomPassActive = isPassActive(RenderPassId::Bloom);
  const auto shadowPassIds = shadowCascadePassIds();
  const bool shadowEnabled = std::ranges::any_of(shadowPassIds, [this](RenderPassId id) {
    return isPassActive(id);
  });
  const bool tileCullActive = isPassActive(RenderPassId::TileCull) && lightingManager_ &&
                              lightingManager_->isTiledLightingReady();
  ppPc.bloomEnabled = (bloomManager_ && bloomManager_->isReady() &&
                       bloomManager_->enabled() && bloomPassActive)
                          ? 1u
                          : 0u;
  ppPc.bloomIntensity = bloomManager_ ? bloomManager_->intensity() : 0.0f;
  const container::gpu::ExposureSettings exposureSettings =
      sanitizeExposureSettings(p.exposureSettings);
  ppPc.exposure = exposureManager_
                      ? exposureManager_->resolvedExposure(exposureSettings)
                      : resolvePostProcessExposure(exposureSettings);
  ppPc.exposureMode = exposureSettings.mode;
  ppPc.targetLuminance = exposureSettings.targetLuminance;
  ppPc.minExposure = exposureSettings.minExposure;
  ppPc.maxExposure = exposureSettings.maxExposure;
  ppPc.adaptationRate = exposureSettings.adaptationRate;
  ppPc.cameraNear = p.cameraNear;
  ppPc.cameraFar  = p.cameraFar;
  if (shadowEnabled && p.shadowData) {
    for (uint32_t i = 0; i < kShadowCascadeCount; ++i)
      ppPc.cascadeSplits[i] = p.shadowData->cascades[i].splitDepth;
  }
  if (tileCullActive) {
    const auto extent = swapChainManager_.extent();
    ppPc.tileCountX  = (extent.width + container::gpu::kTileSize - 1) / container::gpu::kTileSize;
    ppPc.totalLights = static_cast<uint32_t>(lightingManager_->pointLightsSsbo().size());
    ppPc.depthSliceCount = container::gpu::kClusterDepthSlices;
  } else {
    ppPc.tileCountX = 1u;
    ppPc.totalLights = 0u;
    ppPc.depthSliceCount = 1u;
  }
  if (!tileCullActive && displayMode == GBufferViewMode::TileLightHeatMap) {
    ppPc.totalLights = 0u;
  }
  ppPc.oitEnabled = shouldRecordTransparentOit(p, guiManager_) ? 1u : 0u;
  vkCmdPushConstants(cmd, p.layouts.postProcess, VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(PostProcessPushConstants), &ppPc);
  vkCmdDraw(cmd, 3, 1, 0, 0);

  if (guiManager_) guiManager_->render(cmd);

  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordScreenshotCopy(VkCommandBuffer cmd,
                                         const FrameRecordParams& p) const {
  if (p.screenshot.swapChainImage == VK_NULL_HANDLE ||
      p.screenshot.readbackBuffer == VK_NULL_HANDLE ||
      p.screenshot.extent.width == 0 || p.screenshot.extent.height == 0) {
    return;
  }

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = p.screenshot.swapChainImage;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy copyRegion{};
  copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copyRegion.imageSubresource.layerCount = 1;
  copyRegion.imageExtent = {p.screenshot.extent.width,
                            p.screenshot.extent.height, 1};
  vkCmdCopyImageToBuffer(cmd, p.screenshot.swapChainImage,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         p.screenshot.readbackBuffer, 1, &copyRegion);

  VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.buffer = p.screenshot.readbackBuffer;
  hostRead.offset = 0;
  hostRead.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                       &hostRead, 0, nullptr);

  VkImageMemoryBarrier toPresent = toTransfer;
  toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toPresent.dstAccessMask = 0;
  toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toPresent);
}

}  // namespace container::renderer
