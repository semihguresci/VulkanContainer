#include "Container/renderer/FrameRecorder.h"
#include "Container/renderer/BimManager.h"
#include "Container/renderer/BloomManager.h"
#include "Container/renderer/DeferredRasterFrameState.h"
#include "Container/renderer/DeferredRasterLighting.h"
#include "Container/renderer/DeferredRasterPostProcess.h"
#include "Container/renderer/DeferredRasterTransformGizmo.h"
#include "Container/renderer/EnvironmentManager.h"
#include "Container/renderer/ExposureManager.h"
#include "Container/renderer/GpuCullManager.h"
#include "Container/renderer/LightingManager.h"
#include "Container/renderer/OitManager.h"
#include "Container/renderer/RenderPassGpuProfiler.h"
#include "Container/renderer/RendererTelemetry.h"
#include "Container/renderer/SceneController.h"
#include "Container/renderer/SceneViewport.h"
#include "Container/renderer/ScreenshotCaptureRecorder.h"
#include "Container/renderer/ShadowCullManager.h"
#include "Container/renderer/ShadowManager.h"
#include "Container/renderer/bim/BimDrawCompactionPlanner.h"
#include "Container/renderer/bim/BimLightingOverlayPlanner.h"
#include "Container/renderer/bim/BimPrimitivePassPlanner.h"
#include "Container/renderer/bim/BimSurfaceDrawRoutingPlanner.h"
#include "Container/renderer/shadow/ShadowCascadeDrawPlanner.h"
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
using container::gpu::kShadowCascadeCount;
using container::gpu::kShadowMapResolution;
using container::gpu::kTileSize;
using container::gpu::ShadowPushConstants;
using container::gpu::TiledLightingPushConstants;

namespace {

bool hasBimShadowGeometry(const FrameRecordParams &p) {
  const auto &bimScene = p.bim.scene;
  return p.bim.sceneDescriptorSet != VK_NULL_HANDLE &&
         bimScene.vertexSlice.buffer != VK_NULL_HANDLE &&
         bimScene.indexSlice.buffer != VK_NULL_HANDLE &&
         hasBimOpaqueDrawCommands(p.bim);
}

bool usesGpuFilteredBimMeshShadowPath(const FrameRecordParams &p) {
  return p.bim.opaqueMeshDrawsUseGpuVisibility &&
         p.services.bimManager != nullptr && hasOpaqueDrawCommands(p.bim.draws);
}

bool hasRenderableDrawCommands(const FrameDrawLists &draws) {
  return hasOpaqueDrawCommands(draws) || hasTransparentDrawCommands(draws);
}

bool hasSplitTransparentDrawCommands(const FrameDrawLists &draws) {
  return hasDrawCommands(draws.transparentSingleSidedDrawCommands) ||
         hasDrawCommands(draws.transparentWindingFlippedDrawCommands) ||
         hasDrawCommands(draws.transparentDoubleSidedDrawCommands);
}

DeferredLightingDisplayMode deferredLightingDisplayMode(
    container::ui::GBufferViewMode mode) {
  switch (mode) {
  case container::ui::GBufferViewMode::Lit:
    return DeferredLightingDisplayMode::Lit;
  case container::ui::GBufferViewMode::Transparency:
    return DeferredLightingDisplayMode::Transparency;
  case container::ui::GBufferViewMode::Revealage:
    return DeferredLightingDisplayMode::Revealage;
  case container::ui::GBufferViewMode::SurfaceNormals:
    return DeferredLightingDisplayMode::SurfaceNormals;
  case container::ui::GBufferViewMode::ObjectSpaceNormals:
    return DeferredLightingDisplayMode::ObjectSpaceNormals;
  default:
    return DeferredLightingDisplayMode::Other;
  }
}

DeferredLightingWireframeSettings deferredLightingWireframeSettings(
    const container::ui::WireframeSettings &settings) {
  return {.enabled = settings.enabled,
          .mode = settings.mode == container::ui::WireframeMode::Full
                      ? DeferredLightingWireframeMode::Full
                      : DeferredLightingWireframeMode::Overlay,
          .depthTest = settings.depthTest,
          .color = settings.color,
          .lineWidth = settings.lineWidth,
          .overlayIntensity = settings.overlayIntensity};
}

DeferredLightingFrameInputs deferredLightingFrameInputs(
    const FrameRecordParams &p,
    const container::ui::GuiManager *guiManager,
    bool tileCullPassActive,
    bool tiledLightingReady,
    uint32_t pointLightCount) {
  DeferredLightingFrameInputs inputs{};
  inputs.displayMode = deferredLightingDisplayMode(currentDisplayMode(guiManager));
  inputs.guiAvailable = guiManager != nullptr;
  inputs.wireframeSupported =
      guiManager != nullptr && guiManager->wireframeSupported();
  inputs.wireframeWideLinesSupported = p.debug.wireframeWideLinesSupported;
  if (guiManager != nullptr) {
    inputs.wireframeSettings =
        deferredLightingWireframeSettings(guiManager->wireframeSettings());
    inputs.normalValidationSettings = guiManager->normalValidationSettings();
    inputs.geometryOverlayRequested = guiManager->showGeometryOverlay();
    inputs.lightGizmosRequested = guiManager->showLightGizmos();
  }
  inputs.debugDirectionalOnly = p.debug.debugDirectionalOnly;
  inputs.debugVisualizePointLightStencil =
      p.debug.debugVisualizePointLightStencil;
  inputs.tileCullPassActive = tileCullPassActive;
  inputs.tiledLightingReady = tiledLightingReady;
  inputs.depthSamplingReady =
      p.runtime.frame != nullptr &&
      p.runtime.frame->depthSamplingView != VK_NULL_HANDLE;
  inputs.tiledDescriptorSetReady =
      p.descriptors.tiledDescriptorSet != VK_NULL_HANDLE;
  inputs.transparentDrawCommandsAvailable = hasTransparentDrawCommands(p);
  inputs.pointLightCount = pointLightCount;
  inputs.pipelines = {
      .directionalLight =
          p.pipeline.pipelines.directionalLight != VK_NULL_HANDLE,
      .wireframeDepth = p.pipeline.pipelines.wireframeDepth != VK_NULL_HANDLE,
      .wireframeNoDepth =
          p.pipeline.pipelines.wireframeNoDepth != VK_NULL_HANDLE,
      .objectNormalDebug =
          p.pipeline.pipelines.objectNormalDebug != VK_NULL_HANDLE,
      .normalValidation =
          p.pipeline.pipelines.normalValidation != VK_NULL_HANDLE,
      .surfaceNormalLine =
          p.pipeline.pipelines.surfaceNormalLine != VK_NULL_HANDLE,
      .geometryDebug = p.pipeline.pipelines.geometryDebug != VK_NULL_HANDLE,
      .tiledPointLight =
          p.pipeline.pipelines.tiledPointLight != VK_NULL_HANDLE,
      .pointLight = p.pipeline.pipelines.pointLight != VK_NULL_HANDLE,
      .pointLightStencilDebug =
          p.pipeline.pipelines.pointLightStencilDebug != VK_NULL_HANDLE,
      .stencilVolume = p.pipeline.pipelines.stencilVolume != VK_NULL_HANDLE,
      .lightGizmo = p.pipeline.pipelines.lightGizmo != VK_NULL_HANDLE};
  return inputs;
}

bool hasBimRenderableGeometry(const FrameRecordParams &p) {
  const auto &bimScene = p.bim.scene;
  bool hasRenderable = false;
  for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
    hasRenderable = hasRenderable || hasRenderableDrawCommands(*draws);
  }
  return p.bim.sceneDescriptorSet != VK_NULL_HANDLE &&
         bimScene.vertexSlice.buffer != VK_NULL_HANDLE &&
         bimScene.indexSlice.buffer != VK_NULL_HANDLE && hasRenderable;
}

BimPrimitivePassDrawLists primitivePassDrawLists(const FrameDrawLists &draws) {
  return {
      .opaqueDrawCommands = draws.opaqueDrawCommands,
      .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
      .opaqueWindingFlippedDrawCommands =
          draws.opaqueWindingFlippedDrawCommands,
      .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
      .transparentDrawCommands = draws.transparentDrawCommands,
      .transparentSingleSidedDrawCommands =
          draws.transparentSingleSidedDrawCommands,
      .transparentWindingFlippedDrawCommands =
          draws.transparentWindingFlippedDrawCommands,
      .transparentDoubleSidedDrawCommands =
          draws.transparentDoubleSidedDrawCommands,
  };
}

BimSurfaceDrawLists surfaceDrawLists(const FrameDrawLists &draws) {
  return {
      .opaqueDrawCommands = draws.opaqueDrawCommands,
      .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
      .opaqueWindingFlippedDrawCommands =
          draws.opaqueWindingFlippedDrawCommands,
      .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
      .transparentDrawCommands = draws.transparentDrawCommands,
      .transparentSingleSidedDrawCommands =
          draws.transparentSingleSidedDrawCommands,
      .transparentWindingFlippedDrawCommands =
          draws.transparentWindingFlippedDrawCommands,
      .transparentDoubleSidedDrawCommands =
          draws.transparentDoubleSidedDrawCommands,
  };
}

BimLightingOverlayDrawLists
bimLightingOverlayDrawLists(const FrameDrawLists &draws) {
  return {
      .opaqueSingleSidedDrawCommands = draws.opaqueSingleSidedDrawCommands,
      .transparentSingleSidedDrawCommands =
          draws.transparentSingleSidedDrawCommands,
      .opaqueWindingFlippedDrawCommands =
          draws.opaqueWindingFlippedDrawCommands,
      .transparentWindingFlippedDrawCommands =
          draws.transparentWindingFlippedDrawCommands,
      .opaqueDoubleSidedDrawCommands = draws.opaqueDoubleSidedDrawCommands,
      .transparentDoubleSidedDrawCommands =
          draws.transparentDoubleSidedDrawCommands,
  };
}

BimLightingOverlayInputs
bimLightingOverlayInputs(const FrameRecordParams &p, VkExtent2D extent) {
  BimLightingOverlayInputs inputs{};
  inputs.bimGeometryReady = hasBimRenderableGeometry(p);
  inputs.wireframeLayoutReady = p.pipeline.layouts.wireframe != VK_NULL_HANDLE;
  inputs.wireframePushConstantsReady = p.pushConstants.wireframe != nullptr;
  inputs.wideLinesSupported = p.debug.wireframeWideLinesSupported;
  inputs.framebufferWidth = extent.width;
  inputs.framebufferHeight = extent.height;
  inputs.pipelines = {
      .wireframeDepth = p.pipeline.pipelines.wireframeDepth != VK_NULL_HANDLE,
      .wireframeNoDepth =
          p.pipeline.pipelines.wireframeNoDepth != VK_NULL_HANDLE,
      .bimFloorPlanDepth =
          p.pipeline.pipelines.bimFloorPlanDepth != VK_NULL_HANDLE,
      .bimFloorPlanNoDepth =
          p.pipeline.pipelines.bimFloorPlanNoDepth != VK_NULL_HANDLE,
      .bimPointCloudDepth =
          p.pipeline.pipelines.bimPointCloudDepth != VK_NULL_HANDLE,
      .bimCurveDepth = p.pipeline.pipelines.bimCurveDepth != VK_NULL_HANDLE,
      .selectionMask = p.pipeline.pipelines.selectionMask != VK_NULL_HANDLE,
      .selectionOutline =
          p.pipeline.pipelines.selectionOutline != VK_NULL_HANDLE,
  };

  inputs.points = {.enabled = p.bim.pointCurveStyle.points.enabled,
                   .depthTest = p.bim.pointCurveStyle.points.depthTest,
                   .color = p.bim.pointCurveStyle.points.color,
                   .opacity = p.bim.pointCurveStyle.points.opacity,
                   .lineWidth = p.bim.pointCurveStyle.points.pointSize,
                   .draws = bimLightingOverlayDrawLists(p.bim.pointDraws)};
  inputs.curves = {.enabled = p.bim.pointCurveStyle.curves.enabled,
                   .depthTest = p.bim.pointCurveStyle.curves.depthTest,
                   .color = p.bim.pointCurveStyle.curves.color,
                   .opacity = p.bim.pointCurveStyle.curves.opacity,
                   .lineWidth = p.bim.pointCurveStyle.curves.lineWidth,
                   .draws = bimLightingOverlayDrawLists(p.bim.curveDraws)};
  inputs.floorPlan = {.enabled = p.bim.floorPlan.enabled,
                      .depthTest = p.bim.floorPlan.depthTest,
                      .color = p.bim.floorPlan.color,
                      .opacity = p.bim.floorPlan.opacity,
                      .lineWidth = p.bim.floorPlan.lineWidth,
                      .commands = p.bim.floorPlanDrawCommands};
  inputs.sceneHoverCommands = p.draws.hoveredDrawCommands;
  inputs.bimHoverCommands = p.bim.draws.hoveredDrawCommands;
  inputs.sceneSelectionCommands = p.draws.selectedDrawCommands;
  inputs.bimSelectionCommands = p.bim.draws.selectedDrawCommands;
  inputs.nativePointHoverCommands = p.bim.nativePointDraws.hoveredDrawCommands;
  inputs.nativeCurveHoverCommands = p.bim.nativeCurveDraws.hoveredDrawCommands;
  inputs.nativePointSelectionCommands =
      p.bim.nativePointDraws.selectedDrawCommands;
  inputs.nativeCurveSelectionCommands =
      p.bim.nativeCurveDraws.selectedDrawCommands;
  inputs.nativePointSize = p.bim.primitivePasses.pointCloud.pointSize;
  inputs.nativeCurveLineWidth = p.bim.primitivePasses.curves.lineWidth;
  return inputs;
}

