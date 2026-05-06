#include "Container/renderer/shadow/ShadowCascadeFramePassRecorder.h"

#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/shadow/ShadowCascadeGpuCullPlanner.h"
#include "Container/renderer/shadow/ShadowCascadePreparationPlanner.h"
#include "Container/renderer/shadow/ShadowCullManager.h"
#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/renderer/shadow/ShadowPipelineBridge.h"
#include "Container/renderer/shadow/ShadowPassRecorder.h"
#include "Container/renderer/shadow/ShadowResourceBridge.h"
#include "Container/renderer/shadow/ShadowSecondaryCommandBufferPlanner.h"

#include <array>

namespace container::renderer {

using container::gpu::kShadowCascadeCount;

namespace {

[[nodiscard]] bool isPassActive(const ShadowCascadeFramePassContext &context,
                                RenderPassId id) {
  return context.isPassActive ? context.isPassActive(id) : false;
}

[[nodiscard]] bool hasBimShadowGeometry(const FrameRecordParams &p) {
  const auto &bimScene = p.bim.scene;
  return shadowDescriptorSet(p, ShadowDescriptorSetId::BimScene) !=
             VK_NULL_HANDLE &&
         bimScene.vertexSlice.buffer != VK_NULL_HANDLE &&
         bimScene.indexSlice.buffer != VK_NULL_HANDLE &&
         hasBimOpaqueDrawCommands(p.bim);
}

[[nodiscard]] bool
usesGpuFilteredBimMeshShadowPath(const FrameRecordParams &p) {
  return p.bim.opaqueMeshDrawsUseGpuVisibility &&
         p.services.bimManager != nullptr && hasOpaqueDrawCommands(p.bim.draws);
}

[[nodiscard]] const std::vector<DrawCommand> *
primaryOpaqueDrawCommands(const FrameDrawLists &draws) {
  return hasSplitOpaqueDrawCommands(draws) ? draws.opaqueSingleSidedDrawCommands
                                           : draws.opaqueDrawCommands;
}

[[nodiscard]] ShadowCascadeSurfaceDrawLists
sceneShadowCascadeDrawLists(const FrameDrawLists &draws) {
  return {
      .singleSided = draws.opaqueSingleSidedDrawCommands,
      .windingFlipped = draws.opaqueWindingFlippedDrawCommands,
      .doubleSided = draws.opaqueDoubleSidedDrawCommands,
  };
}

[[nodiscard]] ShadowCascadeSurfaceDrawLists
bimShadowCascadeDrawLists(const FrameDrawLists &draws,
                          bool cpuFallbackAllowed) {
  return {
      .singleSided = primaryOpaqueDrawCommands(draws),
      .windingFlipped = draws.opaqueWindingFlippedDrawCommands,
      .doubleSided = draws.opaqueDoubleSidedDrawCommands,
      .cpuFallbackAllowed = cpuFallbackAllowed,
  };
}

[[nodiscard]] ShadowPassGeometryBinding
shadowPassGeometryBinding(VkDescriptorSet descriptorSet,
                          const FrameSceneGeometry &scene) {
  return {.sceneDescriptorSet = descriptorSet,
          .vertexSlice = scene.vertexSlice,
          .indexSlice = scene.indexSlice,
          .indexType = scene.indexType};
}

[[nodiscard]] ShadowPassGpuIndirectBuffers
shadowPassGpuIndirectBuffers(const FrameRecordParams &p,
                             uint32_t cascadeIndex) {
  if (p.shadows.shadowCullManager == nullptr ||
      cascadeIndex >= kShadowCascadeCount) {
    return {};
  }
  return {.drawBuffer =
              p.shadows.shadowCullManager->indirectDrawBuffer(cascadeIndex),
          .countBuffer =
              p.shadows.shadowCullManager->drawCountBuffer(cascadeIndex),
          .maxDrawCount = p.shadows.shadowCullManager->maxDrawCount()};
}

[[nodiscard]] VkPipeline choosePipeline(VkPipeline preferred,
                                        VkPipeline fallback) {
  return preferred != VK_NULL_HANDLE ? preferred : fallback;
}

} // namespace

void ShadowCascadeFramePassRecorder::prepareFrame(
    const FrameRecordParams &params,
    const ShadowCascadeFramePassContext &context) const {
  const auto *shadowGpuCullSourceDrawCommands =
      params.draws.opaqueSingleSidedDrawCommands;
  const ShadowGpuCullSourceUploadPlan shadowGpuCullSourceUploadPlan =
      buildShadowGpuCullSourceUploadPlan(
          {.shadowAtlasVisible = context.shadowAtlasVisible,
           .gpuShadowCullEnabled = params.shadows.useGpuShadowCull,
           .shadowCullManagerReady =
               params.shadows.shadowCullManager != nullptr &&
               params.shadows.shadowCullManager->isReady(),
           .sourceDrawCommandsPresent =
               shadowGpuCullSourceDrawCommands != nullptr,
           .sourceDrawCount =
               shadowGpuCullSourceDrawCommands != nullptr
                   ? static_cast<uint32_t>(
                         shadowGpuCullSourceDrawCommands->size())
                   : 0u});
  if (params.shadows.shadowCullManager != nullptr &&
      shadowGpuCullSourceUploadPlan.uploadSourceDrawCommands &&
      shadowGpuCullSourceDrawCommands != nullptr) {
    // Shadow culling is per-cascade, but all cascades consume the same source
    // draw list. Upload once so each cascade pass can filter into its own
    // indirect buffer.
    params.shadows.shadowCullManager->ensureBufferCapacity(
        shadowGpuCullSourceUploadPlan.requiredDrawCapacity);
    params.shadows.shadowCullManager->uploadDrawCommands(
        *shadowGpuCullSourceDrawCommands);
  }

  if (shouldPrepareDrawCommands(params, context)) {
    prepareDrawCommands(params, context);
  } else {
    clearDrawCommandCache();
  }
  recordShadowCascadeSecondaryPassCommands(
      secondaryPassRecordInputs(params, context));
}

bool ShadowCascadeFramePassRecorder::canRecordCascade(
    const FrameRecordParams &params, uint32_t cascadeIndex) const {
  return cascadeIndex < kShadowCascadeCount &&
         params.shadows.renderPass != VK_NULL_HANDLE &&
         shadowPipelineReady(params, ShadowPipelineId::Depth) &&
         shadowDescriptorSetReady(params, ShadowDescriptorSetId::Shadow) &&
         params.shadows.shadowFramebuffers != nullptr &&
         params.shadows.shadowFramebuffers[cascadeIndex] != VK_NULL_HANDLE;
}

void ShadowCascadeFramePassRecorder::recordCascadePass(
    VkCommandBuffer cmd, const FrameRecordParams &params,
    const ShadowCascadeFramePassContext &context,
    uint32_t cascadeIndex) const {
  static_cast<void>(recordShadowCascadePassCommands(
      cmd, cascadePassRecordInputs(params, context, cascadeIndex)));
}

bool ShadowCascadeFramePassRecorder::shouldPrepareDrawCommands(
    const FrameRecordParams &params,
    const ShadowCascadeFramePassContext &context) const {
  ShadowCascadePreparationPlanInputs inputs{};
  inputs.shadowAtlasVisible = context.shadowAtlasVisible;
  inputs.hasSceneSingleSidedDraws =
      hasDrawCommands(params.draws.opaqueSingleSidedDrawCommands);
  inputs.hasSceneWindingFlippedDraws =
      hasDrawCommands(params.draws.opaqueWindingFlippedDrawCommands);
  inputs.hasSceneDoubleSidedDraws =
      hasDrawCommands(params.draws.opaqueDoubleSidedDrawCommands);
  inputs.hasBimShadowGeometry = hasBimShadowGeometry(params);
  const auto shadowPassIds = shadowCascadePassIds();
  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    if (cascadeIndex >= shadowPassIds.size()) {
      continue;
    }

    inputs.cascades[cascadeIndex] = {
        .shadowPassActive = isPassActive(context, shadowPassIds[cascadeIndex]),
        .shadowPassRecordable = canRecordCascade(params, cascadeIndex),
        .sceneSingleSidedUsesGpuCull =
            useGpuCullForCascade(params, context, cascadeIndex)};
  }
  return buildShadowCascadePreparationPlan(inputs).prepareDrawCommands;
}

