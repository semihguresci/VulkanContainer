#include "Container/renderer/FrameRecorder.h"
#include "Container/renderer/BloomManager.h"
#include "Container/renderer/EnvironmentManager.h"
#include "Container/renderer/ExposureManager.h"
#include "Container/renderer/GpuCullManager.h"
#include "Container/renderer/LightingManager.h"
#include "Container/renderer/OitManager.h"
#include "Container/renderer/RenderPassGpuProfiler.h"
#include "Container/renderer/RendererTelemetry.h"
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
#include <cstddef>
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
  return hasDrawCommands(p.draws.transparentSingleSidedDrawCommands) ||
         hasDrawCommands(p.draws.transparentWindingFlippedDrawCommands) ||
         hasDrawCommands(p.draws.transparentDoubleSidedDrawCommands);
}

VkPipeline choosePipeline(VkPipeline preferred, VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

constexpr uint32_t kIndirectObjectIndex =
    std::numeric_limits<uint32_t>::max();
constexpr size_t kMinParallelShadowCascadeCpuCommands = 512;

void mixHash(uint64_t& signature, uint64_t value) {
  signature ^= value;
  signature *= 1099511628211ull;
}

void hashBytes(uint64_t& signature, const void* data, size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < size; ++i) {
    mixHash(signature, bytes[i]);
  }
}

size_t drawCommandCount(const std::vector<DrawCommand>* commands) {
  return commands != nullptr ? commands->size() : 0u;
}

container::ui::GBufferViewMode currentDisplayMode(
    const container::ui::GuiManager* guiManager) {
  return guiManager ? guiManager->gBufferViewMode()
                    : container::ui::GBufferViewMode::Overview;
}

bool displayModeRecordsShadowAtlas(container::ui::GBufferViewMode mode) {
  return mode == container::ui::GBufferViewMode::Lit;
}

bool displayModeRecordsTileCull(container::ui::GBufferViewMode mode) {
  return mode == container::ui::GBufferViewMode::Lit ||
         mode == container::ui::GBufferViewMode::TileLightHeatMap;
}

bool displayModeRecordsGtao(container::ui::GBufferViewMode mode) {
  return mode == container::ui::GBufferViewMode::Lit;
}

bool displayModeRecordsExposureAdaptation(container::ui::GBufferViewMode mode) {
  return mode == container::ui::GBufferViewMode::Lit;
}