bool hasBimPrimitivePassGeometry(const FrameRecordParams &p) {
  return p.bim.sceneDescriptorSet != VK_NULL_HANDLE &&
         p.bim.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
         p.bim.scene.indexSlice.buffer != VK_NULL_HANDLE;
}

bool hasBimSectionClipCapGeometry(const FrameRecordParams &p) {
  return p.bim.sceneDescriptorSet != VK_NULL_HANDLE &&
         p.bim.sectionClipCapGeometry.valid();
}

bool sectionClipCapStyleActive(const FrameSectionClipCapStyleState &style) {
  return style.enabled && (style.fillEnabled || style.hatchEnabled);
}

const std::vector<DrawCommand> *
primaryOpaqueDrawCommands(const FrameDrawLists &draws) {
  return hasSplitOpaqueDrawCommands(draws) ? draws.opaqueSingleSidedDrawCommands
                                           : draws.opaqueDrawCommands;
}

const std::vector<DrawCommand> *
primaryTransparentDrawCommands(const FrameDrawLists &draws) {
  return hasSplitTransparentDrawCommands(draws)
             ? draws.transparentSingleSidedDrawCommands
             : draws.transparentDrawCommands;
}

ShadowCascadeSurfaceDrawLists
sceneShadowCascadeDrawLists(const FrameDrawLists &draws) {
  return {
      .singleSided = draws.opaqueSingleSidedDrawCommands,
      .windingFlipped = draws.opaqueWindingFlippedDrawCommands,
      .doubleSided = draws.opaqueDoubleSidedDrawCommands,
  };
}

ShadowCascadeSurfaceDrawLists
bimShadowCascadeDrawLists(const FrameDrawLists &draws,
                          bool cpuFallbackAllowed) {
  return {
      .singleSided = primaryOpaqueDrawCommands(draws),
      .windingFlipped = draws.opaqueWindingFlippedDrawCommands,
      .doubleSided = draws.opaqueDoubleSidedDrawCommands,
      .cpuFallbackAllowed = cpuFallbackAllowed,
  };
}

VkPipeline choosePipeline(VkPipeline preferred, VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

VkPipeline pipelineForBimSurfaceRoute(const BimSurfaceDrawRoute &route,
                                      VkPipeline singleSidedPipeline,
                                      VkPipeline windingFlippedPipeline,
                                      VkPipeline doubleSidedPipeline) {
  switch (route.kind) {
  case BimSurfaceDrawRouteKind::SingleSided:
    return singleSidedPipeline;
  case BimSurfaceDrawRouteKind::WindingFlipped:
    return windingFlippedPipeline;
  case BimSurfaceDrawRouteKind::DoubleSided:
    return doubleSidedPipeline;
  }
  return singleSidedPipeline;
}

VkPipeline
pipelineForBimLightingOverlay(const BimLightingOverlayPipeline pipeline,
                              const FrameRecordParams &p) {
  switch (pipeline) {
  case BimLightingOverlayPipeline::WireframeDepth:
    return p.pipeline.pipelines.wireframeDepth;
  case BimLightingOverlayPipeline::WireframeNoDepth:
    return p.pipeline.pipelines.wireframeNoDepth;
  case BimLightingOverlayPipeline::WireframeDepthFrontCull:
    return choosePipeline(p.pipeline.pipelines.wireframeDepthFrontCull,
                          p.pipeline.pipelines.wireframeDepth);
  case BimLightingOverlayPipeline::WireframeNoDepthFrontCull:
    return choosePipeline(p.pipeline.pipelines.wireframeNoDepthFrontCull,
                          p.pipeline.pipelines.wireframeNoDepth);
  case BimLightingOverlayPipeline::BimFloorPlanDepth:
    return p.pipeline.pipelines.bimFloorPlanDepth;
  case BimLightingOverlayPipeline::BimFloorPlanNoDepth:
    return p.pipeline.pipelines.bimFloorPlanNoDepth;
  case BimLightingOverlayPipeline::BimPointCloudDepth:
    return p.pipeline.pipelines.bimPointCloudDepth;
  case BimLightingOverlayPipeline::BimCurveDepth:
    return p.pipeline.pipelines.bimCurveDepth;
  }
  return VK_NULL_HANDLE;
}

constexpr uint32_t kIndirectObjectIndex = std::numeric_limits<uint32_t>::max();
constexpr size_t kMinParallelShadowCascadeCpuCommands = 512;

void syncOverlaySectionPlanePushConstants(
    const FramePushConstantState &pushConstants) {
  const uint32_t enabled = pushConstants.bindless != nullptr
                               ? pushConstants.bindless->sectionPlaneEnabled
                               : 0u;
  const glm::vec4 plane = pushConstants.bindless != nullptr
                              ? pushConstants.bindless->sectionPlane
                              : glm::vec4{0.0f, 1.0f, 0.0f, 0.0f};

  if (pushConstants.wireframe != nullptr) {
    pushConstants.wireframe->sectionPlaneEnabled = enabled;
    pushConstants.wireframe->sectionPlane = plane;
  }
  if (pushConstants.normalValidation != nullptr) {
    pushConstants.normalValidation->sectionPlaneEnabled = enabled;
    pushConstants.normalValidation->sectionPlane = plane;
  }
  if (pushConstants.surfaceNormal != nullptr) {
    pushConstants.surfaceNormal->sectionPlaneEnabled = enabled;
    pushConstants.surfaceNormal->sectionPlane = plane;
  }
}

uint32_t drawInstanceCount(const DrawCommand &command) {
  return std::max(command.instanceCount, 1u);
}

void pushSceneObjectIndex(VkCommandBuffer cmd, VkPipelineLayout layout,
                          BindlessPushConstants &pc, uint32_t objectIndex) {
  pc.objectIndex = objectIndex;
  vkCmdPushConstants(cmd, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(BindlessPushConstants), &pc);
}

} // namespace

FrameRecorder::FrameRecorder(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::SwapChainManager &swapChainManager,
    const OitManager &oitManager, const LightingManager *lightingManager,
    const EnvironmentManager *environmentManager,
    const SceneController *sceneController, GpuCullManager *gpuCullManager,
    BloomManager *bloomManager, ExposureManager *exposureManager,
    const container::scene::BaseCamera *camera,
    container::ui::GuiManager *guiManager)
    : device_(std::move(device)), swapChainManager_(swapChainManager),
      oitManager_(oitManager), lightingManager_(lightingManager),
      environmentManager_(environmentManager),
      sceneController_(sceneController), gpuCullManager_(gpuCullManager),
      bloomManager_(bloomManager), exposureManager_(exposureManager),
      camera_(camera), guiManager_(guiManager) {}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

void FrameRecorder::record(VkCommandBuffer commandBuffer,
                           const FrameRecordParams &p) const {
  if (commandBuffer == VK_NULL_HANDLE) {
    throw std::runtime_error(
        "FrameRecorder::record received a null command buffer");
  }
  if (!p.runtime.frame) {
    throw std::runtime_error("FrameRecordParams::runtime.frame is null");
  }

  if (gpuCullManager_) {
    gpuCullManager_->beginFrameCulling();
  }

  graph_.prepareFrame(p);

  if (displayModeRecordsShadowAtlas(currentDisplayMode(guiManager_)) &&
      p.shadows.useGpuShadowCull && p.shadows.shadowCullManager != nullptr &&
      p.shadows.shadowCullManager->isReady() &&
      p.draws.opaqueSingleSidedDrawCommands != nullptr) {
    // Shadow culling is per-cascade, but all cascades consume the same source
    // draw list. Upload once before graph execution so each cascade pass can
    // filter into its own indirect buffer.
    p.shadows.shadowCullManager->ensureBufferCapacity(
        static_cast<uint32_t>(p.draws.opaqueSingleSidedDrawCommands->size()));
    p.shadows.shadowCullManager->uploadDrawCommands(
        *p.draws.opaqueSingleSidedDrawCommands);
  }

  if (shouldPrepareShadowCascadeDrawCommands(p)) {
    prepareShadowCascadeDrawCommands(p);
  } else {
    for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
         ++cascadeIndex) {
      shadowCascadeSingleSidedDrawCommands_[cascadeIndex].clear();
      shadowCascadeWindingFlippedDrawCommands_[cascadeIndex].clear();
      shadowCascadeDoubleSidedDrawCommands_[cascadeIndex].clear();
      bimShadowCascadeSingleSidedDrawCommands_[cascadeIndex].clear();
      bimShadowCascadeWindingFlippedDrawCommands_[cascadeIndex].clear();
      bimShadowCascadeDoubleSidedDrawCommands_[cascadeIndex].clear();
    }
    shadowCascadeDrawCommandCacheValid_ = false;
  }
  recordShadowCascadeSecondaryCommandBuffers(p);

  if (BimManager *bimManager = p.services.bimManager) {
    const auto compactionPlan = buildBimDrawCompactionPlan(
        makeBimDrawCompactionPlanInputs(*bimManager));
    for (const BimDrawCompactionPlanSource &source : compactionPlan) {
      bimManager->prepareDrawCompaction(source.slot, *source.commands);
    }
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    throw std::runtime_error("failed to begin recording command buffer!");
  }

  if (lightingManager_) {
    lightingManager_->resetGpuTimers(commandBuffer, p.runtime.imageIndex);
  }
  if (p.services.bimManager) {
    p.services.bimManager->recordMeshletResidencyUpdate(
        commandBuffer, p.camera.cameraBuffer, p.camera.cameraBufferSize,
        p.bim.scene.objectBuffer, p.bim.scene.objectBufferSize);
    p.services.bimManager->recordVisibilityFilterUpdate(commandBuffer);
    p.services.bimManager->recordDrawCompactionUpdate(commandBuffer);
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
    graph_.executePreparedFrame(commandBuffer, p, hooks);
  } else {
    graph_.executePreparedFrame(commandBuffer, p);
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
  for (const auto &status : graph_.lastFrameExecutionStatuses()) {
    if (status.id == id) {
      return status.active;
    }
  }
  return graph_.isPassActive(id);
}

bool FrameRecorder::shouldPrepareShadowCascadeDrawCommands(
    const FrameRecordParams &p) const {
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
    if (hasBimShadowGeometry(p)) {
      return true;
    }
  }
  return false;
}

bool FrameRecorder::useGpuShadowCullForCascade(const FrameRecordParams &p,
                                               uint32_t cascadeIndex) const {
  const auto shadowCullIds = shadowCullPassIds();
  const bool shadowCullPassActive = cascadeIndex < shadowCullIds.size() &&
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

  return p.shadows.useGpuShadowCull && shadowCullPassActive &&
         p.shadows.shadowCullManager != nullptr &&
         p.shadows.shadowCullManager->isReady() &&
         p.draws.opaqueSingleSidedDrawCommands != nullptr &&
         !p.draws.opaqueSingleSidedDrawCommands->empty() &&
         cascadeIndex < container::gpu::kShadowCascadeCount &&
         gpuShadowIndirectBuffer != VK_NULL_HANDLE &&
         gpuShadowCountBuffer != VK_NULL_HANDLE && gpuShadowMaxDrawCount > 0;
}

size_t
FrameRecorder::shadowCascadeCpuCommandCount(const FrameRecordParams &p,
                                            uint32_t cascadeIndex) const {
  if (cascadeIndex >= kShadowCascadeCount) {
    return 0u;
  }

  size_t count =
      shadowCascadeWindingFlippedDrawCommands_[cascadeIndex].size() +
      shadowCascadeDoubleSidedDrawCommands_[cascadeIndex].size() +
      bimShadowCascadeSingleSidedDrawCommands_[cascadeIndex].size() +
      bimShadowCascadeWindingFlippedDrawCommands_[cascadeIndex].size() +
      bimShadowCascadeDoubleSidedDrawCommands_[cascadeIndex].size();
  if (!useGpuShadowCullForCascade(p, cascadeIndex)) {
    count += shadowCascadeSingleSidedDrawCommands_[cascadeIndex].size();
  }
  return count;
}