bool ShadowCascadeFramePassRecorder::useGpuCullForCascade(
    const FrameRecordParams &params,
    const ShadowCascadeFramePassContext &context,
    uint32_t cascadeIndex) const {
  const auto shadowCullIds = shadowCullPassIds();
  const bool shadowCullPassActive =
      cascadeIndex < shadowCullIds.size() &&
      isPassActive(context, shadowCullIds[cascadeIndex]);
  const bool cascadeIndexInRange = cascadeIndex < kShadowCascadeCount;
  const VkBuffer gpuShadowIndirectBuffer =
      (params.shadows.shadowCullManager != nullptr && cascadeIndexInRange)
          ? params.shadows.shadowCullManager->indirectDrawBuffer(cascadeIndex)
          : VK_NULL_HANDLE;
  const VkBuffer gpuShadowCountBuffer =
      (params.shadows.shadowCullManager != nullptr && cascadeIndexInRange)
          ? params.shadows.shadowCullManager->drawCountBuffer(cascadeIndex)
          : VK_NULL_HANDLE;
  const uint32_t gpuShadowMaxDrawCount =
      params.shadows.shadowCullManager != nullptr
          ? params.shadows.shadowCullManager->maxDrawCount()
          : 0u;

  return buildShadowCascadeGpuCullPlan(
             {.gpuShadowCullEnabled = params.shadows.useGpuShadowCull,
              .shadowCullPassActive = shadowCullPassActive,
              .shadowCullManagerReady =
                  params.shadows.shadowCullManager != nullptr &&
                  params.shadows.shadowCullManager->isReady(),
              .sceneSingleSidedDrawsAvailable =
                  hasDrawCommands(params.draws.opaqueSingleSidedDrawCommands),
              .cascadeIndexInRange = cascadeIndexInRange,
              .indirectDrawBuffer = gpuShadowIndirectBuffer,
              .drawCountBuffer = gpuShadowCountBuffer,
              .maxDrawCount = gpuShadowMaxDrawCount})
      .useGpuCull;
}