bool displayModeRecordsBloom(container::ui::GBufferViewMode mode) {
  return mode == container::ui::GBufferViewMode::Lit;
}

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

  const auto displayMode = currentDisplayMode(guiManager);
  if (displayMode != container::ui::GBufferViewMode::Lit &&
      displayMode != container::ui::GBufferViewMode::Transparency &&
      displayMode != container::ui::GBufferViewMode::Revealage) {
    return false;
  }

  const auto wireframeSettings =
      guiManager ? guiManager->wireframeSettings()
                 : container::ui::WireframeSettings{};
  const bool wireframeFullMode =
      guiManager && guiManager->wireframeSupported() &&
      wireframeSettings.enabled &&
      wireframeSettings.mode == container::ui::WireframeMode::Full &&
      p.pipeline.pipelines.wireframeDepth != VK_NULL_HANDLE &&
      p.pipeline.pipelines.wireframeNoDepth != VK_NULL_HANDLE;
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
        p.draws.opaqueSingleSidedDrawCommands &&
        !p.draws.opaqueSingleSidedDrawCommands->empty()) {
      gpuCullManager_->ensureBufferCapacity(
          static_cast<uint32_t>(p.draws.opaqueSingleSidedDrawCommands->size()));
      if (p.scene.objectBuffer != VK_NULL_HANDLE && p.scene.objectBufferSize > 0)
        gpuCullManager_->updateObjectSsboDescriptor(p.scene.objectBuffer, p.scene.objectBufferSize);
      gpuCullManager_->uploadDrawCommands(*p.draws.opaqueSingleSidedDrawCommands);

      // Handle freeze-culling: snapshot camera on first frozen frame.
      if (p.debug.debugFreezeCulling && !gpuCullManager_->cullingFrozen())
        gpuCullManager_->freezeCulling(cmd, p.camera.cameraBuffer, p.camera.cameraBufferSize);
      else if (!p.debug.debugFreezeCulling && gpuCullManager_->cullingFrozen())
        gpuCullManager_->unfreezeCulling();

      gpuCullManager_->dispatchFrustumCull(cmd, p.camera.cameraBuffer, p.camera.cameraBufferSize,
                                static_cast<uint32_t>(p.draws.opaqueSingleSidedDrawCommands->size()));
    }
  });

  graph_.addPass(RenderPassId::DepthPrepass, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    recordDepthPrepass(cmd, p, p.descriptors.sceneDescriptorSet);
  });

  graph_.addPass(RenderPassId::HiZGenerate, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!gpuCullManager_ || !gpuCullManager_->isReady() ||
        !p.runtime.frame || p.runtime.frame->depthSamplingView == VK_NULL_HANDLE ||
        p.camera.gBufferSampler == VK_NULL_HANDLE) return;

    const auto extent = swapChainManager_.extent();
    gpuCullManager_->ensureHiZImage(extent.width, extent.height);

    // Transition depth from attachment to shader-readable for Hi-Z sampling.
    VkImageMemoryBarrier depthBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    depthBarrier.oldLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.newLayout     = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.image         = p.runtime.frame->depthStencil.image;
    depthBarrier.subresourceRange = {
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &depthBarrier);

    gpuCullManager_->dispatchHiZGenerate(cmd, p.runtime.frame->depthSamplingView,
                                         p.camera.gBufferSampler,
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
        p.draws.opaqueSingleSidedDrawCommands &&
        !p.draws.opaqueSingleSidedDrawCommands->empty()) {
      gpuCullManager_->dispatchOcclusionCull(cmd, p.camera.cameraBuffer, p.camera.cameraBufferSize,
                                             static_cast<uint32_t>(p.draws.opaqueSingleSidedDrawCommands->size()));
    }
  });

  graph_.addPass(RenderPassId::CullStatsReadback, [this](VkCommandBuffer cmd, const FrameRecordParams&) {
    if (gpuCullManager_ && gpuCullManager_->isReady())
      gpuCullManager_->scheduleStatsReadback(cmd);
  });

  graph_.addPass(RenderPassId::GBuffer, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    recordGBufferPass(cmd, p, p.descriptors.sceneDescriptorSet);
  });

  graph_.addPass(RenderPassId::OitClear, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!shouldRecordTransparentOit(p, guiManager_)) return;
    oitManager_.clearResources(cmd, *p.runtime.frame, std::numeric_limits<uint32_t>::max());
  });

  const auto shadowCullIds = shadowCullPassIds();
  const auto shadowPassIds = shadowCascadePassIds();
  for (uint32_t i = 0; i < kShadowCascadeCount; ++i) {
    graph_.addPass(shadowCullIds[i],
        [this, i](VkCommandBuffer cmd, const FrameRecordParams& p) {
      if (!displayModeRecordsShadowAtlas(currentDisplayMode(guiManager_))) {
        return;
      }
      if (!p.shadows.useGpuShadowCull || p.shadows.shadowCullManager == nullptr ||
          !p.shadows.shadowCullManager->isReady() ||
          p.draws.opaqueSingleSidedDrawCommands == nullptr) {
        return;
      }

      const uint32_t drawCount = static_cast<uint32_t>(
          p.draws.opaqueSingleSidedDrawCommands->size());
      if (drawCount == 0u) return;

      p.shadows.shadowCullManager->dispatchCascadeCull(cmd, p.runtime.imageIndex, i, drawCount);
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
    if (!p.runtime.frame || p.runtime.frame->depthStencil.image == VK_NULL_HANDLE) return;

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout           = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.image               = p.runtime.frame->depthStencil.image;
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
    if (!displayModeRecordsTileCull(currentDisplayMode(guiManager_))) return;
    if (lightingManager_ && lightingManager_->isTiledLightingReady() &&
        p.runtime.frame && p.runtime.frame->depthSamplingView != VK_NULL_HANDLE) {
      lightingManager_->resetGpuTimers(cmd, p.runtime.imageIndex);
      lightingManager_->beginClusterCullTimer(cmd);
      lightingManager_->dispatchTileCull(
          cmd, swapChainManager_.extent(),
          p.camera.cameraBuffer, p.camera.cameraBufferSize,
          p.runtime.frame->depthSamplingView, p.camera.nearPlane, p.camera.farPlane);
      lightingManager_->endClusterCullTimer(cmd);
    }
  });

  graph_.addPass(RenderPassId::GTAO, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!displayModeRecordsGtao(currentDisplayMode(guiManager_))) return;
    if (environmentManager_ && environmentManager_->isGtaoReady() &&
        p.runtime.frame && p.runtime.frame->depthSamplingView != VK_NULL_HANDLE) {
      environmentManager_->dispatchGtao(
          cmd, swapChainManager_.extent().width, swapChainManager_.extent().height,
          p.camera.cameraBuffer, p.camera.cameraBufferSize,
          p.runtime.frame->depthSamplingView, p.camera.gBufferSampler,
          p.runtime.frame->normal.view, p.camera.gBufferSampler);
      environmentManager_->dispatchGtaoBlur(
          cmd, p.runtime.frame->depthSamplingView, p.camera.gBufferSampler,
          p.camera.nearPlane, p.camera.farPlane);
    }
  });

  graph_.addPass(RenderPassId::Lighting, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    const std::array<VkDescriptorSet, 2> lightingSets = {
        p.runtime.frame->lightingDescriptorSet, p.descriptors.lightDescriptorSet};
    const std::array<VkDescriptorSet, 4> transparentSets = {
        p.descriptors.sceneDescriptorSet, p.descriptors.lightDescriptorSet, p.runtime.frame->oitDescriptorSet,
        p.runtime.frame->lightingDescriptorSet};
    recordLightingPass(cmd, p, p.descriptors.sceneDescriptorSet, lightingSets, transparentSets);
  });

  graph_.addPass(RenderPassId::ExposureAdaptation, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!displayModeRecordsExposureAdaptation(currentDisplayMode(guiManager_))) return;
    if (!exposureManager_ || !exposureManager_->isReady()) return;
    if (!p.runtime.frame || p.runtime.frame->sceneColor.view == VK_NULL_HANDLE ||
        p.runtime.frame->sceneColor.image == VK_NULL_HANDLE) return;

    const container::gpu::ExposureSettings exposureSettings =
        sanitizeExposureSettings(p.postProcess.exposureSettings);
    if (exposureSettings.mode != container::gpu::kExposureModeAuto) return;

    VkImageMemoryBarrier sceneBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    sceneBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sceneBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sceneBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.image = p.runtime.frame->sceneColor.image;
    sceneBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    sceneBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sceneBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &sceneBarrier);

    const auto extent = swapChainManager_.extent();
    exposureManager_->dispatch(cmd, p.runtime.frame->sceneColor.view,
                               extent.width, extent.height,
                               exposureSettings);
  });

  graph_.addPass(RenderPassId::OitResolve, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!shouldRecordTransparentOit(p, guiManager_)) return;
    oitManager_.prepareResolve(cmd, *p.runtime.frame);
  });

  graph_.addPass(RenderPassId::Bloom, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    if (!displayModeRecordsBloom(currentDisplayMode(guiManager_))) return;
    if (!bloomManager_ || !bloomManager_->isReady()) return;
    if (!bloomManager_->enabled()) return;
    if (!p.runtime.frame || p.runtime.frame->sceneColor.view == VK_NULL_HANDLE) return;

    // Barrier: scene color attachment write ? compute shader read.
    VkImageMemoryBarrier sceneBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    sceneBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sceneBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sceneBarrier.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sceneBarrier.image         = p.runtime.frame->sceneColor.image;
    sceneBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    sceneBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sceneBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &sceneBarrier);

    const auto extent = swapChainManager_.extent();
    bloomManager_->dispatch(cmd, p.runtime.frame->sceneColor.view, extent.width, extent.height);
  });

  graph_.addPass(RenderPassId::PostProcess, [this](VkCommandBuffer cmd, const FrameRecordParams& p) {
    const std::array<VkDescriptorSet, 2> ppSets = {
        p.runtime.frame->postProcessDescriptorSet, p.runtime.frame->oitDescriptorSet};
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
  if (!p.runtime.frame) {
    throw std::runtime_error("FrameRecordParams::runtime.frame is null");
  }

  if (gpuCullManager_) {
    gpuCullManager_->beginFrameCulling();
  }

  if (displayModeRecordsShadowAtlas(currentDisplayMode(guiManager_)) &&
      p.shadows.useGpuShadowCull && p.shadows.shadowCullManager != nullptr &&
      p.shadows.shadowCullManager->isReady() &&
      p.draws.opaqueSingleSidedDrawCommands != nullptr) {
    // Shadow culling is per-cascade, but all cascades consume the same source
    // draw list. Upload once before graph execution so each cascade pass can
    // filter into its own indirect buffer.
    p.shadows.shadowCullManager->ensureBufferCapacity(static_cast<uint32_t>(
        p.draws.opaqueSingleSidedDrawCommands->size()));
    p.shadows.shadowCullManager->uploadDrawCommands(*p.draws.opaqueSingleSidedDrawCommands);
  }

  if (shouldPrepareShadowCascadeDrawCommands(p)) {
    prepareShadowCascadeDrawCommands(p);
  } else {
    for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
         ++cascadeIndex) {
      shadowCascadeSingleSidedDrawCommands_[cascadeIndex].clear();
      shadowCascadeWindingFlippedDrawCommands_[cascadeIndex].clear();
      shadowCascadeDoubleSidedDrawCommands_[cascadeIndex].clear();
    }
    shadowCascadeDrawCommandCacheValid_ = false;
  }
  recordShadowCascadeSecondaryCommandBuffers(p);

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    throw std::runtime_error("failed to begin recording command buffer!");
  }

  if (p.services.telemetry || p.services.gpuProfiler) {
    if (p.services.gpuProfiler) {
      p.services.gpuProfiler->beginFrame(commandBuffer, p.runtime.imageIndex);
    }
    RenderPassExecutionHooks hooks{};
    hooks.beginPass = [gpuProfiler = p.services.gpuProfiler](
                          RenderPassId id, VkCommandBuffer cmd) {
      if (gpuProfiler) {
        gpuProfiler->beginPass(cmd, id);
      }
    };
    hooks.endPass = [telemetry = p.services.telemetry,
                     gpuProfiler = p.services.gpuProfiler](
                        RenderPassId id, VkCommandBuffer cmd, float cpuMs) {
      if (gpuProfiler) {
        gpuProfiler->endPass(cmd, id);
      }
      if (telemetry) {
        telemetry->recordPassCpuTime(id, cpuMs);
      }
    };
    graph_.execute(commandBuffer, p, hooks);
  } else {
    graph_.execute(commandBuffer, p);
  }
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