ShadowCascadeDrawPlannerInputs FrameRecorder::shadowCascadeDrawPlannerInputs(
    const FrameRecordParams &p) const {
  ShadowCascadeDrawPlannerInputs inputs{};
  inputs.scene = {.objectData = p.scene.objectData,
                  .objectDataRevision = p.scene.objectDataRevision};
  inputs.bimScene = {.objectData = p.bim.scene.objectData,
                     .objectDataRevision = p.bim.scene.objectDataRevision};
  inputs.sceneDraws = sceneShadowCascadeDrawLists(p.draws);
  inputs.hasBimShadowGeometry = hasBimShadowGeometry(p);
  inputs.useGpuShadowCull = p.shadows.useGpuShadowCull;
  inputs.shadowManagerIdentity = p.shadows.shadowManager;
  inputs.shadowData = p.shadows.shadowData;

  const auto shadowPassIds = shadowCascadePassIds();
  const auto shadowCullIds = shadowCullPassIds();
  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    inputs.shadowPassActive[cascadeIndex] =
        cascadeIndex < shadowPassIds.size() &&
        isPassActive(shadowPassIds[cascadeIndex]);
    inputs.shadowCullPassActive[cascadeIndex] =
        cascadeIndex < shadowCullIds.size() &&
        isPassActive(shadowCullIds[cascadeIndex]);
    inputs.sceneSingleSidedUsesGpuCull[cascadeIndex] =
        useGpuShadowCullForCascade(p, cascadeIndex);
  }

  uint32_t bimDrawListCount = 0u;
  for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
    if (bimDrawListCount >= inputs.bimDraws.size()) {
      break;
    }
    inputs.bimDraws[bimDrawListCount] = bimShadowCascadeDrawLists(
        *draws,
        !(draws == &p.bim.draws && p.bim.opaqueMeshDrawsUseGpuVisibility));
    ++bimDrawListCount;
  }
  inputs.bimDrawListCount = bimDrawListCount;

  if (p.shadows.shadowManager != nullptr) {
    inputs.cascadeIntersectsSphere = [shadowManager = p.shadows.shadowManager](
                                         uint32_t cascadeIndex,
                                         const glm::vec4 &boundingSphere) {
      return shadowManager->cascadeIntersectsSphere(cascadeIndex,
                                                    boundingSphere);
    };
  }
  return inputs;
}

uint64_t FrameRecorder::shadowCascadeDrawCommandSignature(
    const FrameRecordParams &p) const {
  return computeShadowCascadeDrawSignature(shadowCascadeDrawPlannerInputs(p));
}

void FrameRecorder::prepareShadowCascadeDrawCommands(
    const FrameRecordParams &p) const {
  const ShadowCascadeDrawPlannerInputs inputs =
      shadowCascadeDrawPlannerInputs(p);
  const ShadowCascadeDrawPlanner planner(inputs);
  const uint64_t signature = planner.signature();
  if (shadowCascadeDrawCommandCacheValid_ &&
      shadowCascadeDrawCommandSignature_ == signature) {
    return;
  }

  const ShadowCascadeDrawPlan plan = planner.build();
  shadowCascadeSingleSidedDrawCommands_ = plan.sceneSingleSided;
  shadowCascadeWindingFlippedDrawCommands_ = plan.sceneWindingFlipped;
  shadowCascadeDoubleSidedDrawCommands_ = plan.sceneDoubleSided;
  bimShadowCascadeSingleSidedDrawCommands_ = plan.bimSingleSided;
  bimShadowCascadeWindingFlippedDrawCommands_ = plan.bimWindingFlipped;
  bimShadowCascadeDoubleSidedDrawCommands_ = plan.bimDoubleSided;
  shadowCascadeDrawCommandSignature_ = plan.signature;
  shadowCascadeDrawCommandCacheValid_ = true;
}

void FrameRecorder::bindSceneGeometryBuffers(VkCommandBuffer cmd,
                                             container::gpu::BufferSlice vertex,
                                             container::gpu::BufferSlice index,
                                             VkIndexType indexType) const {
  if (vertex.buffer == VK_NULL_HANDLE || index.buffer == VK_NULL_HANDLE)
    return;
  VkBuffer vb[] = {vertex.buffer};
  VkDeviceSize off[] = {vertex.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, vb, off);
  vkCmdBindIndexBuffer(cmd, index.buffer, index.offset, indexType);
}

void FrameRecorder::drawDiagnosticCube(VkCommandBuffer cmd,
                                       VkPipelineLayout layout,
                                       uint32_t diagCubeObjectIndex,
                                       BindlessPushConstants &pc) const {
  if (!sceneController_)
    return;
  if (diagCubeObjectIndex == std::numeric_limits<uint32_t>::max() ||
      sceneController_->diagCubeVertexSlice().buffer == VK_NULL_HANDLE)
    return;
  const auto diagVtx = sceneController_->diagCubeVertexSlice();
  const auto diagIdx = sceneController_->diagCubeIndexSlice();
  VkBuffer diagVB[] = {diagVtx.buffer};
  VkDeviceSize diagOff[] = {diagVtx.offset};
  vkCmdBindVertexBuffers(cmd, 0, 1, diagVB, diagOff);
  vkCmdBindIndexBuffer(cmd, diagIdx.buffer, diagIdx.offset,
                       VK_INDEX_TYPE_UINT32);
  pc.objectIndex = diagCubeObjectIndex;
  vkCmdPushConstants(cmd, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(BindlessPushConstants), &pc);
  vkCmdDrawIndexed(cmd, sceneController_->diagCubeIndexCount(), 1, 0, 0,
                   diagCubeObjectIndex);
}