size_t ShadowCascadeFramePassRecorder::cpuCommandCount(
    const FrameRecordParams &params,
    const ShadowCascadeFramePassContext &context,
    uint32_t cascadeIndex) const {
  if (cascadeIndex >= kShadowCascadeCount) {
    return 0u;
  }
  return drawPlanCache_.cpuCommandCount(
      cascadeIndex, !useGpuCullForCascade(params, context, cascadeIndex));
}

bool ShadowCascadeFramePassRecorder::shouldUseSecondaryCommandBuffer(
    const FrameRecordParams &params,
    const ShadowCascadeFramePassContext &context,
    uint32_t cascadeIndex) const {
  const bool secondaryCommandBufferAvailable =
      cascadeIndex < params.shadows.shadowSecondaryCommandBuffers.size() &&
      params.shadows.shadowSecondaryCommandBuffers[cascadeIndex] !=
          VK_NULL_HANDLE;

  return buildShadowSecondaryCommandBufferPlan(
             {.usesGpuFilteredBimMeshShadowPath =
                  usesGpuFilteredBimMeshShadowPath(params),
              .secondaryCommandBuffersEnabled =
                  params.shadows.useShadowSecondaryCommandBuffers,
              .shadowPassRecordable = canRecordCascade(params, cascadeIndex),
              .secondaryCommandBufferAvailable =
                  secondaryCommandBufferAvailable,
              .cpuCommandCount =
                  cpuCommandCount(params, context, cascadeIndex)})
      .useSecondaryCommandBuffer;
}