bool FrameRecorder::shouldPrepareShadowCascadeDrawCommands(
    const FrameRecordParams& p) const {
  if (!displayModeRecordsShadowAtlas(currentDisplayMode(guiManager_))) {
    return false;
  }

  const auto shadowPassIds = shadowCascadePassIds();
  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    if (cascadeIndex >= shadowPassIds.size() ||
        !isPassActive(shadowPassIds[cascadeIndex]) ||
        !canRecordShadowPass(p, cascadeIndex)) {
      continue;
    }

    if (hasDrawCommands(p.draws.opaqueWindingFlippedDrawCommands) ||
        hasDrawCommands(p.draws.opaqueDoubleSidedDrawCommands)) {
      return true;
    }
    if (!useGpuShadowCullForCascade(p, cascadeIndex) &&
        hasDrawCommands(p.draws.opaqueSingleSidedDrawCommands)) {
      return true;
    }
  }
  return false;
}

bool FrameRecorder::useGpuShadowCullForCascade(
    const FrameRecordParams& p,
    uint32_t cascadeIndex) const {
  const auto shadowCullIds = shadowCullPassIds();
  const bool shadowCullPassActive =
      cascadeIndex < shadowCullIds.size() &&
      isPassActive(shadowCullIds[cascadeIndex]);
  const VkBuffer gpuShadowIndirectBuffer =
      (p.shadows.shadowCullManager != nullptr &&
       cascadeIndex < container::gpu::kShadowCascadeCount)
          ? p.shadows.shadowCullManager->indirectDrawBuffer(cascadeIndex)
          : VK_NULL_HANDLE;
  const VkBuffer gpuShadowCountBuffer =
      (p.shadows.shadowCullManager != nullptr &&
       cascadeIndex < container::gpu::kShadowCascadeCount)
          ? p.shadows.shadowCullManager->drawCountBuffer(cascadeIndex)
          : VK_NULL_HANDLE;
  const uint32_t gpuShadowMaxDrawCount =
      p.shadows.shadowCullManager != nullptr
          ? p.shadows.shadowCullManager->maxDrawCount()
          : 0u;

  return p.shadows.useGpuShadowCull &&
         shadowCullPassActive &&
         p.shadows.shadowCullManager != nullptr &&
         p.shadows.shadowCullManager->isReady() &&
         p.draws.opaqueSingleSidedDrawCommands != nullptr &&
         !p.draws.opaqueSingleSidedDrawCommands->empty() &&
         cascadeIndex < container::gpu::kShadowCascadeCount &&
         gpuShadowIndirectBuffer != VK_NULL_HANDLE &&
         gpuShadowCountBuffer != VK_NULL_HANDLE &&
         gpuShadowMaxDrawCount > 0;
}

size_t FrameRecorder::shadowCascadeCpuCommandCount(
    const FrameRecordParams& p,
    uint32_t cascadeIndex) const {
  if (cascadeIndex >= kShadowCascadeCount) {
    return 0u;
  }

  size_t count =
      shadowCascadeWindingFlippedDrawCommands_[cascadeIndex].size() +
      shadowCascadeDoubleSidedDrawCommands_[cascadeIndex].size();
  if (!useGpuShadowCullForCascade(p, cascadeIndex)) {
    count += shadowCascadeSingleSidedDrawCommands_[cascadeIndex].size();
  }
  return count;
}