void FrameRecorder::recordDepthPrepass(VkCommandBuffer cmd,
                                       const FrameRecordParams &p,
                                       VkDescriptorSet sceneSet) const {
  VkRenderPassBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = p.renderPasses.depthPrepass;
  info.framebuffer = p.runtime.frame->depthPrepassFramebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = swapChainManager_.extent();
  VkClearValue clearVal{};
  clearVal.depthStencil = {0.0f, 0};
  info.clearValueCount = 1;
  info.pClearValues = &clearVal;

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.layouts.scene, 0, 1, &sceneSet, 0,
                          nullptr);
  recordSceneViewportAndScissor(cmd, swapChainManager_.extent());
  bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice,
                           p.scene.indexType);
  const bool frustumCullActive = isPassActive(RenderPassId::FrustumCull);

  const VkPipeline depthFrontCullPipeline =
      choosePipeline(p.pipeline.pipelines.depthPrepassFrontCull,
                     p.pipeline.pipelines.depthPrepass);
  const VkPipeline depthNoCullPipeline =
      choosePipeline(p.pipeline.pipelines.depthPrepassNoCull,
                     p.pipeline.pipelines.depthPrepass);

  // Single-sided opaque draws can use the GPU-produced indirect list. Double-
  // sided/alpha-special cases stay on explicit draw lists so pipeline variants
  // and material flags remain easy to reason about.
  if (gpuCullManager_ && gpuCullManager_->isReady() && frustumCullActive &&
      gpuCullManager_->frustumDrawsValid() &&
      p.draws.opaqueSingleSidedDrawCommands &&
      !p.draws.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.depthPrepass);
    pushSceneObjectIndex(cmd, p.pipeline.layouts.scene,
                         *p.pushConstants.bindless, kIndirectObjectIndex);
    gpuCullManager_->drawIndirect(cmd);
  } else if (p.draws.opaqueSingleSidedDrawCommands &&
             !p.draws.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.depthPrepass);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                            *p.draws.opaqueSingleSidedDrawCommands,
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      depthNoCullPipeline);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                            *p.draws.opaqueDoubleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    p.pipeline.pipelines.depthPrepass);
  drawDiagnosticCube(cmd, p.pipeline.layouts.scene, p.scene.diagCubeObjectIndex,
                     *p.pushConstants.bindless);
  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordGBufferPass(VkCommandBuffer cmd,
                                      const FrameRecordParams &p,
                                      VkDescriptorSet sceneSet) const {
  VkRenderPassBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = p.renderPasses.gBuffer;
  info.framebuffer = p.runtime.frame->gBufferFramebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = swapChainManager_.extent();
  std::array<VkClearValue, 6> clearValues{};
  clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  clearValues[1].color = {{0.5f, 0.5f, 1.0f, 1.0f}};
  clearValues[2].color = {{0.0f, 1.0f, 1.0f, 1.0f}};
  clearValues[3].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  clearValues[4].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  clearValues[5].color = {{0u, 0u, 0u, 0u}};
  info.clearValueCount = static_cast<uint32_t>(clearValues.size());
  info.pClearValues = clearValues.data();

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.layouts.scene, 0, 1, &sceneSet, 0,
                          nullptr);
  recordSceneViewportAndScissor(cmd, swapChainManager_.extent());
  bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice,
                           p.scene.indexType);
  const bool frustumCullActive = isPassActive(RenderPassId::FrustumCull);

  const VkPipeline gBufferFrontCullPipeline = choosePipeline(
      p.pipeline.pipelines.gBufferFrontCull, p.pipeline.pipelines.gBuffer);
  const VkPipeline gBufferNoCullPipeline = choosePipeline(
      p.pipeline.pipelines.gBufferNoCull, p.pipeline.pipelines.gBuffer);

  // The same-frame Hi-Z pyramid contains this geometry from the depth prepass,
  // so consuming the occlusion list for G-buffer visibility can self-occlude
  // wall reliefs and thin fixtures. G-buffer therefore fails open to the
  // frustum list, with CPU draws as the final fallback.
  if (gpuCullManager_ && gpuCullManager_->isReady() && frustumCullActive &&
      gpuCullManager_->frustumDrawsValid() &&
      p.draws.opaqueSingleSidedDrawCommands &&
      !p.draws.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.gBuffer);
    pushSceneObjectIndex(cmd, p.pipeline.layouts.scene,
                         *p.pushConstants.bindless, kIndirectObjectIndex);
    gpuCullManager_->drawIndirect(cmd);
  } else if (p.draws.opaqueSingleSidedDrawCommands &&
             !p.draws.opaqueSingleSidedDrawCommands->empty()) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.gBuffer);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                            *p.draws.opaqueSingleSidedDrawCommands,
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      gBufferNoCullPipeline);
    debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                            *p.draws.opaqueDoubleSidedDrawCommands,
                            *p.pushConstants.bindless);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    p.pipeline.pipelines.gBuffer);
  drawDiagnosticCube(cmd, p.pipeline.layouts.scene, p.scene.diagCubeObjectIndex,
                     *p.pushConstants.bindless);
  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordBimDepthPrepass(VkCommandBuffer cmd,
                                          const FrameRecordParams &p) const {
  const auto &bimScene = p.bim.scene;
  if (!p.runtime.frame || p.renderPasses.bimDepthPrepass == VK_NULL_HANDLE ||
      p.runtime.frame->bimDepthPrepassFramebuffer == VK_NULL_HANDLE ||
      p.bim.sceneDescriptorSet == VK_NULL_HANDLE ||
      p.pushConstants.bindless == nullptr ||
      bimScene.vertexSlice.buffer == VK_NULL_HANDLE ||
      bimScene.indexSlice.buffer == VK_NULL_HANDLE ||
      !hasBimOpaqueDrawCommands(p.bim)) {
    return;
  }

  const VkPipeline depthPipeline = choosePipeline(
      p.pipeline.pipelines.bimDepthPrepass, p.pipeline.pipelines.depthPrepass);
  if (depthPipeline == VK_NULL_HANDLE) {
    return;
  }

  VkRenderPassBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = p.renderPasses.bimDepthPrepass;
  info.framebuffer = p.runtime.frame->bimDepthPrepassFramebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = swapChainManager_.extent();

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.layouts.scene, 0, 1,
                          &p.bim.sceneDescriptorSet, 0, nullptr);
  recordSceneViewportAndScissor(cmd, swapChainManager_.extent());
  bindSceneGeometryBuffers(cmd, bimScene.vertexSlice, bimScene.indexSlice,
                           bimScene.indexType);

  const VkPipeline frontCullPipeline = choosePipeline(
      p.pipeline.pipelines.bimDepthPrepassFrontCull, depthPipeline);
  const VkPipeline noCullPipeline =
      choosePipeline(p.pipeline.pipelines.bimDepthPrepassNoCull, depthPipeline);

  const auto drawOpaqueBimLists = [&](const FrameDrawLists &draws) {
    const BimSurfaceDrawRoutingPlan routingPlan =
        buildBimSurfaceDrawRoutingPlan(
            {.kind = BimSurfaceDrawKind::Opaque,
             .draws = surfaceDrawLists(draws),
             .gpuCompactionEligible = &draws == &p.bim.draws,
             .gpuVisibilityOwnsCpuFallback =
                 &draws == &p.bim.draws &&
                 p.bim.opaqueMeshDrawsUseGpuVisibility});
    const auto drawGpuCompacted = [&](const BimSurfaceDrawRoute &route,
                                      VkPipeline pipeline,
                                      BindlessPushConstants pc) {
      return route.gpuCompactionAllowed && p.services.bimManager &&
             p.services.bimManager->drawCompactionReady(route.gpuSlot) &&
             pipeline != VK_NULL_HANDLE &&
             (vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline),
              pushSceneObjectIndex(cmd, p.pipeline.layouts.scene, pc,
                                   kIndirectObjectIndex),
              p.services.bimManager->drawCompacted(route.gpuSlot, cmd), true);
    };

    for (uint32_t routeIndex = 0; routeIndex < routingPlan.routeCount;
         ++routeIndex) {
      const BimSurfaceDrawRoute &route = routingPlan.routes[routeIndex];
      const VkPipeline pipeline = pipelineForBimSurfaceRoute(
          route, depthPipeline, frontCullPipeline, noCullPipeline);
      if (drawGpuCompacted(route, pipeline, *p.pushConstants.bindless)) {
        continue;
      }
      if (route.cpuFallbackAllowed && hasDrawCommands(route.cpuCommands)) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                *route.cpuCommands, *p.pushConstants.bindless);
      }
    }
  };
  for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
    drawOpaqueBimLists(*draws);
  }

  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordBimGBufferPass(VkCommandBuffer cmd,
                                         const FrameRecordParams &p) const {
  const auto &bimScene = p.bim.scene;
  if (!p.runtime.frame || p.renderPasses.bimGBuffer == VK_NULL_HANDLE ||
      p.runtime.frame->bimGBufferFramebuffer == VK_NULL_HANDLE ||
      p.bim.sceneDescriptorSet == VK_NULL_HANDLE ||
      p.pushConstants.bindless == nullptr ||
      bimScene.vertexSlice.buffer == VK_NULL_HANDLE ||
      bimScene.indexSlice.buffer == VK_NULL_HANDLE ||
      !hasBimOpaqueDrawCommands(p.bim)) {
    return;
  }

  const VkPipeline gBufferPipeline = choosePipeline(
      p.pipeline.pipelines.bimGBuffer, p.pipeline.pipelines.gBuffer);
  if (gBufferPipeline == VK_NULL_HANDLE) {
    return;
  }

  VkRenderPassBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = p.renderPasses.bimGBuffer;
  info.framebuffer = p.runtime.frame->bimGBufferFramebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = swapChainManager_.extent();

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.layouts.scene, 0, 1,
                          &p.bim.sceneDescriptorSet, 0, nullptr);
  recordSceneViewportAndScissor(cmd, swapChainManager_.extent());
  bindSceneGeometryBuffers(cmd, bimScene.vertexSlice, bimScene.indexSlice,
                           bimScene.indexType);

  const VkPipeline frontCullPipeline =
      choosePipeline(p.pipeline.pipelines.bimGBufferFrontCull, gBufferPipeline);
  const VkPipeline noCullPipeline =
      choosePipeline(p.pipeline.pipelines.bimGBufferNoCull, gBufferPipeline);

  BindlessPushConstants bimPc = *p.pushConstants.bindless;
  bimPc.semanticColorMode = p.bim.semanticColorMode;

  const auto drawOpaqueBimLists = [&](const FrameDrawLists &draws) {
    const BimSurfaceDrawRoutingPlan routingPlan =
        buildBimSurfaceDrawRoutingPlan(
            {.kind = BimSurfaceDrawKind::Opaque,
             .draws = surfaceDrawLists(draws),
             .gpuCompactionEligible = &draws == &p.bim.draws,
             .gpuVisibilityOwnsCpuFallback =
                 &draws == &p.bim.draws &&
                 p.bim.opaqueMeshDrawsUseGpuVisibility});
    const auto drawGpuCompacted = [&](const BimSurfaceDrawRoute &route,
                                      VkPipeline pipeline,
                                      BindlessPushConstants pc) {
      return route.gpuCompactionAllowed && p.services.bimManager &&
             p.services.bimManager->drawCompactionReady(route.gpuSlot) &&
             pipeline != VK_NULL_HANDLE &&
             (vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline),
              pushSceneObjectIndex(cmd, p.pipeline.layouts.scene, pc,
                                   kIndirectObjectIndex),
              p.services.bimManager->drawCompacted(route.gpuSlot, cmd), true);
    };

    for (uint32_t routeIndex = 0; routeIndex < routingPlan.routeCount;
         ++routeIndex) {
      const BimSurfaceDrawRoute &route = routingPlan.routes[routeIndex];
      const VkPipeline pipeline = pipelineForBimSurfaceRoute(
          route, gBufferPipeline, frontCullPipeline, noCullPipeline);
      if (drawGpuCompacted(route, pipeline, bimPc)) {
        continue;
      }
      if (route.cpuFallbackAllowed && hasDrawCommands(route.cpuCommands)) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                *route.cpuCommands, bimPc);
      }
    }
  };
  for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
    drawOpaqueBimLists(*draws);
  }

  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordTransparentPickPass(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  const VkExtent2D extent = swapChainManager_.extent();
  if (!p.runtime.frame || extent.width == 0u || extent.height == 0u ||
      p.renderPasses.transparentPick == VK_NULL_HANDLE ||
      p.runtime.frame->transparentPickFramebuffer == VK_NULL_HANDLE ||
      p.runtime.frame->depthStencil.image == VK_NULL_HANDLE ||
      p.runtime.frame->pickDepth.image == VK_NULL_HANDLE ||
      p.runtime.frame->pickId.image == VK_NULL_HANDLE ||
      p.pipeline.layouts.scene == VK_NULL_HANDLE ||
      p.pipeline.pipelines.transparentPick == VK_NULL_HANDLE ||
      p.pushConstants.bindless == nullptr) {
    return;
  }

  const auto canDrawScene = [](VkDescriptorSet descriptorSet,
                               const FrameSceneGeometry &scene,
                               const FrameDrawLists &draws) {
    return descriptorSet != VK_NULL_HANDLE &&
           scene.vertexSlice.buffer != VK_NULL_HANDLE &&
           scene.indexSlice.buffer != VK_NULL_HANDLE &&
           hasTransparentDrawCommands(draws);
  };

  const bool drawScene =
      canDrawScene(p.descriptors.sceneDescriptorSet, p.scene, p.draws);
  const bool drawBim = hasBimTransparentGeometry(p);
  if (!drawScene && !drawBim) {
    return;
  }

  constexpr VkImageAspectFlags kDepthStencilAspects =
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  constexpr VkPipelineStageFlags kDepthStages =
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

  auto makeDepthBarrier = [](VkImage image, VkImageLayout oldLayout,
                             VkImageLayout newLayout, VkAccessFlags srcAccess,
                             VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {kDepthStencilAspects, 0, 1, 0, 1};
    return barrier;
  };

  std::array<VkImageMemoryBarrier, 2> toTransfer = {
      makeDepthBarrier(p.runtime.frame->depthStencil.image,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT),
      makeDepthBarrier(p.runtime.frame->pickDepth.image,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_WRITE_BIT),
  };
  vkCmdPipelineBarrier(
      cmd, kDepthStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
      nullptr, static_cast<uint32_t>(toTransfer.size()), toTransfer.data());

  VkImageCopy depthCopy{};
  depthCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  depthCopy.srcSubresource.layerCount = 1;
  depthCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  depthCopy.dstSubresource.layerCount = 1;
  depthCopy.extent = {extent.width, extent.height, 1u};
  vkCmdCopyImage(cmd, p.runtime.frame->depthStencil.image,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 p.runtime.frame->pickDepth.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthCopy);

  std::array<VkImageMemoryBarrier, 2> toAttachment = {
      makeDepthBarrier(p.runtime.frame->depthStencil.image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_ACCESS_TRANSFER_READ_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
      makeDepthBarrier(p.runtime.frame->pickDepth.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
  };
  vkCmdPipelineBarrier(
      cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, kDepthStages, 0, 0, nullptr, 0,
      nullptr, static_cast<uint32_t>(toAttachment.size()), toAttachment.data());

  VkRenderPassBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = p.renderPasses.transparentPick;
  info.framebuffer = p.runtime.frame->transparentPickFramebuffer;
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = extent;

  vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
  recordSceneViewportAndScissor(cmd, swapChainManager_.extent());

  const VkPipeline pickFrontCullPipeline =
      choosePipeline(p.pipeline.pipelines.transparentPickFrontCull,
                     p.pipeline.pipelines.transparentPick);
  const VkPipeline pickNoCullPipeline =
      choosePipeline(p.pipeline.pipelines.transparentPickNoCull,
                     p.pipeline.pipelines.transparentPick);

  const auto bindTransparentPickGeometry =
      [&](VkDescriptorSet descriptorSet, const FrameSceneGeometry &scene) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                p.pipeline.layouts.scene, 0, 1, &descriptorSet,
                                0, nullptr);
        bindSceneGeometryBuffers(cmd, scene.vertexSlice, scene.indexSlice,
                                 scene.indexType);
      };

  const auto drawTransparentPickLists = [&](const FrameDrawLists &draws) {
    const BimSurfaceDrawRoutingPlan routingPlan =
        buildBimSurfaceDrawRoutingPlan(
            {.kind = BimSurfaceDrawKind::Transparent,
             .draws = surfaceDrawLists(draws),
             .gpuCompactionEligible = &draws == &p.bim.draws,
             .gpuVisibilityOwnsCpuFallback =
                 &draws == &p.bim.draws &&
                 p.bim.transparentMeshDrawsUseGpuVisibility});
    const auto drawGpuCompacted = [&](const BimSurfaceDrawRoute &route,
                                      VkPipeline pipeline,
                                      BindlessPushConstants pc) {
      return route.gpuCompactionAllowed && p.services.bimManager &&
             p.services.bimManager->drawCompactionReady(route.gpuSlot) &&
             pipeline != VK_NULL_HANDLE &&
             (vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline),
              pushSceneObjectIndex(cmd, p.pipeline.layouts.scene, pc,
                                   kIndirectObjectIndex),
              p.services.bimManager->drawCompacted(route.gpuSlot, cmd), true);
    };

    for (uint32_t routeIndex = 0; routeIndex < routingPlan.routeCount;
         ++routeIndex) {
      const BimSurfaceDrawRoute &route = routingPlan.routes[routeIndex];
      const VkPipeline pipeline = pipelineForBimSurfaceRoute(
          route, p.pipeline.pipelines.transparentPick, pickFrontCullPipeline,
          pickNoCullPipeline);
      if (drawGpuCompacted(route, pipeline, *p.pushConstants.bindless)) {
        continue;
      }
      if (route.cpuFallbackAllowed && hasDrawCommands(route.cpuCommands)) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                *route.cpuCommands, *p.pushConstants.bindless);
      }
    }
  };

  if (drawScene) {
    bindTransparentPickGeometry(p.descriptors.sceneDescriptorSet, p.scene);
    drawTransparentPickLists(p.draws);
  }
  if (drawBim) {
    bindTransparentPickGeometry(p.bim.sceneDescriptorSet, p.bim.scene);
    for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
      drawTransparentPickLists(*draws);
    }
  }

  vkCmdEndRenderPass(cmd);
}

bool FrameRecorder::canRecordShadowPass(const FrameRecordParams &p,
                                        uint32_t cascadeIndex) const {
  return cascadeIndex < kShadowCascadeCount &&
         p.renderPasses.shadow != VK_NULL_HANDLE &&
         p.pipeline.pipelines.shadowDepth != VK_NULL_HANDLE &&
         p.descriptors.shadowDescriptorSet != VK_NULL_HANDLE &&
         p.shadows.shadowFramebuffers != nullptr &&
         p.shadows.shadowFramebuffers[cascadeIndex] != VK_NULL_HANDLE;
}

bool FrameRecorder::shouldUseShadowSecondaryCommandBuffer(
    const FrameRecordParams &p, uint32_t cascadeIndex) const {
  if (usesGpuFilteredBimMeshShadowPath(p)) {
    return false;
  }
  return p.shadows.useShadowSecondaryCommandBuffers &&
         canRecordShadowPass(p, cascadeIndex) &&
         cascadeIndex < p.shadows.shadowSecondaryCommandBuffers.size() &&
         p.shadows.shadowSecondaryCommandBuffers[cascadeIndex] !=
             VK_NULL_HANDLE &&
         shadowCascadeCpuCommandCount(p, cascadeIndex) >=
             kMinParallelShadowCascadeCpuCommands;
}