ShadowCascadeDrawPlannerInputs
ShadowCascadeFramePassRecorder::drawPlannerInputs(
    const FrameRecordParams &params,
    const ShadowCascadeFramePassContext &context) const {
  ShadowCascadeDrawPlannerInputs inputs{};
  inputs.scene = {.objectData = params.scene.objectData,
                  .objectDataRevision = params.scene.objectDataRevision};
  inputs.bimScene = {.objectData = params.bim.scene.objectData,
                     .objectDataRevision =
                         params.bim.scene.objectDataRevision};
  inputs.sceneDraws = sceneShadowCascadeDrawLists(params.draws);
  inputs.hasBimShadowGeometry = hasBimShadowGeometry(params);
  inputs.useGpuShadowCull = params.shadows.useGpuShadowCull;
  inputs.shadowManagerIdentity = params.shadows.shadowManager;
  inputs.shadowData = params.shadows.shadowData;

  const auto shadowPassIds = shadowCascadePassIds();
  const auto shadowCullIds = shadowCullPassIds();
  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    inputs.shadowPassActive[cascadeIndex] =
        cascadeIndex < shadowPassIds.size() &&
        isPassActive(context, shadowPassIds[cascadeIndex]);
    inputs.shadowCullPassActive[cascadeIndex] =
        cascadeIndex < shadowCullIds.size() &&
        isPassActive(context, shadowCullIds[cascadeIndex]);
    inputs.sceneSingleSidedUsesGpuCull[cascadeIndex] =
        useGpuCullForCascade(params, context, cascadeIndex);
  }

  uint32_t bimDrawListCount = 0u;
  for (const FrameDrawLists *draws : bimSurfaceDrawListSet(params.bim)) {
    if (bimDrawListCount >= inputs.bimDraws.size()) {
      break;
    }
    inputs.bimDraws[bimDrawListCount] = bimShadowCascadeDrawLists(
        *draws,
        !(draws == &params.bim.draws &&
          params.bim.opaqueMeshDrawsUseGpuVisibility));
    ++bimDrawListCount;
  }
  inputs.bimDrawListCount = bimDrawListCount;

  if (params.shadows.shadowManager != nullptr) {
    inputs.cascadeIntersectsSphere =
        [shadowManager = params.shadows.shadowManager](
            uint32_t cascadeIndex, const glm::vec4 &boundingSphere) {
          return shadowManager->cascadeIntersectsSphere(cascadeIndex,
                                                        boundingSphere);
        };
  }
  return inputs;
}