uint64_t FrameRecorder::shadowCascadeDrawCommandSignature(
    const FrameRecordParams& p) const {
  uint64_t signature = 1469598103934665603ull;
  mixHash(signature, p.scene.objectDataRevision);
  mixHash(signature, reinterpret_cast<uintptr_t>(p.scene.objectData));
  mixHash(signature, p.scene.objectData ? p.scene.objectData->size() : 0u);
  mixHash(signature, reinterpret_cast<uintptr_t>(p.shadows.shadowManager));
  mixHash(signature, p.shadows.useGpuShadowCull ? 1u : 0u);

  const auto shadowPassIds = shadowCascadePassIds();
  const auto shadowCullIds = shadowCullPassIds();
  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    mixHash(signature,
            cascadeIndex < shadowPassIds.size() &&
                    isPassActive(shadowPassIds[cascadeIndex])
                ? 1u
                : 0u);
    mixHash(signature,
            cascadeIndex < shadowCullIds.size() &&
                    isPassActive(shadowCullIds[cascadeIndex])
                ? 1u
                : 0u);
  }

  mixHash(signature, reinterpret_cast<uintptr_t>(
                         p.draws.opaqueSingleSidedDrawCommands));
  mixHash(signature, drawCommandCount(p.draws.opaqueSingleSidedDrawCommands));
  mixHash(signature, reinterpret_cast<uintptr_t>(
                         p.draws.opaqueWindingFlippedDrawCommands));
  mixHash(signature, drawCommandCount(p.draws.opaqueWindingFlippedDrawCommands));
  mixHash(signature, reinterpret_cast<uintptr_t>(
                         p.draws.opaqueDoubleSidedDrawCommands));
  mixHash(signature, drawCommandCount(p.draws.opaqueDoubleSidedDrawCommands));

  if (p.shadows.shadowData != nullptr) {
    hashBytes(signature, p.shadows.shadowData, sizeof(*p.shadows.shadowData));
  }
  return signature;
}

void FrameRecorder::prepareShadowCascadeDrawCommands(
    const FrameRecordParams& p) const {
  const uint64_t signature = shadowCascadeDrawCommandSignature(p);
  if (shadowCascadeDrawCommandCacheValid_ &&
      shadowCascadeDrawCommandSignature_ == signature) {
    return;
  }

  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    shadowCascadeSingleSidedDrawCommands_[cascadeIndex].clear();
    shadowCascadeWindingFlippedDrawCommands_[cascadeIndex].clear();
    shadowCascadeDoubleSidedDrawCommands_[cascadeIndex].clear();
  }

  const auto shouldWriteCascade = [this, &p](
                                      bool skipGpuCulledSingleSided,
                                      uint32_t cascadeIndex) {
    return !skipGpuCulledSingleSided ||
           !useGpuShadowCullForCascade(p, cascadeIndex);
  };

  const auto distributeCommands = [this, &shouldWriteCascade](
                                      const std::vector<DrawCommand>* source,
                                      auto& destination,
                                      bool skipGpuCulledSingleSided) {
    if (source == nullptr) return;

    for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
         ++cascadeIndex) {
      if (!shouldWriteCascade(skipGpuCulledSingleSided, cascadeIndex)) {
        continue;
      }
      destination[cascadeIndex].reserve(source->size());
    }

    for (const DrawCommand& command : *source) {
      for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
           ++cascadeIndex) {
        if (!shouldWriteCascade(skipGpuCulledSingleSided, cascadeIndex)) {
          continue;
        }
        destination[cascadeIndex].push_back(command);
      }
    }
  };

  if (p.shadows.shadowManager == nullptr || p.scene.objectData == nullptr) {
    // Without cascade/object bounds, use the full draw list for every cascade.
    // This keeps shadows correct and simply gives up CPU-side cascade pruning.
    distributeCommands(p.draws.opaqueSingleSidedDrawCommands,
                       shadowCascadeSingleSidedDrawCommands_, true);
    distributeCommands(p.draws.opaqueWindingFlippedDrawCommands,
                       shadowCascadeWindingFlippedDrawCommands_, false);
    distributeCommands(p.draws.opaqueDoubleSidedDrawCommands,
                       shadowCascadeDoubleSidedDrawCommands_, false);
    shadowCascadeDrawCommandSignature_ = signature;
    shadowCascadeDrawCommandCacheValid_ = true;
    return;
  }

  const auto filterCommands = [this, &p, &shouldWriteCascade](
                                  const std::vector<DrawCommand>* source,
                                  auto& destination,
                                  bool skipGpuCulledSingleSided) {
    if (source == nullptr) return;

    for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
         ++cascadeIndex) {
      if (!shouldWriteCascade(skipGpuCulledSingleSided, cascadeIndex)) {
        continue;
      }
      destination[cascadeIndex].reserve(source->size());
    }

    for (const DrawCommand& command : *source) {
      if (command.objectIndex >= p.scene.objectData->size()) {
        for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
             ++cascadeIndex) {
          if (!shouldWriteCascade(skipGpuCulledSingleSided, cascadeIndex)) {
            continue;
          }
          destination[cascadeIndex].push_back(command);
        }
        continue;
      }

      const glm::vec4 boundingSphere =
          (*p.scene.objectData)[command.objectIndex].boundingSphere;
      const bool hasValidBounds = boundingSphere.w > 0.0f;

      for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
           ++cascadeIndex) {
        if (!shouldWriteCascade(skipGpuCulledSingleSided, cascadeIndex)) {
          continue;
        }
        if (!hasValidBounds ||
            p.shadows.shadowManager->cascadeIntersectsSphere(cascadeIndex,
                                                     boundingSphere)) {
          destination[cascadeIndex].push_back(command);
        }
      }
    }
  };

  filterCommands(p.draws.opaqueSingleSidedDrawCommands,
                 shadowCascadeSingleSidedDrawCommands_, true);
  filterCommands(p.draws.opaqueWindingFlippedDrawCommands,
                 shadowCascadeWindingFlippedDrawCommands_, false);
  filterCommands(p.draws.opaqueDoubleSidedDrawCommands,
                 shadowCascadeDoubleSidedDrawCommands_, false);
  shadowCascadeDrawCommandSignature_ = signature;
  shadowCascadeDrawCommandCacheValid_ = true;
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
  info.framebuffer       = p.runtime.frame->depthPrepassFramebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = swapChainManager_.extent();
  VkClearValue clearVal{};
  clearVal.depthStencil  = {0.0f, 0};
  info.clearValueCount   = 1;
  info.pClearValues      = &clearVal;

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.layouts.scene, 0, 1, &sceneSet, 0, nullptr);
  setViewportAndScissor(cmd);
  bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType);
  const bool frustumCullActive = isPassActive(RenderPassId::FrustumCull);

  const VkPipeline depthFrontCullPipeline =
      choosePipeline(p.pipeline.pipelines.depthPrepassFrontCull,
                     p.pipeline.pipelines.depthPrepass);
  const VkPipeline depthNoCullPipeline =
      choosePipeline(p.pipeline.pipelines.depthPrepassNoCull, p.pipeline.pipelines.depthPrepass);

  // Single-sided opaque draws can use the GPU-produced indirect list. Double-
  // sided/alpha-special cases stay on explicit draw lists so pipeline variants
  // and material flags remain easy to reason about.
  if (gpuCullManager_ && gpuCullManager_->isReady() && frustumCullActive &&
      gpuCullManager_->frustumDrawsValid() &&
      p.draws.opaqueSingleSidedDrawCommands &&
      !p.draws.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.depthPrepass);
    pushSceneObjectIndex(cmd, p.pipeline.layouts.scene, *p.pushConstants.bindless,
                         kIndirectObjectIndex);
    gpuCullManager_->drawIndirect(cmd);
  } else if (p.draws.opaqueSingleSidedDrawCommands &&
             !p.draws.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.depthPrepass);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene, *p.draws.opaqueSingleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  if (p.draws.opaqueWindingFlippedDrawCommands &&
      !p.draws.opaqueWindingFlippedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      depthFrontCullPipeline);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                            *p.draws.opaqueWindingFlippedDrawCommands,
                            *p.pushConstants.bindless);
  }

  if (p.draws.opaqueDoubleSidedDrawCommands &&
      !p.draws.opaqueDoubleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depthNoCullPipeline);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene, *p.draws.opaqueDoubleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.depthPrepass);
  drawDiagnosticCube(cmd, p.pipeline.layouts.scene, p.scene.diagCubeObjectIndex,
                     *p.pushConstants.bindless);
  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordGBufferPass(VkCommandBuffer cmd, const FrameRecordParams& p,
                                       VkDescriptorSet sceneSet) const {
  VkRenderPassBeginInfo info{};
  info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass        = p.renderPasses.gBuffer;
  info.framebuffer       = p.runtime.frame->gBufferFramebuffer;
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
                          p.pipeline.layouts.scene, 0, 1, &sceneSet, 0, nullptr);
  setViewportAndScissor(cmd);
  bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType);
  const bool frustumCullActive = isPassActive(RenderPassId::FrustumCull);

  const VkPipeline gBufferFrontCullPipeline =
      choosePipeline(p.pipeline.pipelines.gBufferFrontCull, p.pipeline.pipelines.gBuffer);
  const VkPipeline gBufferNoCullPipeline =
      choosePipeline(p.pipeline.pipelines.gBufferNoCull, p.pipeline.pipelines.gBuffer);

  // The same-frame Hi-Z pyramid contains this geometry from the depth prepass,
  // so consuming the occlusion list for G-buffer visibility can self-occlude
  // wall reliefs and thin fixtures. G-buffer therefore fails open to the
  // frustum list, with CPU draws as the final fallback.
  if (gpuCullManager_ && gpuCullManager_->isReady() && frustumCullActive &&
      gpuCullManager_->frustumDrawsValid() &&
      p.draws.opaqueSingleSidedDrawCommands &&
      !p.draws.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.gBuffer);
    pushSceneObjectIndex(cmd, p.pipeline.layouts.scene, *p.pushConstants.bindless,
                         kIndirectObjectIndex);
    gpuCullManager_->drawIndirect(cmd);
  } else if (p.draws.opaqueSingleSidedDrawCommands &&
             !p.draws.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.gBuffer);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene, *p.draws.opaqueSingleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  if (p.draws.opaqueWindingFlippedDrawCommands &&
      !p.draws.opaqueWindingFlippedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      gBufferFrontCullPipeline);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                            *p.draws.opaqueWindingFlippedDrawCommands,
                            *p.pushConstants.bindless);
  }

  if (p.draws.opaqueDoubleSidedDrawCommands &&
      !p.draws.opaqueDoubleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gBufferNoCullPipeline);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene, *p.draws.opaqueDoubleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.gBuffer);
  drawDiagnosticCube(cmd, p.pipeline.layouts.scene, p.scene.diagCubeObjectIndex,
                     *p.pushConstants.bindless);
  vkCmdEndRenderPass(cmd);
}