void FrameRecorder::recordShadowCascadeSecondaryCommandBuffers(
    const FrameRecordParams &p) const {
  if (!p.shadows.useShadowSecondaryCommandBuffers)
    return;

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
    workers.emplace_back(
        std::async(std::launch::async, [this, &p, secondary, cascadeIndex]() {
          recordShadowCascadeSecondaryCommandBuffer(secondary, p, cascadeIndex);
        }));
  }

  for (auto &worker : workers) {
    worker.get();
  }
}

void FrameRecorder::recordShadowCascadeSecondaryCommandBuffer(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    uint32_t cascadeIndex) const {
  if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) {
    throw std::runtime_error(
        "failed to reset shadow secondary command buffer!");
  }

  VkCommandBufferInheritanceInfo inheritanceInfo{};
  inheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
  inheritanceInfo.renderPass = p.renderPasses.shadow;
  inheritanceInfo.subpass = 0;
  inheritanceInfo.framebuffer = p.shadows.shadowFramebuffers[cascadeIndex];

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
  beginInfo.pInheritanceInfo = &inheritanceInfo;
  if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
    throw std::runtime_error(
        "failed to begin shadow secondary command buffer!");
  }

  recordShadowPassBody(cmd, p, cascadeIndex);

  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    throw std::runtime_error(
        "failed to record shadow secondary command buffer!");
  }
}

void FrameRecorder::recordShadowPass(VkCommandBuffer cmd,
                                     const FrameRecordParams &p,
                                     uint32_t cascadeIndex) const {
  if (!displayModeRecordsShadowAtlas(currentDisplayMode(guiManager_)))
    return;
  if (!canRecordShadowPass(p, cascadeIndex))
    return;

  VkRenderPassBeginInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  info.renderPass = p.renderPasses.shadow;
  info.framebuffer = p.shadows.shadowFramebuffers[cascadeIndex];
  info.renderArea.offset = {0, 0};
  info.renderArea.extent = {kShadowMapResolution, kShadowMapResolution};
  VkClearValue clearVal{};
  clearVal.depthStencil = {0.0f, 0};
  info.clearValueCount = 1;
  info.pClearValues = &clearVal;

  if (shouldUseShadowSecondaryCommandBuffer(p, cascadeIndex)) {
    vkCmdBeginRenderPass(cmd, &info,
                         VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
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
                                         const FrameRecordParams &p,
                                         uint32_t cascadeIndex) const {
  VkViewport viewport{};
  // Shadow maps intentionally use a positive-height viewport. Their atlas UV
  // mapping therefore differs from scene-buffer UV mapping and does not flip Y.
  viewport.width = static_cast<float>(kShadowMapResolution);
  viewport.height = static_cast<float>(kShadowMapResolution);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.extent = {kShadowMapResolution, kShadowMapResolution};
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  // Reverse-Z shadow maps need negative caster bias to push written depth
  // toward far (0.0). Keep this dynamic so tuning does not rebuild pipelines.
  vkCmdSetDepthBias(cmd, p.shadows.shadowSettings.rasterConstantBias, 0.0f,
                    p.shadows.shadowSettings.rasterSlopeBias);

  ShadowPushConstants spc{};
  spc.cascadeIndex = cascadeIndex;
  if (p.pushConstants.bindless != nullptr) {
    spc.sectionPlaneEnabled = p.pushConstants.bindless->sectionPlaneEnabled;
    spc.sectionPlane = p.pushConstants.bindless->sectionPlane;
  }
  constexpr VkShaderStageFlags kShadowPushStages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  const auto bindShadowDescriptorSets = [&](VkDescriptorSet sceneSet) {
    std::array<VkDescriptorSet, 2> shadowSets = {
        sceneSet, p.descriptors.shadowDescriptorSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.pipeline.layouts.shadow, 0,
                            static_cast<uint32_t>(shadowSets.size()),
                            shadowSets.data(), 0, nullptr);
  };

  const auto drawShadowList = [&](const std::vector<DrawCommand> *commands) {
    if (commands == nullptr || commands->empty())
      return;
    spc.objectIndex = kIndirectObjectIndex;
    vkCmdPushConstants(cmd, p.pipeline.layouts.shadow, kShadowPushStages, 0,
                       sizeof(ShadowPushConstants), &spc);
    for (const auto &dc : *commands) {
      vkCmdDrawIndexed(cmd, dc.indexCount, drawInstanceCount(dc), dc.firstIndex,
                       0, dc.objectIndex);
    }
  };

  const VkPipeline shadowFrontCullPipeline =
      choosePipeline(p.pipeline.pipelines.shadowDepthFrontCull,
                     p.pipeline.pipelines.shadowDepth);
  const VkPipeline shadowNoCullPipeline = choosePipeline(
      p.pipeline.pipelines.shadowDepthNoCull, p.pipeline.pipelines.shadowDepth);

  const bool hasSceneShadowGeometry =
      p.descriptors.sceneDescriptorSet != VK_NULL_HANDLE &&
      p.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
      p.scene.indexSlice.buffer != VK_NULL_HANDLE;
  if (hasSceneShadowGeometry) {
    bindShadowDescriptorSets(p.descriptors.sceneDescriptorSet);
    bindSceneGeometryBuffers(cmd, p.scene.vertexSlice, p.scene.indexSlice,
                             p.scene.indexType);

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
    const bool useGpuShadowCull = useGpuShadowCullForCascade(p, cascadeIndex);

    if (useGpuShadowCull) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        p.pipeline.pipelines.shadowDepth);
      spc.objectIndex = kIndirectObjectIndex;
      vkCmdPushConstants(cmd, p.pipeline.layouts.shadow, kShadowPushStages, 0,
                         sizeof(ShadowPushConstants), &spc);
      vkCmdDrawIndexedIndirectCount(
          cmd, gpuShadowIndirectBuffer, 0, gpuShadowCountBuffer, 0,
          gpuShadowMaxDrawCount,
          sizeof(container::gpu::GpuDrawIndexedIndirectCommand));
    }

    const auto &singleSidedCommands =
        shadowCascadeSingleSidedDrawCommands_[cascadeIndex];
    if (!useGpuShadowCull && !singleSidedCommands.empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        p.pipeline.pipelines.shadowDepth);
      drawShadowList(&singleSidedCommands);
    }

    const auto &windingFlippedCommands =
        shadowCascadeWindingFlippedDrawCommands_[cascadeIndex];
    if (!windingFlippedCommands.empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadowFrontCullPipeline);
      drawShadowList(&windingFlippedCommands);
    }

    const auto &doubleSidedCommands =
        shadowCascadeDoubleSidedDrawCommands_[cascadeIndex];
    if (!doubleSidedCommands.empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadowNoCullPipeline);
      drawShadowList(&doubleSidedCommands);
    }
  }

  if (hasBimShadowGeometry(p)) {
    bindShadowDescriptorSets(p.bim.sceneDescriptorSet);
    bindSceneGeometryBuffers(cmd, p.bim.scene.vertexSlice,
                             p.bim.scene.indexSlice, p.bim.scene.indexType);

    const auto drawGpuFilteredBimShadowSlot = [&](BimDrawCompactionSlot slot,
                                                  VkPipeline pipeline) {
      if (!usesGpuFilteredBimMeshShadowPath(p) || pipeline == VK_NULL_HANDLE ||
          !p.services.bimManager->drawCompactionReady(slot)) {
        return;
      }

      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      spc.objectIndex = kIndirectObjectIndex;
      vkCmdPushConstants(cmd, p.pipeline.layouts.shadow, kShadowPushStages, 0,
                         sizeof(ShadowPushConstants), &spc);
      p.services.bimManager->drawCompacted(slot, cmd);
    };

    drawGpuFilteredBimShadowSlot(BimDrawCompactionSlot::OpaqueSingleSided,
                                 p.pipeline.pipelines.shadowDepth);
    drawGpuFilteredBimShadowSlot(BimDrawCompactionSlot::OpaqueWindingFlipped,
                                 shadowFrontCullPipeline);
    drawGpuFilteredBimShadowSlot(BimDrawCompactionSlot::OpaqueDoubleSided,
                                 shadowNoCullPipeline);

    const auto &bimSingleSidedCommands =
        bimShadowCascadeSingleSidedDrawCommands_[cascadeIndex];
    if (!bimSingleSidedCommands.empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        p.pipeline.pipelines.shadowDepth);
      drawShadowList(&bimSingleSidedCommands);
    }

    const auto &bimWindingFlippedCommands =
        bimShadowCascadeWindingFlippedDrawCommands_[cascadeIndex];
    if (!bimWindingFlippedCommands.empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadowFrontCullPipeline);
      drawShadowList(&bimWindingFlippedCommands);
    }

    const auto &bimDoubleSidedCommands =
        bimShadowCascadeDoubleSidedDrawCommands_[cascadeIndex];
    if (!bimDoubleSidedCommands.empty()) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadowNoCullPipeline);
      drawShadowList(&bimDoubleSidedCommands);
    }
  }
}

void FrameRecorder::recordBimPointCloudPrimitivePass(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  const auto &pass = p.bim.primitivePasses.pointCloud;
  const BimPrimitivePassPlan primitivePlan = buildBimPrimitivePassPlan(
      {.kind = BimPrimitivePassKind::Points,
       .enabled = pass.enabled,
       .depthTest = pass.depthTest,
       .placeholderRangePreviewEnabled = pass.placeholderRangePreviewEnabled,
       .nativeDrawsUseGpuVisibility = p.bim.nativePointDrawsUseGpuVisibility,
       .opacity = pass.opacity,
       .primitiveSize = pass.pointSize,
       .placeholderDraws = primitivePassDrawLists(p.bim.pointDraws),
       .nativeDraws = primitivePassDrawLists(p.bim.nativePointDraws)});
  if (!primitivePlan.active || !hasBimPrimitivePassGeometry(p) ||
      p.pipeline.layouts.wireframe == VK_NULL_HANDLE ||
      p.pushConstants.wireframe == nullptr) {
    return;
  }

  const VkPipeline pipeline = primitivePlan.depthTest
                                  ? p.pipeline.pipelines.bimPointCloudDepth
                                  : p.pipeline.pipelines.bimPointCloudNoDepth;
  if (pipeline == VK_NULL_HANDLE) {
    return;
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.layouts.wireframe, 0, 1,
                          &p.bim.sceneDescriptorSet, 0, nullptr);
  bindSceneGeometryBuffers(cmd, p.bim.scene.vertexSlice, p.bim.scene.indexSlice,
                           p.bim.scene.indexType);

  WireframePushConstants pc = *p.pushConstants.wireframe;
  if (primitivePlan.gpuCompaction) {
    const auto drawGpuCompactedNativePoint = [&](BimDrawCompactionSlot slot) {
      if (p.services.bimManager == nullptr ||
          !p.services.bimManager->drawCompactionReady(slot)) {
        return;
      }
      WireframePushConstants gpuPc = pc;
      gpuPc.objectIndex = kIndirectObjectIndex;
      gpuPc.colorIntensity = glm::vec4(pass.color, primitivePlan.opacity);
      gpuPc.lineWidth = primitivePlan.primitiveSize;
      vkCmdPushConstants(cmd, p.pipeline.layouts.wireframe,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(WireframePushConstants), &gpuPc);
      p.services.bimManager->drawCompacted(slot, cmd);
    };
    for (uint32_t slotIndex = 0; slotIndex < primitivePlan.gpuSlotCount;
         ++slotIndex) {
      drawGpuCompactedNativePoint(primitivePlan.gpuSlots[slotIndex]);
    }
    return;
  }
  for (const std::vector<DrawCommand> *commands :
       primitivePlan.cpuDrawSources) {
    if (hasDrawCommands(commands)) {
      debugOverlay_.drawWireframe(cmd, p.pipeline.layouts.wireframe, *commands,
                                  pass.color, primitivePlan.opacity,
                                  primitivePlan.primitiveSize, pc);
    }
  }
}