ShadowCascadePassRecordInputs
ShadowCascadeFramePassRecorder::cascadePassRecordInputs(
    const FrameRecordParams &params,
    const ShadowCascadeFramePassContext &context,
    uint32_t cascadeIndex) const {
  ShadowCascadePassRecordInputs inputs{};
  const auto shadowPassIds = shadowCascadePassIds();
  inputs.cascadePassActive = cascadeIndex < shadowPassIds.size() &&
                             isPassActive(context, shadowPassIds[cascadeIndex]);
  inputs.raster = {
      .shadowAtlasVisible = context.shadowAtlasVisible,
      .shadowPassRecordable = canRecordCascade(params, cascadeIndex),
      .useSecondaryCommandBuffer =
          shouldUseSecondaryCommandBuffer(params, context, cascadeIndex),
      .secondaryCommandBuffer =
          cascadeIndex < params.shadows.shadowSecondaryCommandBuffers.size()
              ? params.shadows.shadowSecondaryCommandBuffers[cascadeIndex]
              : VK_NULL_HANDLE};
  inputs.renderPass = params.shadows.renderPass;
  inputs.framebuffer =
      params.shadows.shadowFramebuffers != nullptr &&
              cascadeIndex < kShadowCascadeCount
          ? params.shadows.shadowFramebuffers[cascadeIndex]
          : VK_NULL_HANDLE;
  inputs.scene = shadowPassGeometryBinding(
      shadowDescriptorSet(params, ShadowDescriptorSetId::Scene), params.scene);
  inputs.bim =
      shadowPassGeometryBinding(
          shadowDescriptorSet(params, ShadowDescriptorSetId::BimScene),
          params.bim.scene);
  inputs.shadowDescriptorSet =
      shadowDescriptorSet(params, ShadowDescriptorSetId::Shadow);
  const VkPipeline primaryPipeline =
      shadowPipelineHandle(params, ShadowPipelineId::Depth);
  inputs.pipelines = {
      .primary = primaryPipeline,
      .frontCull = choosePipeline(
          shadowPipelineHandle(params, ShadowPipelineId::DepthFrontCull),
          primaryPipeline),
      .noCull = choosePipeline(
          shadowPipelineHandle(params, ShadowPipelineId::DepthNoCull),
          primaryPipeline)};
  inputs.pipelineLayout =
      shadowPipelineLayout(params, ShadowPipelineLayoutId::Shadow);
  inputs.pushConstants.cascadeIndex = cascadeIndex;
  if (params.pushConstants.bindless != nullptr) {
    inputs.pushConstants.sectionPlaneEnabled =
        params.pushConstants.bindless->sectionPlaneEnabled;
    inputs.pushConstants.sectionPlane =
        params.pushConstants.bindless->sectionPlane;
  }
  inputs.rasterConstantBias =
      params.shadows.shadowSettings.rasterConstantBias;
  inputs.rasterSlopeBias = params.shadows.shadowSettings.rasterSlopeBias;
  inputs.sceneGpuIndirect = shadowPassGpuIndirectBuffers(params, cascadeIndex);
  inputs.bimManager = params.services.bimManager;

  if (cascadeIndex >= kShadowCascadeCount) {
    return inputs;
  }

  const bool hasSceneShadowGeometry =
      shadowDescriptorSet(params, ShadowDescriptorSetId::Scene) !=
          VK_NULL_HANDLE &&
      params.scene.vertexSlice.buffer != VK_NULL_HANDLE &&
      params.scene.indexSlice.buffer != VK_NULL_HANDLE;
  inputs.drawInputs = {
      .sceneGeometryReady = hasSceneShadowGeometry,
      .bimGeometryReady = hasBimShadowGeometry(params),
      .sceneGpuCullActive = useGpuCullForCascade(params, context, cascadeIndex),
      .bimGpuFilteredMeshActive = usesGpuFilteredBimMeshShadowPath(params),
      .sceneDraws =
          {.singleSided = &drawPlanCache_.sceneSingleSided[cascadeIndex],
           .windingFlipped =
               &drawPlanCache_.sceneWindingFlipped[cascadeIndex],
           .doubleSided = &drawPlanCache_.sceneDoubleSided[cascadeIndex]},
      .bimDraws = {
          .singleSided = &drawPlanCache_.bimSingleSided[cascadeIndex],
          .windingFlipped =
              &drawPlanCache_.bimWindingFlipped[cascadeIndex],
          .doubleSided = &drawPlanCache_.bimDoubleSided[cascadeIndex]}};
  return inputs;
}

ShadowCascadeSecondaryPassRecordInputs
ShadowCascadeFramePassRecorder::secondaryPassRecordInputs(
    const FrameRecordParams &params,
    const ShadowCascadeFramePassContext &context) const {
  ShadowCascadeSecondaryPassRecordInputs inputs{};
  inputs.secondaryCommandBuffersEnabled =
      params.shadows.useShadowSecondaryCommandBuffers;
  for (uint32_t cascadeIndex = 0; cascadeIndex < kShadowCascadeCount;
       ++cascadeIndex) {
    inputs.cascades[cascadeIndex] =
        cascadePassRecordInputs(params, context, cascadeIndex);
  }
  return inputs;
}

void ShadowCascadeFramePassRecorder::prepareDrawCommands(
    const FrameRecordParams &params,
    const ShadowCascadeFramePassContext &context) const {
  const ShadowCascadeDrawPlannerInputs inputs =
      drawPlannerInputs(params, context);
  const ShadowCascadeDrawPlanner planner(inputs);
  if (drawCommandCacheValid_ &&
      drawPlanCache_.signature == planner.signature()) {
    return;
  }

  drawPlanCache_ = planner.build();
  drawCommandCacheValid_ = true;
}

void ShadowCascadeFramePassRecorder::clearDrawCommandCache() const {
  drawPlanCache_ = {};
  drawCommandCacheValid_ = false;
}

} // namespace container::renderer