bool FrameRecorder::canRecordShadowPass(const FrameRecordParams& p,
                                         uint32_t cascadeIndex) const {
  return cascadeIndex < kShadowCascadeCount &&
         p.renderPasses.shadow != VK_NULL_HANDLE &&
         p.pipeline.pipelines.shadowDepth != VK_NULL_HANDLE &&
         p.shadows.shadowFramebuffers != nullptr &&
         p.shadows.shadowFramebuffers[cascadeIndex] != VK_NULL_HANDLE;
}

bool FrameRecorder::shouldUseShadowSecondaryCommandBuffer(
    const FrameRecordParams& p,
    uint32_t cascadeIndex) const {
  return p.shadows.useShadowSecondaryCommandBuffers &&
         canRecordShadowPass(p, cascadeIndex) &&
         cascadeIndex < p.shadows.shadowSecondaryCommandBuffers.size() &&
         p.shadows.shadowSecondaryCommandBuffers[cascadeIndex] != VK_NULL_HANDLE &&
         shadowCascadeCpuCommandCount(p, cascadeIndex) >=
             kMinParallelShadowCascadeCpuCommands;
}

void FrameRecorder::recordShadowCascadeSecondaryCommandBuffers(
    const FrameRecordParams& p) const {
  if (!p.shadows.useShadowSecondaryCommandBuffers) return;

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
        p.shadows.shadowSecondaryCommandBuffers[cascadeIndex];
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
  inheritanceInfo.framebuffer = p.shadows.shadowFramebuffers[cascadeIndex];

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
  if (!displayModeRecordsShadowAtlas(currentDisplayMode(guiManager_))) return;
  if (!canRecordShadowPass(p, cascadeIndex)) return;

  VkRenderPassBeginInfo info{};
  info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass        = p.renderPasses.shadow;
  info.framebuffer       = p.shadows.shadowFramebuffers[cascadeIndex];
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = {kShadowMapResolution, kShadowMapResolution};
  VkClearValue clearVal{};
  clearVal.depthStencil  = {0.0f, 0};
  info.clearValueCount   = 1;
  info.pClearValues      = &clearVal;

  if (shouldUseShadowSecondaryCommandBuffer(p, cascadeIndex)) {
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    const VkCommandBuffer secondary =
        p.shadows.shadowSecondaryCommandBuffers[cascadeIndex];
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
                    p.shadows.shadowSettings.rasterConstantBias,
                    0.0f,
                    p.shadows.shadowSettings.rasterSlopeBias);

  std::array<VkDescriptorSet, 2> shadowSets = {
      p.descriptors.sceneDescriptorSet, p.descriptors.shadowDescriptorSet};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.layouts.shadow, 0,
                          static_cast<uint32_t>(shadowSets.size()),
                          shadowSets.data(), 0, nullptr);

  bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType);

  ShadowPushConstants spc{};
  spc.cascadeIndex = cascadeIndex;

  const auto drawShadowList = [&](const std::vector<DrawCommand>* commands) {
    if (commands == nullptr) return;
    for (const auto& dc : *commands) {
      spc.objectIndex = dc.objectIndex;
      vkCmdPushConstants(cmd, p.pipeline.layouts.shadow,
                         VK_SHADER_STAGE_VERTEX_BIT,
                         0, sizeof(ShadowPushConstants), &spc);
      vkCmdDrawIndexed(cmd, dc.indexCount, 1, dc.firstIndex, 0, dc.objectIndex);
    }
  };

  const VkPipeline shadowFrontCullPipeline =
      choosePipeline(p.pipeline.pipelines.shadowDepthFrontCull, p.pipeline.pipelines.shadowDepth);
  const VkPipeline shadowNoCullPipeline =
      choosePipeline(p.pipeline.pipelines.shadowDepthNoCull, p.pipeline.pipelines.shadowDepth);

  const VkBuffer gpuShadowIndirectBuffer =
      (p.shadows.shadowCullManager != nullptr &&
       cascadeIndex < container::gpu::kShadowCascadeCount)
          ? p.shadows.shadowCullManager->indirectDrawBuffer(cascadeIndex)
          : VK_NULL_HANDLE;
  const VkBuffer gpuShadowCountBuffer =
      (p.shadows.shadowCullManager != nullptr &&
       cascadeIndex < container::gpu::kShadowCascadeCount)
          ? p.shadows.shadowCullManager->drawCountBuffer(cascadeIndex)
          : VK_NULL_HANDLE;
  const uint32_t gpuShadowMaxDrawCount =
      p.shadows.shadowCullManager != nullptr ? p.shadows.shadowCullManager->maxDrawCount() : 0u;
  const bool useGpuShadowCull = useGpuShadowCullForCascade(p, cascadeIndex);

  if (useGpuShadowCull) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.shadowDepth);
    spc.objectIndex = kIndirectObjectIndex;
    vkCmdPushConstants(cmd, p.pipeline.layouts.shadow,
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.shadowDepth);
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

  const GBufferViewMode displayMode = currentDisplayMode(guiManager_);
  const bool showObjectSpaceNormals = displayMode == GBufferViewMode::ObjectSpaceNormals;
  const auto wireframeSettings =
      guiManager_ ? guiManager_->wireframeSettings() : container::ui::WireframeSettings{};
  const bool wireframeEnabled =
      guiManager_ && guiManager_->wireframeSupported() && wireframeSettings.enabled &&
      p.pipeline.pipelines.wireframeDepth != VK_NULL_HANDLE &&
      p.pipeline.pipelines.wireframeNoDepth != VK_NULL_HANDLE;
  const bool wireframeFullMode =
      wireframeEnabled && wireframeSettings.mode == WireframeMode::Full;
  const bool wireframeOverlayMode =
      wireframeEnabled && wireframeSettings.mode == WireframeMode::Overlay;
  const float wireframeIntensity = wireframeFullMode
                                       ? 1.0f
                                       : wireframeSettings.overlayIntensity;
  const VkPipeline activeWireframePipeline =
      wireframeSettings.depthTest ? p.pipeline.pipelines.wireframeDepth : p.pipeline.pipelines.wireframeNoDepth;
  const VkPipeline activeWireframeFrontCullPipeline =
      wireframeSettings.depthTest
          ? choosePipeline(p.pipeline.pipelines.wireframeDepthFrontCull,
                           p.pipeline.pipelines.wireframeDepth)
          : choosePipeline(p.pipeline.pipelines.wireframeNoDepthFrontCull,
                           p.pipeline.pipelines.wireframeNoDepth);
  const bool showNormalValidation =
      guiManager_ && guiManager_->showNormalValidation() &&
      p.pipeline.pipelines.normalValidation != VK_NULL_HANDLE;
  const bool transparentOitEnabled =
      shouldRecordTransparentOit(p, guiManager_);

  VkRenderPassBeginInfo lightingPassInfo{};
  lightingPassInfo.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  lightingPassInfo.renderPass  = p.renderPasses.lighting;
  lightingPassInfo.framebuffer = p.runtime.frame->lightingFramebuffer;
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
    if (p.debug.wireframeRasterModeSupported) {
      const float lineWidth = p.debug.wireframeWideLinesSupported
                                  ? wireframeSettings.lineWidth
                                  : 1.0f;
      vkCmdSetLineWidth(cmd, lineWidth);
    }
  };
  const auto drawWireframeCommands =
      [&](const std::vector<DrawCommand>* commands, VkPipeline pipeline) {
    if (!hasDrawCommands(commands) || pipeline == VK_NULL_HANDLE) return;
    bindWireframePipeline(pipeline);
    debugOverlay_.drawWireframe(cmd, p.pipeline.layouts.wireframe, *commands,
                                wireframeSettings.color, wireframeIntensity,
                                wireframeSettings.lineWidth,
                                *p.pushConstants.wireframe);
  };

  if (wireframeFullMode) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.pipeline.layouts.wireframe, 0, 1, &p.descriptors.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType);
    drawWireframeCommands(p.draws.opaqueSingleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.draws.transparentSingleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.draws.opaqueWindingFlippedDrawCommands,
                          activeWireframeFrontCullPipeline);
    drawWireframeCommands(p.draws.transparentWindingFlippedDrawCommands,
                          activeWireframeFrontCullPipeline);
    drawWireframeCommands(p.draws.opaqueDoubleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.draws.transparentDoubleSidedDrawCommands,
                          activeWireframePipeline);
    bindWireframePipeline(activeWireframePipeline);
    drawDiagnosticCube(cmd, p.pipeline.layouts.wireframe, p.scene.diagCubeObjectIndex,
                       *p.pushConstants.bindless);
  } else if (showObjectSpaceNormals &&
             p.pipeline.pipelines.objectNormalDebug != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.pipeline.layouts.scene, 0, 1, &p.descriptors.sceneDescriptorSet, 0,
                            nullptr);
    bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType);
    const VkPipeline objectNormalNoCullPipeline =
        choosePipeline(p.pipeline.pipelines.objectNormalDebugNoCull,
                       p.pipeline.pipelines.objectNormalDebug);
    const VkPipeline objectNormalFrontCullPipeline =
        choosePipeline(p.pipeline.pipelines.objectNormalDebugFrontCull,
                       p.pipeline.pipelines.objectNormalDebug);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.objectNormalDebug);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene, *p.draws.opaqueSingleSidedDrawCommands,
                            *p.pushConstants.bindless);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene, *p.draws.transparentSingleSidedDrawCommands,
                            *p.pushConstants.bindless);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      objectNormalFrontCullPipeline);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                            *p.draws.opaqueWindingFlippedDrawCommands,
                            *p.pushConstants.bindless);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                            *p.draws.transparentWindingFlippedDrawCommands,
                            *p.pushConstants.bindless);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      objectNormalNoCullPipeline);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene, *p.draws.opaqueDoubleSidedDrawCommands,
                            *p.pushConstants.bindless);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene, *p.draws.transparentDoubleSidedDrawCommands,
                            *p.pushConstants.bindless);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.objectNormalDebug);
    drawDiagnosticCube(cmd, p.pipeline.layouts.scene, p.scene.diagCubeObjectIndex,
                       *p.pushConstants.bindless);
  } else {
    const std::array<VkDescriptorSet, 3> directionalDescriptorSets = {
        lightingDescriptorSets[0], lightingDescriptorSets[1], sceneSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.directionalLight);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.layouts.lighting, 0,
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

  if (!wireframeFullMode && !showObjectSpaceNormals && !p.debug.debugDirectionalOnly) {
    const bool tileCullActive = isPassActive(RenderPassId::TileCull);
    const bool useTiled =
        tileCullActive &&
        lightingManager_ && lightingManager_->isTiledLightingReady() &&
        !lightingManager_->pointLightsSsbo().empty() &&
        p.runtime.frame && p.runtime.frame->depthSamplingView != VK_NULL_HANDLE &&
        p.pipeline.pipelines.tiledPointLight != VK_NULL_HANDLE &&
        p.descriptors.tiledDescriptorSet != VK_NULL_HANDLE;

    if (useTiled) {
      // Tiled point light accumulation — single fullscreen triangle.
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        p.pipeline.pipelines.tiledPointLight);
      const std::array<VkDescriptorSet, 3> tiledSets = {
          p.runtime.frame->lightingDescriptorSet, p.descriptors.tiledDescriptorSet, sceneSet};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              p.pipeline.layouts.tiledLighting, 0,
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
      tlpc.cameraNear = p.camera.nearPlane;
      tlpc.cameraFar = p.camera.farPlane;
      vkCmdPushConstants(cmd, p.pipeline.layouts.tiledLighting,
                         VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(TiledLightingPushConstants), &tlpc);
      lightingManager_->beginClusteredLightingTimer(cmd);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      lightingManager_->endClusteredLightingTimer(cmd);
    } else {
      // Fallback: per-light stencil loop.
      const VkPipeline activePointPipeline =
          p.debug.debugVisualizePointLightStencil ? p.pipeline.pipelines.pointLightStencilDebug
                                           : p.pipeline.pipelines.pointLight;
      const auto* pointLights =
          lightingManager_ ? &lightingManager_->pointLightsSsbo() : nullptr;
      const uint32_t numLights = std::min(
          pointLights ? static_cast<uint32_t>(pointLights->size()) : 0u,
          kMaxDeferredPointLights);

      if (numLights > 0u) {
        const std::array<VkDescriptorSet, 3> pointLightingSets = {
            lightingDescriptorSets[0], lightingDescriptorSets[1], sceneSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                p.pipeline.layouts.lighting, 0,
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

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.stencilVolume);
        vkCmdPushConstants(cmd, p.pipeline.layouts.lighting,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(LightPushConstants), p.pushConstants.light);
        vkCmdDraw(cmd, lightingManager_ ? lightingManager_->lightVolumeIndexCount() : 0, 1, 0, 0);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, activePointPipeline);
        vkCmdPushConstants(cmd, p.pipeline.layouts.lighting,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(LightPushConstants), p.pushConstants.light);
        vkCmdDraw(cmd, 3, 1, 0, 0);
      }
    }
  }

  if (transparentOitEnabled) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.layouts.transparent, 0,
                            static_cast<uint32_t>(transparentDescriptorSets.size()),
                            transparentDescriptorSets.data(), 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType);
    const VkPipeline transparentFrontCullPipeline =
        choosePipeline(p.pipeline.pipelines.transparentFrontCull, p.pipeline.pipelines.transparent);
    const VkPipeline transparentNoCullPipeline =
        choosePipeline(p.pipeline.pipelines.transparentNoCull, p.pipeline.pipelines.transparent);

    if (p.draws.transparentSingleSidedDrawCommands &&
        !p.draws.transparentSingleSidedDrawCommands->empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.transparent);
      debugOverlay_.drawScene(cmd, p.pipeline.layouts.transparent,
                              *p.draws.transparentSingleSidedDrawCommands,
                              *p.pushConstants.bindless);
    }
    if (p.draws.transparentWindingFlippedDrawCommands &&
        !p.draws.transparentWindingFlippedDrawCommands->empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        transparentFrontCullPipeline);
      debugOverlay_.drawScene(cmd, p.pipeline.layouts.transparent,
                              *p.draws.transparentWindingFlippedDrawCommands,
                              *p.pushConstants.bindless);
    }
    if (p.draws.transparentDoubleSidedDrawCommands &&
        !p.draws.transparentDoubleSidedDrawCommands->empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, transparentNoCullPipeline);
      debugOverlay_.drawScene(cmd, p.pipeline.layouts.transparent,
                              *p.draws.transparentDoubleSidedDrawCommands,
                              *p.pushConstants.bindless);
    }
  }

  if (guiManager_ && guiManager_->showGeometryOverlay() &&
      p.pipeline.pipelines.geometryDebug != VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.geometryDebug);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.pipeline.layouts.scene, 0, 1, &p.descriptors.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene, *p.draws.opaqueDrawCommands,
                            *p.pushConstants.bindless);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene, *p.draws.transparentDrawCommands,
                            *p.pushConstants.bindless);
  }

  if (showNormalValidation && p.pipeline.pipelines.normalValidation != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.pipeline.layouts.normalValidation, 0, 1, &p.descriptors.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType);
    const VkPipeline normalValidationFrontCullPipeline =
        choosePipeline(p.pipeline.pipelines.normalValidationFrontCull,
                       p.pipeline.pipelines.normalValidation);
    const VkPipeline normalValidationNoCullPipeline =
        choosePipeline(p.pipeline.pipelines.normalValidationNoCull,
                       p.pipeline.pipelines.normalValidation);
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
              cmd, p.pipeline.layouts.normalValidation,
              opaque ? *opaque : emptyDrawCommands,
              transparent ? *transparent : emptyDrawCommands,
              faceClassificationFlags,
              guiManager_->normalValidationSettings(),
              *p.pushConstants.normalValidation);
        };

    drawNormalValidationCommands(p.draws.opaqueSingleSidedDrawCommands,
                                 p.draws.transparentSingleSidedDrawCommands,
                                 p.pipeline.pipelines.normalValidation,
                                 0u);
    drawNormalValidationCommands(p.draws.opaqueWindingFlippedDrawCommands,
                                 p.draws.transparentWindingFlippedDrawCommands,
                                 normalValidationFrontCullPipeline,
                                 kNormalValidationInvertFaceClassification);
    drawNormalValidationCommands(p.draws.opaqueDoubleSidedDrawCommands,
                                 p.draws.transparentDoubleSidedDrawCommands,
                                 normalValidationNoCullPipeline,
                                 kNormalValidationBothSidesValid);
  }

  const bool showSurfaceNormalLines =
      (showNormalValidation && p.pipeline.pipelines.surfaceNormalLine != VK_NULL_HANDLE) ||
      (guiManager_ && displayMode == GBufferViewMode::SurfaceNormals &&
       p.pipeline.pipelines.surfaceNormalLine != VK_NULL_HANDLE);
  if (showSurfaceNormalLines) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.surfaceNormalLine);
    if (p.debug.wireframeRasterModeSupported && guiManager_) {
      const auto& nv = guiManager_->normalValidationSettings();
      const float lw = p.debug.wireframeWideLinesSupported ? nv.lineWidth : 1.0f;
      vkCmdSetLineWidth(cmd, lw);
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.pipeline.layouts.surfaceNormal, 0, 1, &p.descriptors.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType);
    debugOverlay_.recordSurfaceNormals(cmd, p.pipeline.layouts.surfaceNormal,
                                       *p.draws.opaqueDrawCommands, *p.draws.transparentDrawCommands,
                                       guiManager_->normalValidationSettings(),
                                       *p.pushConstants.surfaceNormal);
  }

  if (wireframeOverlayMode && activeWireframePipeline != VK_NULL_HANDLE) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.pipeline.layouts.wireframe, 0, 1, &p.descriptors.sceneDescriptorSet, 0, nullptr);
    bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice, p.scene.indexType);
    drawWireframeCommands(p.draws.opaqueSingleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.draws.transparentSingleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.draws.opaqueWindingFlippedDrawCommands,
                          activeWireframeFrontCullPipeline);
    drawWireframeCommands(p.draws.transparentWindingFlippedDrawCommands,
                          activeWireframeFrontCullPipeline);
    drawWireframeCommands(p.draws.opaqueDoubleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(p.draws.transparentDoubleSidedDrawCommands,
                          activeWireframePipeline);
  }

  if (guiManager_ && guiManager_->showLightGizmos() &&
      p.pipeline.pipelines.lightGizmo != VK_NULL_HANDLE && lightingManager_) {
    lightingManager_->drawLightGizmos(cmd, lightingDescriptorSets,
                                      p.pipeline.pipelines.lightGizmo, p.pipeline.layouts.lighting, camera_);
  }

  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordPostProcessPass(
    VkCommandBuffer cmd, const FrameRecordParams& p,
    const std::array<VkDescriptorSet, 2>& postProcessSets) const {
  using container::ui::GBufferViewMode;

  if (!p.swapchain.swapChainFramebuffers || p.runtime.imageIndex >= p.swapchain.swapChainFramebuffers->size()) {
    throw std::runtime_error("invalid swapChainFramebuffers in FrameRecordParams");
  }

  VkRenderPassBeginInfo info{};
  info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass        = p.renderPasses.postProcess;
  info.framebuffer       = (*p.swapchain.swapChainFramebuffers)[p.runtime.imageIndex];
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = swapChainManager_.extent();
  VkClearValue clearVal{};
  clearVal.color       = {{0.0f, 0.0f, 0.0f, 1.0f}};
  info.clearValueCount = 1;
  info.pClearValues    = &clearVal;

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.pipelines.postProcess);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.layouts.postProcess, 0,
                          static_cast<uint32_t>(postProcessSets.size()),
                          postProcessSets.data(), 0, nullptr);
  setViewportAndScissor(cmd);

  const GBufferViewMode displayMode = currentDisplayMode(guiManager_);
  PostProcessPushConstants ppPc{};
  ppPc.outputMode = static_cast<uint32_t>(displayMode);
  const bool bloomPassActive =
      displayModeRecordsBloom(displayMode) && isPassActive(RenderPassId::Bloom);
  const bool tileCullActive = displayModeRecordsTileCull(displayMode) &&
                              isPassActive(RenderPassId::TileCull) && lightingManager_ &&
                              lightingManager_->isTiledLightingReady();
  ppPc.bloomEnabled = (bloomManager_ && bloomManager_->isReady() &&
                       bloomManager_->enabled() && bloomPassActive)
                          ? 1u
                          : 0u;
  ppPc.bloomIntensity = bloomManager_ ? bloomManager_->intensity() : 0.0f;
  const container::gpu::ExposureSettings exposureSettings =
      sanitizeExposureSettings(p.postProcess.exposureSettings);
  ppPc.exposure = exposureManager_
                      ? exposureManager_->resolvedExposure(exposureSettings)
                      : resolvePostProcessExposure(exposureSettings);
  ppPc.exposureMode = exposureSettings.mode;
  ppPc.targetLuminance = exposureSettings.targetLuminance;
  ppPc.minExposure = exposureSettings.minExposure;
  ppPc.maxExposure = exposureSettings.maxExposure;
  ppPc.adaptationRate = exposureSettings.adaptationRate;
  ppPc.cameraNear = p.camera.nearPlane;
  ppPc.cameraFar  = p.camera.farPlane;
  if (p.shadows.shadowData &&
      (displayMode == GBufferViewMode::ShadowCascades ||
       displayMode == GBufferViewMode::ShadowTexelDensity)) {
    for (uint32_t i = 0; i < kShadowCascadeCount; ++i)
      ppPc.cascadeSplits[i] = p.shadows.shadowData->cascades[i].splitDepth;
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
  vkCmdPushConstants(cmd, p.pipeline.layouts.postProcess, VK_SHADER_STAGE_FRAGMENT_BIT,
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