void FrameRecorder::recordBimCurvePrimitivePass(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  const auto &pass = p.bim.primitivePasses.curves;
  const BimPrimitivePassPlan primitivePlan = buildBimPrimitivePassPlan(
      {.kind = BimPrimitivePassKind::Curves,
       .enabled = pass.enabled,
       .depthTest = pass.depthTest,
       .placeholderRangePreviewEnabled = pass.placeholderRangePreviewEnabled,
       .nativeDrawsUseGpuVisibility = p.bim.nativeCurveDrawsUseGpuVisibility,
       .opacity = pass.opacity,
       .primitiveSize = pass.lineWidth,
       .placeholderDraws = primitivePassDrawLists(p.bim.curveDraws),
       .nativeDraws = primitivePassDrawLists(p.bim.nativeCurveDraws)});
  if (!primitivePlan.active || !hasBimPrimitivePassGeometry(p) ||
      p.pipeline.layouts.wireframe == VK_NULL_HANDLE ||
      p.pushConstants.wireframe == nullptr) {
    return;
  }

  const VkPipeline pipeline = primitivePlan.depthTest
                                  ? p.pipeline.pipelines.bimCurveDepth
                                  : p.pipeline.pipelines.bimCurveNoDepth;
  if (pipeline == VK_NULL_HANDLE) {
    return;
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdSetLineWidth(cmd, p.debug.wireframeWideLinesSupported
                             ? primitivePlan.primitiveSize
                             : 1.0f);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.layouts.wireframe, 0, 1,
                          &p.bim.sceneDescriptorSet, 0, nullptr);
  bindSceneGeometryBuffers(cmd, p.bim.scene.vertexSlice, p.bim.scene.indexSlice,
                           p.bim.scene.indexType);

  WireframePushConstants pc = *p.pushConstants.wireframe;
  if (primitivePlan.gpuCompaction) {
    const auto drawGpuCompactedNativeCurve = [&](BimDrawCompactionSlot slot) {
      if (p.services.bimManager == nullptr ||
          !p.services.bimManager->drawCompactionReady(slot)) {
        return;
      }
      WireframePushConstants gpuPc = pc;
      gpuPc.objectIndex = kIndirectObjectIndex;
      gpuPc.colorIntensity = glm::vec4(pass.color, primitivePlan.opacity);
      gpuPc.lineWidth = primitivePlan.primitiveSize;
      vkCmdPushConstants(cmd, p.pipeline.layouts.wireframe,
                         VK_SHADER_STAGE_VERTEX_BIT |
                             VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, sizeof(WireframePushConstants), &gpuPc);
      p.services.bimManager->drawCompacted(slot, cmd);
    };
    for (uint32_t slotIndex = 0; slotIndex < primitivePlan.gpuSlotCount;
         ++slotIndex) {
      drawGpuCompactedNativeCurve(primitivePlan.gpuSlots[slotIndex]);
    }
    return;
  }
  for (const std::vector<DrawCommand> *commands :
       primitivePlan.cpuDrawSources) {
    if (hasDrawCommands(commands)) {
      debugOverlay_.drawWireframe(cmd, p.pipeline.layouts.wireframe, *commands,
                                  pass.color, primitivePlan.opacity,
                                  primitivePlan.primitiveSize, pc);
    }
  }
}

void FrameRecorder::recordBimSectionClipCapPass(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  if (!sectionClipCapStyleActive(p.bim.sectionClipCaps) ||
      !hasBimSectionClipCapGeometry(p) ||
      p.pipeline.layouts.wireframe == VK_NULL_HANDLE ||
      p.pushConstants.wireframe == nullptr) {
    return;
  }

  const bool fillPipelineReady =
      p.bim.sectionClipCaps.fillEnabled &&
      p.pipeline.pipelines.bimSectionClipCapFill != VK_NULL_HANDLE;
  const bool hatchPipelineReady =
      p.bim.sectionClipCaps.hatchEnabled &&
      p.pipeline.pipelines.bimSectionClipCapHatch != VK_NULL_HANDLE;
  if (!fillPipelineReady && !hatchPipelineReady) {
    return;
  }

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.layouts.wireframe, 0, 1,
                          &p.bim.sceneDescriptorSet, 0, nullptr);
  bindSceneGeometryBuffers(cmd, p.bim.sectionClipCapGeometry.scene.vertexSlice,
                           p.bim.sectionClipCapGeometry.scene.indexSlice,
                           p.bim.sectionClipCapGeometry.scene.indexType);

  WireframePushConstants pc = *p.pushConstants.wireframe;
  pc.sectionPlaneEnabled = 0u;
  const auto &style = p.bim.sectionClipCaps;
  if (fillPipelineReady &&
      hasDrawCommands(p.bim.sectionClipCapGeometry.fillDrawCommands)) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.bimSectionClipCapFill);
    debugOverlay_.drawWireframe(cmd, p.pipeline.layouts.wireframe,
                                *p.bim.sectionClipCapGeometry.fillDrawCommands,
                                glm::vec3(style.fillColor), style.fillColor.a,
                                1.0f, pc);
  }
  if (hatchPipelineReady &&
      hasDrawCommands(p.bim.sectionClipCapGeometry.hatchDrawCommands)) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.bimSectionClipCapHatch);
    const float hatchLineWidth = p.debug.wireframeWideLinesSupported
                                     ? std::max(style.hatchLineWidth, 1.0f)
                                     : 1.0f;
    vkCmdSetLineWidth(cmd, hatchLineWidth);
    debugOverlay_.drawWireframe(cmd, p.pipeline.layouts.wireframe,
                                *p.bim.sectionClipCapGeometry.hatchDrawCommands,
                                glm::vec3(style.hatchColor), style.hatchColor.a,
                                hatchLineWidth, pc);
    vkCmdSetLineWidth(cmd, 1.0f);
  }
}

void FrameRecorder::recordLightingPass(
    VkCommandBuffer cmd, const FrameRecordParams &p, VkDescriptorSet sceneSet,
    const std::array<VkDescriptorSet, 2> &lightingDescriptorSets,
    const std::array<VkDescriptorSet, 4> &transparentDescriptorSets) const {
  const uint32_t pointLightCount =
      lightingManager_
          ? static_cast<uint32_t>(lightingManager_->pointLightsSsbo().size())
          : 0u;
  const DeferredLightingFrameState lightingState =
      buildDeferredLightingFrameState(deferredLightingFrameInputs(
          p, guiManager_, isPassActive(RenderPassId::TileCull),
          lightingManager_ && lightingManager_->isTiledLightingReady(),
          pointLightCount));
  const auto &wireframeSettings = lightingState.wireframeSettings;
  const auto &normalValidationSettings =
      lightingState.normalValidationSettings;
  const bool wireframeFullMode = lightingState.wireframeFullMode;
  const bool wireframeOverlayMode = lightingState.wireframeOverlayMode;
  const bool showObjectSpaceNormals =
      lightingState.objectSpaceNormalsEnabled;
  const bool showNormalValidation = lightingState.normalValidationEnabled;
  const bool transparentOitEnabled = lightingState.transparentOitEnabled;
  const VkPipeline activeWireframePipeline =
      wireframeSettings.depthTest ? p.pipeline.pipelines.wireframeDepth
                                  : p.pipeline.pipelines.wireframeNoDepth;
  const VkPipeline activeWireframeFrontCullPipeline =
      wireframeSettings.depthTest
          ? choosePipeline(p.pipeline.pipelines.wireframeDepthFrontCull,
                           p.pipeline.pipelines.wireframeDepth)
          : choosePipeline(p.pipeline.pipelines.wireframeNoDepthFrontCull,
                           p.pipeline.pipelines.wireframeNoDepth);
  syncOverlaySectionPlanePushConstants(p.pushConstants);

  VkRenderPassBeginInfo lightingPassInfo{};
  lightingPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  lightingPassInfo.renderPass = p.renderPasses.lighting;
  lightingPassInfo.framebuffer = p.runtime.frame->lightingFramebuffer;
  lightingPassInfo.renderArea.offset = {0, 0};
  lightingPassInfo.renderArea.extent = swapChainManager_.extent();
  std::array<VkClearValue, 2> lightingClearValues{};
  lightingClearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  lightingClearValues[1].depthStencil = {0.0f, 0};
  lightingPassInfo.clearValueCount =
      static_cast<uint32_t>(lightingClearValues.size());
  lightingPassInfo.pClearValues = lightingClearValues.data();

  vkCmdBeginRenderPass(cmd, &lightingPassInfo, VK_SUBPASS_CONTENTS_INLINE);
  recordSceneViewportAndScissor(cmd, swapChainManager_.extent());

  const auto bindWireframePipeline = [&](VkPipeline pipeline, float lineWidth) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (p.debug.wireframeRasterModeSupported) {
      vkCmdSetLineWidth(cmd,
                        p.debug.wireframeWideLinesSupported ? lineWidth : 1.0f);
    }
  };
  const auto drawWireframeCommands =
      [&](const std::vector<DrawCommand> *commands, VkPipeline pipeline) {
        if (!hasDrawCommands(commands) || pipeline == VK_NULL_HANDLE)
          return;
        bindWireframePipeline(pipeline, wireframeSettings.lineWidth);
        debugOverlay_.drawWireframe(
            cmd, p.pipeline.layouts.wireframe, *commands,
            wireframeSettings.color, lightingState.wireframeIntensity,
            wireframeSettings.lineWidth, *p.pushConstants.wireframe);
      };
  const auto bindWireframeGeometry = [&](VkDescriptorSet descriptorSet,
                                         const FrameSceneGeometry &scene) {
    if (descriptorSet == VK_NULL_HANDLE ||
        scene.vertexSlice.buffer == VK_NULL_HANDLE ||
        scene.indexSlice.buffer == VK_NULL_HANDLE) {
      return false;
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p.pipeline.layouts.wireframe, 0, 1, &descriptorSet,
                            0, nullptr);
    bindSceneGeometryBuffers(cmd, scene.vertexSlice, scene.indexSlice,
                             scene.indexType);
    return true;
  };
  const auto drawWireframeLists = [&](const FrameDrawLists &draws) {
    drawWireframeCommands(draws.opaqueSingleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(draws.transparentSingleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(draws.opaqueWindingFlippedDrawCommands,
                          activeWireframeFrontCullPipeline);
    drawWireframeCommands(draws.transparentWindingFlippedDrawCommands,
                          activeWireframeFrontCullPipeline);
    drawWireframeCommands(draws.opaqueDoubleSidedDrawCommands,
                          activeWireframePipeline);
    drawWireframeCommands(draws.transparentDoubleSidedDrawCommands,
                          activeWireframePipeline);
  };

  if (wireframeFullMode) {
    if (bindWireframeGeometry(p.descriptors.sceneDescriptorSet, p.scene)) {
      drawWireframeLists(p.draws);
      bindWireframePipeline(activeWireframePipeline,
                            wireframeSettings.lineWidth);
      drawDiagnosticCube(cmd, p.pipeline.layouts.wireframe,
                         p.scene.diagCubeObjectIndex,
                         *p.pushConstants.bindless);
    }
    if (hasBimRenderableGeometry(p) &&
        bindWireframeGeometry(p.bim.sceneDescriptorSet, p.bim.scene)) {
      for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
        drawWireframeLists(*draws);
      }
    }
  } else if (showObjectSpaceNormals) {
    const VkPipeline objectNormalNoCullPipeline =
        choosePipeline(p.pipeline.pipelines.objectNormalDebugNoCull,
                       p.pipeline.pipelines.objectNormalDebug);
    const VkPipeline objectNormalFrontCullPipeline =
        choosePipeline(p.pipeline.pipelines.objectNormalDebugFrontCull,
                       p.pipeline.pipelines.objectNormalDebug);
    const auto bindObjectNormalGeometry = [&](VkDescriptorSet descriptorSet,
                                              const FrameSceneGeometry &scene) {
      if (descriptorSet == VK_NULL_HANDLE ||
          scene.vertexSlice.buffer == VK_NULL_HANDLE ||
          scene.indexSlice.buffer == VK_NULL_HANDLE) {
        return false;
      }
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              p.pipeline.layouts.scene, 0, 1, &descriptorSet, 0,
                              nullptr);
      bindSceneGeometryBuffers(cmd, scene.vertexSlice, scene.indexSlice,
                               scene.indexType);
      return true;
    };
    const auto drawObjectNormalLists = [&](const FrameDrawLists &draws) {
      if (hasDrawCommands(draws.opaqueSingleSidedDrawCommands) ||
          hasDrawCommands(draws.transparentSingleSidedDrawCommands)) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.pipelines.objectNormalDebug);
        if (hasDrawCommands(draws.opaqueSingleSidedDrawCommands)) {
          debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                  *draws.opaqueSingleSidedDrawCommands,
                                  *p.pushConstants.bindless);
        }
        if (hasDrawCommands(draws.transparentSingleSidedDrawCommands)) {
          debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                  *draws.transparentSingleSidedDrawCommands,
                                  *p.pushConstants.bindless);
        }
      }

      if (hasDrawCommands(draws.opaqueWindingFlippedDrawCommands) ||
          hasDrawCommands(draws.transparentWindingFlippedDrawCommands)) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          objectNormalFrontCullPipeline);
        if (hasDrawCommands(draws.opaqueWindingFlippedDrawCommands)) {
          debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                  *draws.opaqueWindingFlippedDrawCommands,
                                  *p.pushConstants.bindless);
        }
        if (hasDrawCommands(draws.transparentWindingFlippedDrawCommands)) {
          debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                  *draws.transparentWindingFlippedDrawCommands,
                                  *p.pushConstants.bindless);
        }
      }

      if (hasDrawCommands(draws.opaqueDoubleSidedDrawCommands) ||
          hasDrawCommands(draws.transparentDoubleSidedDrawCommands)) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          objectNormalNoCullPipeline);
        if (hasDrawCommands(draws.opaqueDoubleSidedDrawCommands)) {
          debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                  *draws.opaqueDoubleSidedDrawCommands,
                                  *p.pushConstants.bindless);
        }
        if (hasDrawCommands(draws.transparentDoubleSidedDrawCommands)) {
          debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                  *draws.transparentDoubleSidedDrawCommands,
                                  *p.pushConstants.bindless);
        }
      }
    };

    if (bindObjectNormalGeometry(p.descriptors.sceneDescriptorSet, p.scene)) {
      drawObjectNormalLists(p.draws);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        p.pipeline.pipelines.objectNormalDebug);
      drawDiagnosticCube(cmd, p.pipeline.layouts.scene,
                         p.scene.diagCubeObjectIndex,
                         *p.pushConstants.bindless);
    }
    if (hasBimRenderableGeometry(p) &&
        bindObjectNormalGeometry(p.bim.sceneDescriptorSet, p.bim.scene)) {
      for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
        drawObjectNormalLists(*draws);
      }
    }
  } else if (lightingState.directionalLightingEnabled) {
    const std::array<VkDescriptorSet, 3> directionalDescriptorSets = {
        lightingDescriptorSets[0], lightingDescriptorSets[1], sceneSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.directionalLight);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.layouts.lighting, 0,
        static_cast<uint32_t>(directionalDescriptorSets.size()),
        directionalDescriptorSets.data(), 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
  }

  VkClearAttachment stencilClearAttachment{};
  stencilClearAttachment.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
  stencilClearAttachment.clearValue.depthStencil = {0.0f, 0};
  VkClearRect stencilClearRect{};
  stencilClearRect.rect.offset = {0, 0};
  stencilClearRect.rect.extent = swapChainManager_.extent();
  stencilClearRect.baseArrayLayer = 0;
  stencilClearRect.layerCount = 1;

  if (lightingState.pointLighting.path != DeferredPointLightingPath::None) {
    if (lightingState.pointLighting.path == DeferredPointLightingPath::Tiled) {
      // Tiled point light accumulation — single fullscreen triangle.
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        p.pipeline.pipelines.tiledPointLight);
      const std::array<VkDescriptorSet, 3> tiledSets = {
          p.runtime.frame->lightingDescriptorSet,
          p.descriptors.tiledDescriptorSet, sceneSet};
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
          p.debug.debugVisualizePointLightStencil
              ? p.pipeline.pipelines.pointLightStencilDebug
              : p.pipeline.pipelines.pointLight;
      const auto *pointLights =
          lightingManager_ ? &lightingManager_->pointLightsSsbo() : nullptr;
      const uint32_t numLights =
          lightingState.pointLighting.stencilLightCount;

      if (numLights > 0u) {
        const std::array<VkDescriptorSet, 3> pointLightingSets = {
            lightingDescriptorSets[0], lightingDescriptorSets[1], sceneSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                p.pipeline.layouts.lighting, 0,
                                static_cast<uint32_t>(pointLightingSets.size()),
                                pointLightingSets.data(), 0, nullptr);
      }
      for (uint32_t i = 0; i < numLights; ++i) {
        vkCmdClearAttachments(cmd, 1, &stencilClearAttachment, 1,
                              &stencilClearRect);
        p.pushConstants.light->positionRadius =
            (*pointLights)[i].positionRadius;
        p.pushConstants.light->colorIntensity =
            (*pointLights)[i].colorIntensity;
        p.pushConstants.light->directionInnerCos =
            (*pointLights)[i].directionInnerCos;
        p.pushConstants.light->coneOuterCosType =
            (*pointLights)[i].coneOuterCosType;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.pipelines.stencilVolume);
        vkCmdPushConstants(
            cmd, p.pipeline.layouts.lighting,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(LightPushConstants), p.pushConstants.light);
        vkCmdDraw(cmd,
                  lightingManager_ ? lightingManager_->lightVolumeIndexCount()
                                   : 0,
                  1, 0, 0);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          activePointPipeline);
        vkCmdPushConstants(
            cmd, p.pipeline.layouts.lighting,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
            sizeof(LightPushConstants), p.pushConstants.light);
        vkCmdDraw(cmd, 3, 1, 0, 0);
      }
    }
  }

  if (transparentOitEnabled) {
    const VkPipeline transparentFrontCullPipeline =
        choosePipeline(p.pipeline.pipelines.transparentFrontCull,
                       p.pipeline.pipelines.transparent);
    const VkPipeline transparentNoCullPipeline =
        choosePipeline(p.pipeline.pipelines.transparentNoCull,
                       p.pipeline.pipelines.transparent);

    const auto bindTransparentGeometry = [&](VkDescriptorSet sceneSet,
                                             const FrameSceneGeometry &scene) {
      std::array<VkDescriptorSet, 4> sets = transparentDescriptorSets;
      sets[0] = sceneSet;
      vkCmdBindDescriptorSets(
          cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline.layouts.transparent,
          0, static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
      bindSceneGeometryBuffers(cmd, scene.vertexSlice, scene.indexSlice,
                               scene.indexType);
    };

    const auto drawTransparentLists = [&](const FrameDrawLists &draws,
                                          uint32_t semanticColorMode) {
      if (!hasTransparentDrawCommands(draws))
        return;

      const BimSurfaceDrawRoutingPlan routingPlan =
          buildBimSurfaceDrawRoutingPlan(
              {.kind = BimSurfaceDrawKind::Transparent,
               .draws = surfaceDrawLists(draws),
               .gpuCompactionEligible = &draws == &p.bim.draws,
               .gpuVisibilityOwnsCpuFallback =
                   &draws == &p.bim.draws &&
                   p.bim.transparentMeshDrawsUseGpuVisibility});
      BindlessPushConstants transparentPc = *p.pushConstants.bindless;
      transparentPc.semanticColorMode = semanticColorMode;
      const auto drawGpuCompacted = [&](const BimSurfaceDrawRoute &route,
                                        VkPipeline pipeline,
                                        BindlessPushConstants pc) {
        return route.gpuCompactionAllowed && p.services.bimManager &&
               p.services.bimManager->drawCompactionReady(route.gpuSlot) &&
               pipeline != VK_NULL_HANDLE &&
               (vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipeline),
                pushSceneObjectIndex(cmd, p.pipeline.layouts.transparent, pc,
                                     kIndirectObjectIndex),
                p.services.bimManager->drawCompacted(route.gpuSlot, cmd), true);
      };

      for (uint32_t routeIndex = 0; routeIndex < routingPlan.routeCount;
           ++routeIndex) {
        const BimSurfaceDrawRoute &route = routingPlan.routes[routeIndex];
        const VkPipeline pipeline = pipelineForBimSurfaceRoute(
            route, p.pipeline.pipelines.transparent,
            transparentFrontCullPipeline, transparentNoCullPipeline);
        if (drawGpuCompacted(route, pipeline, transparentPc)) {
          continue;
        }
        if (route.cpuFallbackAllowed && hasDrawCommands(route.cpuCommands)) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          debugOverlay_.drawScene(cmd, p.pipeline.layouts.transparent,
                                  *route.cpuCommands, transparentPc);
        }
      }
    };

    if (hasTransparentDrawCommands(p.draws)) {
      bindTransparentGeometry(p.descriptors.sceneDescriptorSet, p.scene);
      drawTransparentLists(p.draws, 0u);
    }
    if (hasBimTransparentGeometry(p)) {
      bindTransparentGeometry(p.bim.sceneDescriptorSet, p.bim.scene);
      for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
        drawTransparentLists(*draws, p.bim.semanticColorMode);
      }
    }
  }

  if (lightingState.geometryOverlayEnabled) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.geometryDebug);
    const auto drawGeometryOverlayScene = [&](VkDescriptorSet descriptorSet,
                                              const FrameSceneGeometry &scene,
                                              const FrameDrawLists &draws) {
      if (descriptorSet == VK_NULL_HANDLE ||
          scene.vertexSlice.buffer == VK_NULL_HANDLE ||
          scene.indexSlice.buffer == VK_NULL_HANDLE ||
          !hasRenderableDrawCommands(draws)) {
        return;
      }
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              p.pipeline.layouts.scene, 0, 1, &descriptorSet, 0,
                              nullptr);
      bindSceneGeometryBuffers(cmd, scene.vertexSlice, scene.indexSlice,
                               scene.indexType);
      if (hasDrawCommands(draws.opaqueDrawCommands)) {
        debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                *draws.opaqueDrawCommands,
                                *p.pushConstants.bindless);
      }
      if (hasDrawCommands(draws.transparentDrawCommands)) {
        debugOverlay_.drawScene(cmd, p.pipeline.layouts.scene,
                                *draws.transparentDrawCommands,
                                *p.pushConstants.bindless);
      }
    };
    drawGeometryOverlayScene(p.descriptors.sceneDescriptorSet, p.scene,
                             p.draws);
    for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
      drawGeometryOverlayScene(p.bim.sceneDescriptorSet, p.bim.scene, *draws);
    }
  }

  if (showNormalValidation &&
      p.pipeline.pipelines.normalValidation != VK_NULL_HANDLE) {
    const VkPipeline normalValidationFrontCullPipeline =
        choosePipeline(p.pipeline.pipelines.normalValidationFrontCull,
                       p.pipeline.pipelines.normalValidation);
    const VkPipeline normalValidationNoCullPipeline =
        choosePipeline(p.pipeline.pipelines.normalValidationNoCull,
                       p.pipeline.pipelines.normalValidation);
    static const std::vector<DrawCommand> emptyDrawCommands;
    const auto bindNormalValidationGeometry =
        [&](VkDescriptorSet descriptorSet, const FrameSceneGeometry &scene) {
          if (descriptorSet == VK_NULL_HANDLE ||
              scene.vertexSlice.buffer == VK_NULL_HANDLE ||
              scene.indexSlice.buffer == VK_NULL_HANDLE) {
            return false;
          }
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  p.pipeline.layouts.normalValidation, 0, 1,
                                  &descriptorSet, 0, nullptr);
          bindSceneGeometryBuffers(cmd, scene.vertexSlice, scene.indexSlice,
                                   scene.indexType);
          return true;
        };
    const auto drawNormalValidationCommands =
        [&](const std::vector<DrawCommand> *opaque,
            const std::vector<DrawCommand> *transparent, VkPipeline pipeline,
            uint32_t faceClassificationFlags) {
          if (!hasDrawCommands(opaque) && !hasDrawCommands(transparent)) {
            return;
          }
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          debugOverlay_.recordNormalValidation(
              cmd, p.pipeline.layouts.normalValidation,
              opaque ? *opaque : emptyDrawCommands,
              transparent ? *transparent : emptyDrawCommands,
              faceClassificationFlags, normalValidationSettings,
              *p.pushConstants.normalValidation);
        };
    const auto drawNormalValidationLists = [&](const FrameDrawLists &draws) {
      drawNormalValidationCommands(draws.opaqueSingleSidedDrawCommands,
                                   draws.transparentSingleSidedDrawCommands,
                                   p.pipeline.pipelines.normalValidation, 0u);
      drawNormalValidationCommands(draws.opaqueWindingFlippedDrawCommands,
                                   draws.transparentWindingFlippedDrawCommands,
                                   normalValidationFrontCullPipeline,
                                   kNormalValidationInvertFaceClassification);
      drawNormalValidationCommands(draws.opaqueDoubleSidedDrawCommands,
                                   draws.transparentDoubleSidedDrawCommands,
                                   normalValidationNoCullPipeline,
                                   kNormalValidationBothSidesValid);
    };

    if (bindNormalValidationGeometry(p.descriptors.sceneDescriptorSet,
                                     p.scene)) {
      drawNormalValidationLists(p.draws);
    }
    if (hasBimRenderableGeometry(p) &&
        bindNormalValidationGeometry(p.bim.sceneDescriptorSet, p.bim.scene)) {
      for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
        drawNormalValidationLists(*draws);
      }
    }
  }

  if (lightingState.surfaceNormalLinesEnabled) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p.pipeline.pipelines.surfaceNormalLine);
    if (p.debug.wireframeRasterModeSupported) {
      vkCmdSetLineWidth(cmd, lightingState.surfaceNormalLineWidth);
    }
    static const std::vector<DrawCommand> emptyDrawCommands;
    const auto drawSurfaceNormalsScene = [&](VkDescriptorSet descriptorSet,
                                             const FrameSceneGeometry &scene,
                                             const FrameDrawLists &draws) {
      if (descriptorSet == VK_NULL_HANDLE ||
          scene.vertexSlice.buffer == VK_NULL_HANDLE ||
          scene.indexSlice.buffer == VK_NULL_HANDLE ||
          !hasRenderableDrawCommands(draws)) {
        return;
      }
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              p.pipeline.layouts.surfaceNormal, 0, 1,
                              &descriptorSet, 0, nullptr);
      bindSceneGeometryBuffers(cmd, scene.vertexSlice, scene.indexSlice,
                               scene.indexType);
      debugOverlay_.recordSurfaceNormals(
          cmd, p.pipeline.layouts.surfaceNormal,
          draws.opaqueDrawCommands ? *draws.opaqueDrawCommands
                                   : emptyDrawCommands,
          draws.transparentDrawCommands ? *draws.transparentDrawCommands
                                        : emptyDrawCommands,
          normalValidationSettings, *p.pushConstants.surfaceNormal);
    };
    drawSurfaceNormalsScene(p.descriptors.sceneDescriptorSet, p.scene, p.draws);
    for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
      drawSurfaceNormalsScene(p.bim.sceneDescriptorSet, p.bim.scene, *draws);
    }
  }

  if (wireframeOverlayMode && activeWireframePipeline != VK_NULL_HANDLE) {
    if (bindWireframeGeometry(p.descriptors.sceneDescriptorSet, p.scene)) {
      drawWireframeLists(p.draws);
    }
    if (hasBimRenderableGeometry(p) &&
        bindWireframeGeometry(p.bim.sceneDescriptorSet, p.bim.scene)) {
      for (const FrameDrawLists *draws : bimSurfaceDrawListSet(p.bim)) {
        drawWireframeLists(*draws);
      }
    }
  }

  recordBimSectionClipCapPass(cmd, p);
  recordBimPointCloudPrimitivePass(cmd, p);
  recordBimCurvePrimitivePass(cmd, p);

  const BimLightingOverlayPlan bimLightingOverlayPlan =
      buildBimLightingOverlayPlan(
          bimLightingOverlayInputs(p, swapChainManager_.extent()));

  const auto drawStyleOverlayRoutes =
      [&](const BimLightingOverlayStylePlan &plan) {
        if (!plan.active) {
          return;
        }
        for (uint32_t routeIndex = 0u; routeIndex < plan.routeCount;
             ++routeIndex) {
          const BimLightingOverlayDrawRoute &route = plan.routes[routeIndex];
          const VkPipeline pipeline =
              pipelineForBimLightingOverlay(route.pipeline, p);
          if (!hasDrawCommands(route.commands) || pipeline == VK_NULL_HANDLE) {
            continue;
          }
          bindWireframePipeline(pipeline, route.rasterLineWidth);
          debugOverlay_.drawWireframe(
              cmd, p.pipeline.layouts.wireframe, *route.commands, plan.color,
              plan.opacity, plan.drawLineWidth, *p.pushConstants.wireframe);
        }
      };

  if ((bimLightingOverlayPlan.pointStyle.active ||
       bimLightingOverlayPlan.curveStyle.active) &&
      bindWireframeGeometry(p.bim.sceneDescriptorSet, p.bim.scene)) {
    drawStyleOverlayRoutes(bimLightingOverlayPlan.pointStyle);
    drawStyleOverlayRoutes(bimLightingOverlayPlan.curveStyle);
  }

  const auto drawOverlayPlan =
      [&](VkDescriptorSet descriptorSet, const FrameSceneGeometry &scene,
          const BimLightingOverlayDrawPlan &plan,
          bool useWireframePipelineBinding) {
        if (!plan.active) {
          return;
        }
        const VkPipeline pipeline =
            pipelineForBimLightingOverlay(plan.pipeline, p);
        if (pipeline == VK_NULL_HANDLE ||
            !bindWireframeGeometry(descriptorSet, scene)) {
          return;
        }

        if (useWireframePipelineBinding) {
          bindWireframePipeline(pipeline, plan.rasterLineWidth);
        } else {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          if (plan.rasterLineWidthApplies) {
            vkCmdSetLineWidth(cmd, plan.rasterLineWidth);
          }
        }
        debugOverlay_.drawWireframe(cmd, p.pipeline.layouts.wireframe,
                                    *plan.commands, plan.color, plan.opacity,
                                    plan.drawLineWidth,
                                    *p.pushConstants.wireframe);
      };

  drawOverlayPlan(p.bim.sceneDescriptorSet, p.bim.scene,
                  bimLightingOverlayPlan.floorPlan, false);
  drawOverlayPlan(p.descriptors.sceneDescriptorSet, p.scene,
                  bimLightingOverlayPlan.sceneHover, true);
  drawOverlayPlan(p.bim.sceneDescriptorSet, p.bim.scene,
                  bimLightingOverlayPlan.bimHover, true);

  // TODO(BIM native GPU visibility): keep native point/curve hover and
  // selection highlights visibility-tested when those primitives move to
  // GPU-owned compaction instead of CPU-filtered explicit draw lists.
  drawOverlayPlan(p.bim.sceneDescriptorSet, p.bim.scene,
                  bimLightingOverlayPlan.nativePointHover, false);
  drawOverlayPlan(p.bim.sceneDescriptorSet, p.bim.scene,
                  bimLightingOverlayPlan.nativeCurveHover, false);

  const auto drawSelectionOutlinePlan =
      [&](VkDescriptorSet descriptorSet, const FrameSceneGeometry &scene,
          const BimLightingSelectionOutlinePlan &plan) {
        if (!plan.active || !bindWireframeGeometry(descriptorSet, scene)) {
          return;
        }

        p.pushConstants.wireframe->padding7 =
            static_cast<float>(plan.framebufferWidth);
        p.pushConstants.wireframe->padding8 =
            static_cast<float>(plan.framebufferHeight);

        vkCmdClearAttachments(cmd, 1, &stencilClearAttachment, 1,
                              &stencilClearRect);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.pipelines.selectionMask);
        debugOverlay_.drawWireframe(cmd, p.pipeline.layouts.wireframe,
                                    *plan.commands, plan.color, 1.0f,
                                    plan.maskLineWidth,
                                    *p.pushConstants.wireframe);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          p.pipeline.pipelines.selectionOutline);
        debugOverlay_.drawWireframe(cmd, p.pipeline.layouts.wireframe,
                                    *plan.commands, plan.color, 1.0f,
                                    plan.outlineLineWidth,
                                    *p.pushConstants.wireframe);
      };
  drawSelectionOutlinePlan(p.descriptors.sceneDescriptorSet, p.scene,
                           bimLightingOverlayPlan.sceneSelectionOutline);
  drawSelectionOutlinePlan(p.bim.sceneDescriptorSet, p.bim.scene,
                           bimLightingOverlayPlan.bimSelectionOutline);
  drawOverlayPlan(p.bim.sceneDescriptorSet, p.bim.scene,
                  bimLightingOverlayPlan.nativePointSelection, false);
  drawOverlayPlan(p.bim.sceneDescriptorSet, p.bim.scene,
                  bimLightingOverlayPlan.nativeCurveSelection, false);

  if (lightingState.lightGizmosEnabled && lightingManager_) {
    lightingManager_->drawLightGizmos(cmd, lightingDescriptorSets,
                                      p.pipeline.pipelines.lightGizmo,
                                      p.pipeline.layouts.lighting, camera_);
  }

  vkCmdEndRenderPass(cmd);
}

void FrameRecorder::recordTransformGizmoPass(VkCommandBuffer cmd,
                                             const FrameRecordParams &p) const {
  const FrameResources *frame = p.runtime.frame;
  recordDeferredTransformGizmoPass(
      {.renderPass = p.renderPasses.transformGizmos,
       .framebuffer = frame ? frame->transformGizmoFramebuffer : VK_NULL_HANDLE,
       .draw = {.commandBuffer = cmd,
                .extent = swapChainManager_.extent(),
                .gizmo = p.transformGizmo,
                .wideLinesSupported = p.debug.wireframeWideLinesSupported,
                .lightingDescriptorSet =
                    frame ? frame->lightingDescriptorSet : VK_NULL_HANDLE,
                .pipelineLayout = p.pipeline.layouts.transformGizmo,
                .pipeline = p.pipeline.pipelines.transformGizmo,
                .solidPipeline = p.pipeline.pipelines.transformGizmoSolid,
                .pushConstants = p.pushConstants.transformGizmo}});
}

void FrameRecorder::recordTransformGizmoOverlay(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  const FrameResources *frame = p.runtime.frame;
  recordDeferredTransformGizmoOverlay(
      {.commandBuffer = cmd,
       .extent = swapChainManager_.extent(),
       .gizmo = p.transformGizmo,
       .wideLinesSupported = p.debug.wireframeWideLinesSupported,
       .lightingDescriptorSet =
           frame ? frame->lightingDescriptorSet : VK_NULL_HANDLE,
       .pipelineLayout = p.pipeline.layouts.transformGizmo,
       .pipeline = p.pipeline.pipelines.transformGizmoOverlay,
       .solidPipeline = p.pipeline.pipelines.transformGizmoSolidOverlay,
       .pushConstants = p.pushConstants.transformGizmo});
}

void FrameRecorder::recordPostProcessPass(
    VkCommandBuffer cmd, const FrameRecordParams &p,
    const std::array<VkDescriptorSet, 2> &postProcessSets) const {
  using container::ui::GBufferViewMode;

  if (!p.swapchain.swapChainFramebuffers ||
      p.runtime.imageIndex >= p.swapchain.swapChainFramebuffers->size()) {
    throw std::runtime_error(
        "invalid swapChainFramebuffers in FrameRecordParams");
  }

  const GBufferViewMode displayMode = currentDisplayMode(guiManager_);
  const container::gpu::ExposureSettings exposureSettings =
      sanitizeExposureSettings(p.postProcess.exposureSettings);
  const auto extent = swapChainManager_.extent();
  const DeferredPostProcessFrameState postProcessState =
      buildDeferredPostProcessFrameState(
          {.displayMode = displayMode,
           .bloomPassActive = isPassActive(RenderPassId::Bloom),
           .bloomReady = bloomManager_ && bloomManager_->isReady(),
           .bloomEnabled = bloomManager_ && bloomManager_->enabled(),
           .bloomIntensity = bloomManager_ ? bloomManager_->intensity() : 0.0f,
           .exposureSettings = exposureSettings,
           .resolvedExposure =
               exposureManager_
                   ? exposureManager_->resolvedExposure(exposureSettings)
                   : resolvePostProcessExposure(exposureSettings),
           .cameraNear = p.camera.nearPlane,
           .cameraFar = p.camera.farPlane,
           .shadowData = p.shadows.shadowData,
           .tileCullPassActive = isPassActive(RenderPassId::TileCull),
           .tiledLightingReady =
               lightingManager_ && lightingManager_->isTiledLightingReady(),
           .framebufferWidth = extent.width,
           .pointLightCount =
               lightingManager_
                   ? static_cast<uint32_t>(
                         lightingManager_->pointLightsSsbo().size())
                   : 0u,
           .transparentOitActive = shouldRecordTransparentOit(p, guiManager_)});

  const DeferredPostProcessPassScope postProcessPass(
      {.commandBuffer = cmd,
       .renderPass = p.renderPasses.postProcess,
       .framebuffer =
           (*p.swapchain.swapChainFramebuffers)[p.runtime.imageIndex],
       .extent = extent});
  postProcessPass.recordFullscreenDraw(
      {.pipeline = p.pipeline.pipelines.postProcess,
       .pipelineLayout = p.pipeline.layouts.postProcess,
       .descriptorSets = postProcessSets,
       .pushConstants = postProcessState.pushConstants});

  recordTransformGizmoOverlay(cmd, p);

  if (guiManager_)
    guiManager_->render(cmd);
}

void FrameRecorder::recordScreenshotCopy(VkCommandBuffer cmd,
                                         const FrameRecordParams &p) const {
  recordScreenshotCaptureCopy({.commandBuffer = cmd,
                               .swapChainImage = p.screenshot.swapChainImage,
                               .readbackBuffer = p.screenshot.readbackBuffer,
                               .extent = p.screenshot.extent});
}

} // namespace container::renderer
