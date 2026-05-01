#include "Container/renderer/RendererFrontend.h"
#include "Container/app/AppConfig.h"
#include "Container/ecs/World.h"
#include "Container/renderer/BloomManager.h"
#include "Container/renderer/CameraController.h"
#include "Container/renderer/CommandBufferManager.h"
#include "Container/renderer/DebugUiPresenter.h"
#include "Container/renderer/EnvironmentManager.h"
#include "Container/renderer/ExposureManager.h"
#include "Container/renderer/FrameConcurrencyPolicy.h"
#include "Container/renderer/FrameRecorder.h"
#include "Container/renderer/GpuCullManager.h"
#include "Container/renderer/GraphicsPipelineBuilder.h"
#include "Container/renderer/LightingManager.h"
#include "Container/renderer/OitManager.h"
#include "Container/renderer/RenderPassGpuProfiler.h"
#include "Container/renderer/RendererTelemetry.h"
#include "Container/renderer/SceneController.h"
#include "Container/renderer/ShadowCullManager.h"
#include "Container/renderer/ShadowManager.h"
#include "Container/renderer/VulkanContextInitializer.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/DebugMessengerExt.h"
#include "Container/utility/FrameSyncManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/Logger.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/SwapChainManager.h"

#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace container::renderer {

using container::gpu::CameraData;
using container::gpu::LightingData;

namespace {

using TelemetryClock = std::chrono::steady_clock;

float elapsedMilliseconds(TelemetryClock::time_point start,
                          TelemetryClock::time_point end =
                              TelemetryClock::now()) {
  return std::chrono::duration<float, std::milli>(end - start).count();
}

uint32_t saturatingU32(size_t value) {
  return static_cast<uint32_t>(
      std::min<size_t>(value, std::numeric_limits<uint32_t>::max()));
}

glm::vec3 arrayToVec3(const std::array<float, 3> &value) {
  return {value[0], value[1], value[2]};
}

container::gpu::ExposureSettings exposureSettingsFromConfig(
    const container::app::AppConfig &config) {
  container::gpu::ExposureSettings settings{};
  if (config.hasManualExposureOverride) {
    settings.mode = container::gpu::kExposureModeManual;
    settings.manualExposure = std::max(config.manualExposure, 0.0f);
  }
  return settings;
}

void applyCameraOverride(container::scene::BaseCamera *camera,
                         const container::app::AppConfig &config) {
  if (!camera || !config.hasCameraOverride) {
    return;
  }

  const glm::vec3 position = arrayToVec3(config.cameraPosition);
  const glm::vec3 target = arrayToVec3(config.cameraTarget);
  glm::vec3 forward = target - position;
  const float length = glm::length(forward);
  if (length <= 0.0001f) {
    return;
  }
  forward /= length;

  const float pitchDegrees =
      glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
  const float yawDegrees = glm::degrees(std::atan2(-forward.z, forward.x));
  camera->setPosition(position);
  camera->setYawPitch(yawDegrees, pitchDegrees);
  if (auto *perspective =
          dynamic_cast<container::scene::PerspectiveCamera *>(camera)) {
    perspective->setFieldOfView(std::clamp(config.cameraVerticalFovDegrees,
                                           1.0f, 179.0f));
  }
}

bool isSupportedScreenshotFormat(VkFormat format) {
  switch (format) {
  case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_R8G8B8A8_UNORM:
    return true;
  default:
    return false;
  }
}

std::vector<unsigned char> convertSwapchainBytesToRgba(
    const unsigned char *src, size_t pixelCount, VkFormat format) {
  std::vector<unsigned char> rgba(pixelCount * 4u);
  const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB ||
                    format == VK_FORMAT_B8G8R8A8_UNORM;
  for (size_t i = 0; i < pixelCount; ++i) {
    const unsigned char c0 = src[i * 4u + 0u];
    const unsigned char c1 = src[i * 4u + 1u];
    const unsigned char c2 = src[i * 4u + 2u];
    const unsigned char c3 = src[i * 4u + 3u];
    rgba[i * 4u + 0u] = bgra ? c2 : c0;
    rgba[i * 4u + 1u] = c1;
    rgba[i * 4u + 2u] = bgra ? c0 : c2;
    rgba[i * 4u + 3u] = c3;
  }
  return rgba;
}

bool isRenderPassActive(const FrameRecorder *frameRecorder, RenderPassId id) {
  if (frameRecorder == nullptr)
    return false;
  return frameRecorder->graph().isPassActive(id);
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

} // namespace

RendererFrontend::RendererFrontend(RendererFrontendCreateInfo info)
    : svc_{*info.ctx,
           *info.pipelineManager,
           *info.allocationManager,
           *info.swapChainManager,
           *info.commandBufferManager,
           *info.config,
           info.nativeWindow,
           *info.inputManager} {}

RendererFrontend::~RendererFrontend() { shutdown(); }

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void RendererFrontend::initialize() {
  // Initialization order mirrors runtime dependencies: render passes define
  // attachment compatibility, scene/lighting managers create descriptor
  // layouts, and only then can graphics pipelines be built.
  subs_.renderPassManager =
      std::make_unique<RenderPassManager>(svc_.ctx.deviceWrapper);
  subs_.oitManager = std::make_unique<OitManager>(svc_.ctx.deviceWrapper);
  createRenderPasses();

  subs_.sceneManager = std::make_unique<container::scene::SceneManager>(
      svc_.allocationManager, svc_.pipelineManager, svc_.ctx.deviceWrapper,
      svc_.config);
  subs_.sceneManager->initialize(
      svc_.config.modelPath,
      svc_.config.importScale,
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  sceneState_.indexType = subs_.sceneManager->indexType();

  subs_.sceneController = std::make_unique<SceneController>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      sceneGraph_, *subs_.sceneManager, nullptr, svc_.config);
  subs_.lightingManager = std::make_unique<LightingManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      subs_.sceneManager.get(), sceneGraph_, subs_.sceneController->world());
  {
    auto lightingSettings = subs_.lightingManager->lightingSettings();
    if (svc_.config.hasEnvironmentIntensityOverride) {
      lightingSettings.environmentIntensity = svc_.config.environmentIntensity;
    }
    if (svc_.config.hasDirectionalIntensityOverride) {
      lightingSettings.directionalIntensity = svc_.config.directionalIntensity;
    }
    subs_.lightingManager->setLightingSettings(lightingSettings);
  }
  subs_.shadowManager = std::make_unique<ShadowManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.shadowManager->createResources(
      resources_.gBufferFormats.depthStencil,
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.shadowManager->createFramebuffers(resources_.renderPasses.shadow);
  subs_.shadowCullManager = std::make_unique<ShadowCullManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.shadowCullManager->createResources(
      container::util::executableDirectory(),
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.environmentManager = std::make_unique<EnvironmentManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.commandBufferManager.pool());
  subs_.environmentManager->createResources(
      container::util::executableDirectory());
  {
    const auto exeDir = container::util::executableDirectory();
    const std::filesystem::path hdrPath =
        exeDir / container::app::kDefaultEnvironmentHdrRelativePath;
    if (std::filesystem::exists(hdrPath)) {
      subs_.environmentManager->loadHdrEnvironment(exeDir, hdrPath);
    }
  }
  subs_.gpuCullManager = std::make_unique<GpuCullManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.gpuCullManager->createResources(container::util::executableDirectory());
  subs_.bloomManager = std::make_unique<BloomManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.commandBufferManager.pool());
  subs_.bloomManager->createResources(container::util::executableDirectory());
  subs_.exposureManager = std::make_unique<ExposureManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.exposureManager->createResources(
      container::util::executableDirectory());
  subs_.frameResourceManager = std::make_unique<FrameResourceManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.swapChainManager, svc_.commandBufferManager.pool());
  subs_.frameResourceManager->createDescriptorSetLayouts();
  subs_.frameResourceManager->createGBufferSampler();
  subs_.lightingManager->createDescriptorResources(
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  subs_.lightingManager->createTiledResources(
      container::util::executableDirectory(), svc_.swapChainManager.extent());
  createGraphicsPipelines();
  svc_.swapChainManager.createFramebuffers(resources_.renderPasses.postProcess);

  createCamera();

  if (svc_.config.enableGui) {
    subs_.guiManager = std::make_unique<container::ui::GuiManager>();
    subs_.guiManager->initialize(
        svc_.ctx.instance, svc_.ctx.deviceWrapper->device(),
        svc_.ctx.deviceWrapper->physicalDevice(),
        svc_.ctx.deviceWrapper->graphicsQueue(),
        svc_.ctx.deviceWrapper->queueFamilyIndices().graphicsFamily.value(),
        resources_.renderPasses.postProcess,
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()),
        svc_.nativeWindow, svc_.config.modelPath, svc_.config.importScale);
    subs_.guiManager->setWireframeCapabilities(
        svc_.ctx.wireframeSupported, svc_.ctx.wireframeRasterModeSupported,
        svc_.ctx.wireframeWideLinesSupported);
    if (subs_.environmentManager) {
      subs_.guiManager->setEnvironmentStatus(
          subs_.environmentManager->environmentStatus());
    }
    if (subs_.sceneController)
      subs_.sceneController->setGuiManager(subs_.guiManager.get());
  }

  subs_.frameRecorder = std::make_unique<FrameRecorder>(
      svc_.ctx.deviceWrapper, svc_.swapChainManager, *subs_.oitManager,
      subs_.lightingManager.get(), subs_.environmentManager.get(),
      subs_.sceneController.get(), subs_.gpuCullManager.get(),
      subs_.bloomManager.get(),
      subs_.exposureManager.get(),
      subs_.cameraController ? subs_.cameraController->camera() : nullptr,
      subs_.guiManager.get());

  svc_.commandBufferManager.allocate(svc_.swapChainManager.imageCount());
  svc_.commandBufferManager.configureSecondaryBuffers(
      static_cast<uint32_t>(shadowCascadePassIds().size()), 1);
  subs_.frameSyncManager = std::make_unique<container::gpu::FrameSyncManager>(
      svc_.ctx.deviceWrapper->device(), svc_.config.maxFramesInFlight);
  subs_.frameSyncManager->initialize(
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  frame_.imagesInFlight.assign(svc_.swapChainManager.imageCount(),
                               VK_NULL_HANDLE);
  subs_.renderPassGpuProfiler = std::make_unique<RenderPassGpuProfiler>();
  subs_.renderPassGpuProfiler->initialize(
      svc_.ctx.deviceWrapper->device(), svc_.ctx.instance,
      svc_.ctx.deviceWrapper->physicalDevice(),
      svc_.ctx.deviceWrapper->queueFamilyIndices().graphicsFamily.value(),
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  container::log::ContainerLogger::instance().renderer()->info(
      "GPU pass profiler: {}",
      subs_.renderPassGpuProfiler->backendStatus());
  subs_.rendererTelemetry = std::make_unique<RendererTelemetry>();

  initializeScene();
}

bool RendererFrontend::drawFrame(bool &framebufferResized) {
  const auto frameStart = TelemetryClock::now();
  const auto concurrencyPolicy =
      FrameConcurrencyPolicy::serializedGpuResources(
          "shared object buffer plus GPU cull, tile cull, GTAO, bloom, "
          "exposure, and readback resources are not yet per-frame");
  auto* telemetry = subs_.rendererTelemetry.get();
  if (telemetry) {
    telemetry->beginFrame(
        frame_.submittedFrameCount, frame_.currentFrame,
        svc_.config.maxFramesInFlight,
        concurrencyPolicy.mode() ==
            FrameConcurrencyMode::SerializedGpuResources,
        concurrencyPolicy.reason());
  }

  auto phaseStart = TelemetryClock::now();
  concurrencyPolicy.waitBeforeAcquire(*subs_.frameSyncManager,
                                      frame_.currentFrame);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::WaitForFrame,
                           elapsedMilliseconds(phaseStart));
    if (subs_.renderPassGpuProfiler) {
      const auto gpuTimings = subs_.renderPassGpuProfiler->collectLatest();
      telemetry->setPassGpuTimings(gpuTimings,
                                   subs_.renderPassGpuProfiler->timingSource());
      telemetry->setGpuProfilerStatus(RendererGpuProfilerTelemetry{
          .source = subs_.renderPassGpuProfiler->timingSource(),
          .available = subs_.renderPassGpuProfiler->isReady(),
          .resultLatencyFrames =
              subs_.renderPassGpuProfiler->resultLatencyFrames(),
          .status =
              std::string(subs_.renderPassGpuProfiler->backendStatus()),
      });
    }
  }

  // Collect culling statistics from the previous frame (now safe after fence).
  phaseStart = TelemetryClock::now();
  if (subs_.gpuCullManager)
    subs_.gpuCullManager->collectStats();
  if (subs_.lightingManager)
    subs_.lightingManager->collectStats();
  if (subs_.exposureManager) {
    subs_.exposureManager->collectReadback(
        subs_.guiManager ? subs_.guiManager->exposureSettings()
                         : container::gpu::ExposureSettings{});
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::Readbacks,
                           elapsedMilliseconds(phaseStart));
    if (subs_.gpuCullManager) {
      telemetry->setCullingStats(subs_.gpuCullManager->cullStats());
    }
    if (subs_.lightingManager) {
      telemetry->setLightCullingStats(
          subs_.lightingManager->lightCullingStats());
    }
  }

  uint32_t imageIndex = 0;
  phaseStart = TelemetryClock::now();
  VkResult result = vkAcquireNextImageKHR(
      svc_.ctx.deviceWrapper->device(), svc_.swapChainManager.swapChain(),
      UINT64_MAX, subs_.frameSyncManager->imageAvailable(frame_.currentFrame),
      VK_NULL_HANDLE, &imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::AcquireImage,
                           elapsedMilliseconds(phaseStart));
  }

  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    phaseStart = TelemetryClock::now();
    handleResize();
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
      telemetry->setCpuPhase(RendererTelemetryPhase::Frame,
                             elapsedMilliseconds(frameStart));
      telemetry->endFrame();
    }
    ++frame_.submittedFrameCount;
    return true;
  } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    throw std::runtime_error("failed to acquire swap chain image!");
  }
  if (telemetry) {
    telemetry->setImageIndex(imageIndex);
  }

  phaseStart = TelemetryClock::now();
  if (frame_.imagesInFlight[imageIndex]) {
    vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1,
                    &frame_.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::ImageFenceWait,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  growExactOitNodePoolIfNeeded(imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                           elapsedMilliseconds(phaseStart));
  }

  subs_.frameSyncManager->resetFence(frame_.currentFrame);
  frame_.imagesInFlight[imageIndex] =
      subs_.frameSyncManager->fence(frame_.currentFrame);

  phaseStart = TelemetryClock::now();
  presentSceneControls();
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::Gui,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  updateObjectBuffer();
  updateCameraBuffer(imageIndex);

  if (subs_.lightingManager && subs_.cameraController) {
    subs_.lightingManager->updateLightingDataForActiveCamera();
  }

  if (subs_.shadowManager && subs_.lightingManager && subs_.cameraController) {
    const auto &ld = subs_.lightingManager->lightingData();
    const float aspect =
        static_cast<float>(svc_.swapChainManager.extent().width) /
        static_cast<float>(svc_.swapChainManager.extent().height);
    const container::gpu::ShadowSettings shadowSettings =
        subs_.guiManager ? subs_.guiManager->shadowSettings()
                         : container::gpu::ShadowSettings{};
    subs_.shadowManager->update(subs_.cameraController->camera(), aspect,
                                glm::vec3(ld.directionalDirection),
                                shadowSettings, imageIndex);
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::SceneUpdate,
                           elapsedMilliseconds(phaseStart));
  }

  phaseStart = TelemetryClock::now();
  updateFrameDescriptorSets(imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::DescriptorUpdate,
                           elapsedMilliseconds(phaseStart));
  }

  const bool screenshotThisFrame = screenshot_.pending;
  if (screenshotThisFrame) {
    phaseStart = TelemetryClock::now();
    ensureScreenshotReadbackBuffer(svc_.swapChainManager.extent(),
                                   svc_.swapChainManager.imageFormat());
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
    }
  }

  phaseStart = TelemetryClock::now();
  vkResetCommandBuffer(svc_.commandBufferManager.buffer(imageIndex), 0);
  recordCommandBuffer(svc_.commandBufferManager.buffer(imageIndex), imageIndex);
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::CommandRecord,
                           elapsedMilliseconds(phaseStart));
    if (subs_.frameRecorder) {
      telemetry->setRenderGraph(subs_.frameRecorder->graph());
    }
  }

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  VkPerformanceQuerySubmitInfoKHR performanceSubmitInfo{};
  if (subs_.renderPassGpuProfiler &&
      subs_.renderPassGpuProfiler->usesPerformanceQueries()) {
    performanceSubmitInfo.sType =
        VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR;
    performanceSubmitInfo.counterPassIndex = 0u;
    submitInfo.pNext = &performanceSubmitInfo;
  }

  VkSemaphore waitSemaphores[] = {
      subs_.frameSyncManager->imageAvailable(frame_.currentFrame)};
  VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  submitInfo.waitSemaphoreCount = 1;
  submitInfo.pWaitSemaphores = waitSemaphores;
  submitInfo.pWaitDstStageMask = waitStages;

  VkCommandBuffer cmdHandle = svc_.commandBufferManager.buffer(imageIndex);
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmdHandle;

  VkSemaphore signalSemaphores[] = {
      subs_.frameSyncManager->renderFinishedForImage(imageIndex)};
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;

  phaseStart = TelemetryClock::now();
  if (vkQueueSubmit(svc_.ctx.deviceWrapper->graphicsQueue(), 1, &submitInfo,
                    subs_.frameSyncManager->fence(frame_.currentFrame)) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to submit draw command buffer!");
  }
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::QueueSubmit,
                           elapsedMilliseconds(phaseStart));
  }

  if (screenshotThisFrame) {
    phaseStart = TelemetryClock::now();
    const VkFence fence = subs_.frameSyncManager->fence(frame_.currentFrame);
    if (vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1, &fence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
      throw std::runtime_error("failed to wait for screenshot frame");
    }
    writePendingScreenshotPng();
    if (telemetry) {
      telemetry->setCpuPhase(RendererTelemetryPhase::Screenshot,
                             elapsedMilliseconds(phaseStart));
    }
  }

  phaseStart = TelemetryClock::now();
  result = svc_.swapChainManager.present(
      svc_.ctx.deviceWrapper->presentQueue(), imageIndex,
      subs_.frameSyncManager->renderFinishedForImage(imageIndex));
  if (telemetry) {
    telemetry->setCpuPhase(RendererTelemetryPhase::Present,
                           elapsedMilliseconds(phaseStart));
  }

  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
      framebufferResized) {
    framebufferResized = false;
    phaseStart = TelemetryClock::now();
    handleResize();
    if (telemetry) {
      telemetry->addCpuPhase(RendererTelemetryPhase::ResourceGrowth,
                             elapsedMilliseconds(phaseStart));
    }
  } else if (result != VK_SUCCESS) {
    throw std::runtime_error("failed to present swap chain image!");
  }

  if (telemetry) {
    RendererWorkloadTelemetry workload{};
    if (subs_.sceneController) {
      workload.objectCount =
          saturatingU32(subs_.sceneController->objectData().size());
      workload.opaqueDrawCount =
          saturatingU32(subs_.sceneController->opaqueDrawCommands().size());
      workload.transparentDrawCount = saturatingU32(
          subs_.sceneController->transparentDrawCommands().size());
      workload.totalDrawCount = saturatingU32(
          static_cast<size_t>(workload.opaqueDrawCount) +
          static_cast<size_t>(workload.transparentDrawCount));
    }
    if (subs_.lightingManager) {
      workload.submittedLights =
          subs_.lightingManager->lightCullingStats().submittedLights;
    }
    telemetry->setWorkload(workload);

    RendererResourceTelemetry resources{};
    const VkExtent2D extent = svc_.swapChainManager.extent();
    resources.swapchainWidth = extent.width;
    resources.swapchainHeight = extent.height;
    resources.swapchainImageCount =
        saturatingU32(svc_.swapChainManager.imageCount());
    resources.cameraBufferCount = saturatingU32(buffers_.cameras.size());
    resources.objectBufferCapacity = saturatingU32(buffers_.objectCapacity);
    if (subs_.frameResourceManager &&
        imageIndex < subs_.frameResourceManager->frameCount()) {
      if (const auto* frame = subs_.frameResourceManager->frame(imageIndex)) {
        resources.oitNodeCapacity = frame->oitNodeCapacity;
      }
    }
    telemetry->setResources(resources);
    telemetry->setCpuPhase(RendererTelemetryPhase::Frame,
                           elapsedMilliseconds(frameStart));
    telemetry->endFrame();
  }

  frame_.currentFrame =
      (frame_.currentFrame + 1) % svc_.config.maxFramesInFlight;
  ++frame_.submittedFrameCount;
  return true;
}

void RendererFrontend::handleResize() {
  if (subs_.rendererTelemetry) {
    subs_.rendererTelemetry->noteDeviceWaitIdle();
    subs_.rendererTelemetry->noteSwapchainRecreate();
  }
  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());

  destroyGBufferResources();
  svc_.swapChainManager.recreate(resources_.renderPasses.postProcess);
  ensureCameraBuffers();
  if (subs_.lightingManager) {
    subs_.lightingManager->createDescriptorResources(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  if (subs_.shadowManager) {
    subs_.shadowManager->recreatePerFrameResources(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  if (subs_.shadowCullManager) {
    subs_.shadowCullManager->recreatePerFrameResources(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }
  createFrameResources();
  if (subs_.environmentManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.environmentManager->recreateGtaoTextures(ext.width, ext.height);
  }
  if (subs_.bloomManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.bloomManager->createTextures(ext.width, ext.height);
  }
  svc_.commandBufferManager.reallocate(svc_.swapChainManager.imageCount());

  const uint32_t imageCount =
      static_cast<uint32_t>(svc_.swapChainManager.imageCount());
  if (subs_.guiManager)
    subs_.guiManager->updateSwapchainImageCount(imageCount);

  subs_.frameSyncManager->recreateRenderFinishedSemaphores(
      svc_.swapChainManager.imageCount());
  frame_.imagesInFlight.assign(svc_.swapChainManager.imageCount(),
                               VK_NULL_HANDLE);
  if (subs_.renderPassGpuProfiler) {
    subs_.renderPassGpuProfiler->recreate(
        static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  }

  for (uint32_t imageIndex = 0;
       imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
       ++imageIndex) {
    updateCameraBuffer(imageIndex);
  }
  updateObjectBuffer();
  updateFrameDescriptorSets();
}

void RendererFrontend::processInput(float deltaTime) {
  if (!subs_.cameraController)
    return;

  auto toggleFromKey = [this](int key, bool &previousState, auto &&onToggle) {
    if (!svc_.nativeWindow)
      return;
    const bool keyDown = glfwGetKey(svc_.nativeWindow, key) == GLFW_PRESS;
    if (keyDown && !previousState)
      onToggle();
    previousState = keyDown;
  };

  toggleFromKey(GLFW_KEY_F6, debugState_.directionalOnlyKeyDown, [this]() {
    debugState_.directionalOnly = !debugState_.directionalOnly;
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage(
          debugState_.directionalOnly ? "Debug: directional-only enabled"
                                      : "Debug: directional-only disabled");
    }
  });

  toggleFromKey(
      GLFW_KEY_F7, debugState_.visualizePointLightStencilKeyDown, [this]() {
        debugState_.visualizePointLightStencil =
            !debugState_.visualizePointLightStencil;
        if (subs_.guiManager) {
          subs_.guiManager->setStatusMessage(
              debugState_.visualizePointLightStencil
                  ? "Debug: point-light stencil visualization enabled"
                  : "Debug: point-light stencil visualization disabled");
        }
      });

  toggleFromKey(GLFW_KEY_F8, debugState_.freezeCullingKeyDown, [this]() {
    debugState_.freezeCulling = !debugState_.freezeCulling;
    if (!debugState_.freezeCulling && subs_.gpuCullManager)
      subs_.gpuCullManager->unfreezeCulling();
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage(
          debugState_.freezeCulling
              ? "Debug: culling camera frozen (F8 to unfreeze)"
              : "Debug: culling camera unfrozen");
    }
  });

  if (subs_.guiManager && subs_.guiManager->isCapturingInput() &&
      !svc_.inputManager.isLooking()) {
    return;
  }

  // Input updates the CPU camera object only. GPU camera buffers are uploaded
  // in drawFrame() after all in-flight work has completed, so culling, depth,
  // and G-buffer passes cannot race a previous frame still reading them.
  (void)svc_.inputManager.update(deltaTime);
}

void RendererFrontend::requestScreenshot(std::filesystem::path outputPath) {
  if (outputPath.empty()) {
    throw std::runtime_error("screenshot output path is empty");
  }
  if (!svc_.swapChainManager.supportsTransferSrc()) {
    throw std::runtime_error(
        "swapchain does not support VK_IMAGE_USAGE_TRANSFER_SRC_BIT");
  }
  if (!isSupportedScreenshotFormat(svc_.swapChainManager.imageFormat())) {
    throw std::runtime_error("unsupported swapchain screenshot format");
  }

  screenshot_.outputPath = std::move(outputPath);
  screenshot_.pending = true;
}

bool RendererFrontend::reloadSceneModel(const std::string &path,
                                        float importScale) {
  if (!subs_.sceneController)
    return false;
  const bool result = subs_.sceneController->reloadSceneModel(
      path, importScale, buffers_.object, buffers_.objectCapacity,
      buffers_.cameras.empty() ? container::gpu::AllocatedBuffer{}
                               : buffers_.cameras.front(),
      sceneState_.indexType, sceneState_.rootNode, sceneState_.selectedMeshNode,
      sceneState_.cubeNode);
  syncSceneStateFromController();
  if (subs_.lightingManager) {
    subs_.lightingManager->setRootNode(sceneState_.rootNode);
    subs_.lightingManager->updateLightingData();
    subs_.lightingManager->createLightVolumeGeometry();
  }
  if (result) {
    if (subs_.cameraController)
      subs_.cameraController->resetCameraForScene();
    for (uint32_t imageIndex = 0;
         imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
         ++imageIndex) {
      updateCameraBuffer(imageIndex);
    }
  }
  updateObjectBuffer();
  if (subs_.sceneManager) {
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.object);
  }
  return result;
}

void RendererFrontend::shutdown() {
  if (!svc_.ctx.deviceWrapper)
    return;

  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());

  subs_.frameSyncManager.reset();
  subs_.frameRecorder.reset();
  subs_.renderPassGpuProfiler.reset();

  destroyGBufferResources();
  subs_.frameResourceManager.reset();

  if (subs_.guiManager) {
    subs_.guiManager->shutdown(svc_.ctx.deviceWrapper->device());
    subs_.guiManager.reset();
  }
  subs_.rendererTelemetry.reset();

  subs_.renderPassManager.reset();
  resources_.renderPasses = {};

  subs_.sceneController.reset();
  subs_.sceneManager.reset();
  subs_.lightingManager.reset();
  subs_.shadowCullManager.reset();
  subs_.shadowManager.reset();
  subs_.environmentManager.reset();
  subs_.bloomManager.reset();
  subs_.exposureManager.reset();
  subs_.oitManager.reset();
  subs_.cameraController.reset();
  subs_.pipelineBuilder.reset();

  for (auto &cameraBuffer : buffers_.cameras) {
    if (cameraBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(cameraBuffer);
    }
  }
  buffers_.cameras.clear();
  if (buffers_.object.buffer != VK_NULL_HANDLE)
    svc_.allocationManager.destroyBuffer(buffers_.object);
  if (screenshot_.readbackBuffer.buffer != VK_NULL_HANDLE) {
    svc_.allocationManager.destroyBuffer(screenshot_.readbackBuffer);
  }
  screenshot_.readbackSize = 0;
}

// ---------------------------------------------------------------------------
// Init helpers
// ---------------------------------------------------------------------------

void RendererFrontend::createRenderPasses() {
  resources_.gBufferFormats.depthStencil =
      subs_.renderPassManager->findDepthStencilFormat();
  subs_.renderPassManager->create(
      svc_.swapChainManager.imageFormat(),
      resources_.gBufferFormats.depthStencil,
      resources_.gBufferFormats.sceneColor, resources_.gBufferFormats.albedo,
      resources_.gBufferFormats.normal, resources_.gBufferFormats.material,
      resources_.gBufferFormats.emissive, resources_.gBufferFormats.specular);
  resources_.renderPasses = subs_.renderPassManager->passes();
}

void RendererFrontend::createGraphicsPipelines() {
  if (!subs_.pipelineBuilder) {
    subs_.pipelineBuilder = std::make_unique<GraphicsPipelineBuilder>(
        svc_.ctx.deviceWrapper, svc_.pipelineManager);
  }
  const PipelineDescriptorLayouts descLayouts{
      subs_.sceneManager->descriptorSetLayout(),
      subs_.frameResourceManager->lightingLayout(),
      subs_.lightingManager->lightDescriptorSetLayout(),
      // Some tests or fallback configurations may not create tiled lighting
      // resources. Reuse the regular light layout so pipeline layout creation
      // stays well-formed even when the tiled path is unavailable.
      subs_.lightingManager->isTiledLightingReady()
          ? subs_.lightingManager->tiledDescriptorSetLayout()
          : subs_.lightingManager->lightDescriptorSetLayout(),
      subs_.shadowManager->descriptorSetLayout(),
      subs_.frameResourceManager->postProcessLayout(),
      subs_.frameResourceManager->oitLayout()};
  const PipelineRenderPasses rp{
      resources_.renderPasses.depthPrepass, resources_.renderPasses.gBuffer,
      resources_.renderPasses.shadow, resources_.renderPasses.lighting,
      resources_.renderPasses.postProcess};
  resources_.builtPipelines = subs_.pipelineBuilder->build(
      container::util::executableDirectory(), descLayouts, rp);
}

void RendererFrontend::createCamera() {
  subs_.cameraController = std::make_unique<CameraController>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.swapChainManager,
      sceneGraph_, subs_.sceneManager.get(), subs_.sceneController->world(),
      svc_.inputManager);
  subs_.cameraController->createCamera();
  applyCameraOverride(subs_.cameraController->camera(), svc_.config);
}

void RendererFrontend::initializeScene() {
  buildSceneGraph();
  createSceneBuffers();
  subs_.lightingManager->updateLightingData();
  subs_.lightingManager->createLightVolumeGeometry();
  createFrameResources();
  if (subs_.environmentManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.environmentManager->createGtaoResources(
        container::util::executableDirectory(), ext.width, ext.height);
  }
  if (subs_.bloomManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.bloomManager->createTextures(ext.width, ext.height);
  }
  createGeometryBuffers();
  subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.object);
  updateFrameDescriptorSets();

  container::log::ContainerLogger::instance().renderer()->info(
      "Initializing Vulkan renderer");
  container::log::ContainerLogger::instance().vulkan()->debug(
      "Debugging Vulkan initialization");
}

void RendererFrontend::buildSceneGraph() {
  if (!subs_.sceneController)
    return;
  subs_.sceneController->buildSceneGraph(
      sceneState_.rootNode, sceneState_.selectedMeshNode, sceneState_.cubeNode);
  if (subs_.lightingManager)
    subs_.lightingManager->setRootNode(sceneState_.rootNode);
}

void RendererFrontend::ensureCameraBuffers() {
  const size_t imageCount = svc_.swapChainManager.imageCount();
  if (buffers_.cameras.size() == imageCount)
    return;

  for (auto &cameraBuffer : buffers_.cameras) {
    if (cameraBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(cameraBuffer);
    }
  }

  buffers_.cameras.assign(imageCount, {});
  for (auto &cameraBuffer : buffers_.cameras) {
    cameraBuffer = svc_.allocationManager.createBuffer(
        sizeof(container::gpu::CameraData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
}

void RendererFrontend::createSceneBuffers() {
  ensureCameraBuffers();
  if (subs_.sceneController) {
    subs_.sceneController->createSceneBuffers(
        buffers_.cameras.front(), buffers_.object, buffers_.objectCapacity);
  }
  for (uint32_t imageIndex = 0;
       imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
       ++imageIndex) {
    updateCameraBuffer(imageIndex);
  }
  updateObjectBuffer();
  updateFrameDescriptorSets();
}

void RendererFrontend::createGeometryBuffers() {
  if (!subs_.sceneController)
    return;
  subs_.sceneController->createGeometryBuffers();
  syncSceneStateFromController();
}

void RendererFrontend::createFrameResources() {
  if (subs_.lightingManager) {
    subs_.lightingManager->resizeTiledResources(svc_.swapChainManager.extent());
  }
  subs_.frameResourceManager->create(
      resources_.gBufferFormats, resources_.renderPasses.depthPrepass,
      resources_.renderPasses.gBuffer, resources_.renderPasses.lighting,
      buffers_.cameras, buffers_.object);
}

// ---------------------------------------------------------------------------
// Per-frame helpers
// ---------------------------------------------------------------------------

void RendererFrontend::updateCameraBuffer(uint32_t imageIndex) {
  if (!subs_.cameraController || imageIndex >= buffers_.cameras.size())
    return;
  subs_.cameraController->updateCameraBuffer(buffers_.cameraData,
                                             buffers_.cameras[imageIndex]);
}

void RendererFrontend::updateObjectBuffer() {
  if (!subs_.sceneController)
    return;
  const bool recreated = subs_.sceneController->updateObjectBuffer(
      buffers_.object, buffers_.objectCapacity,
      buffers_.cameras.empty() ? container::gpu::AllocatedBuffer{}
                               : buffers_.cameras.front());
  if (recreated && subs_.sceneManager)
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.object);
  if ((recreated || !buffers_.shadowObjectDescriptorReady) &&
      subs_.shadowCullManager) {
    subs_.shadowCullManager->updateObjectSsboDescriptor(
        buffers_.object.buffer,
        sizeof(container::gpu::ObjectData) * buffers_.objectCapacity);
    buffers_.shadowObjectDescriptorReady = true;
  }
  sceneState_.diagCubeObjectIndex =
      subs_.sceneController->diagCubeObjectIndex();
}

void RendererFrontend::updateFrameDescriptorSets(uint32_t imageIndex) {
  if (subs_.frameResourceManager) {
    if (subs_.lightingManager) {
      auto &lightingData = subs_.lightingManager->lightingData();
      const auto displayMode = currentDisplayMode(subs_.guiManager.get());
      const bool shadowEnabled =
          displayModeRecordsShadowAtlas(displayMode) &&
          std::ranges::any_of(shadowCascadePassIds(), [this](RenderPassId id) {
            return isRenderPassActive(subs_.frameRecorder.get(), id);
          });
      const bool gtaoEnabled =
          displayModeRecordsGtao(displayMode) &&
          isRenderPassActive(subs_.frameRecorder.get(), RenderPassId::GTAO) &&
          subs_.environmentManager &&
          subs_.environmentManager->isGtaoReady() &&
          subs_.environmentManager->isAoEnabled();
      const bool tileCullEnabled =
          displayModeRecordsTileCull(displayMode) &&
          isRenderPassActive(subs_.frameRecorder.get(), RenderPassId::TileCull);
      lightingData.shadowEnabled = shadowEnabled ? 1u : 0u;
      lightingData.gtaoEnabled = gtaoEnabled ? 1u : 0u;
      lightingData.tileCullEnabled = tileCullEnabled ? 1u : 0u;
      lightingData.prefilteredMipCount =
          subs_.environmentManager
              ? subs_.environmentManager->prefilteredMipCount()
              : 1u;
      if (imageIndex == UINT32_MAX) {
        subs_.lightingManager->uploadLightingData();
      } else {
        subs_.lightingManager->uploadLightingData(imageIndex);
      }
    }

    VkImageView shadowView = VK_NULL_HANDLE;
    VkSampler shadowSampler = VK_NULL_HANDLE;
    std::span<const container::gpu::AllocatedBuffer> shadowUbos{};
    if (subs_.shadowManager) {
      shadowView = subs_.shadowManager->shadowAtlasArrayView();
      shadowSampler = subs_.shadowManager->shadowSampler();
      shadowUbos = subs_.shadowManager->shadowUbos();
    }
    VkImageView irradianceView = VK_NULL_HANDLE;
    VkImageView prefilteredView = VK_NULL_HANDLE;
    VkImageView brdfLutView = VK_NULL_HANDLE;
    VkSampler envSampler = VK_NULL_HANDLE;
    VkSampler brdfLutSampler = VK_NULL_HANDLE;
    VkImageView aoTextureView = VK_NULL_HANDLE;
    VkSampler aoSampler = VK_NULL_HANDLE;
    if (subs_.environmentManager && subs_.environmentManager->isReady()) {
      irradianceView = subs_.environmentManager->irradianceView();
      prefilteredView = subs_.environmentManager->prefilteredView();
      brdfLutView = subs_.environmentManager->brdfLutView();
      envSampler = subs_.environmentManager->envSampler();
      brdfLutSampler = subs_.environmentManager->brdfLutSampler();
    }
    if (subs_.environmentManager && subs_.environmentManager->isGtaoReady() &&
        isRenderPassActive(subs_.frameRecorder.get(), RenderPassId::GTAO)) {
      aoTextureView = subs_.environmentManager->aoTextureView();
      aoSampler = subs_.environmentManager->aoSampler();
    }
    VkImageView bloomTextureView = VK_NULL_HANDLE;
    VkSampler bloomSampler = VK_NULL_HANDLE;
    if (subs_.bloomManager && subs_.bloomManager->isReady()) {
      bloomTextureView = subs_.bloomManager->bloomResultView();
      bloomSampler = subs_.bloomManager->bloomSampler();
    }
    VkBuffer tileGridBuffer = VK_NULL_HANDLE;
    VkDeviceSize tileGridBufferSize = 0;
    if (subs_.lightingManager &&
        subs_.lightingManager->isTiledLightingReady() &&
        isRenderPassActive(subs_.frameRecorder.get(), RenderPassId::TileCull)) {
      tileGridBuffer = subs_.lightingManager->tileGridBuffer();
      tileGridBufferSize = subs_.lightingManager->tileGridBufferSize();
    }
    VkBuffer exposureStateBuffer = VK_NULL_HANDLE;
    VkDeviceSize exposureStateBufferSize = 0;
    if (subs_.exposureManager && subs_.exposureManager->isReady()) {
      exposureStateBuffer = subs_.exposureManager->exposureStateBuffer();
      exposureStateBufferSize =
          subs_.exposureManager->exposureStateBufferSize();
    }
    subs_.frameResourceManager->updateDescriptorSets(
        buffers_.cameras, buffers_.object, shadowView, shadowSampler,
        shadowUbos, irradianceView, prefilteredView, brdfLutView, envSampler,
        brdfLutSampler, aoTextureView, aoSampler, bloomTextureView,
        bloomSampler, tileGridBuffer, tileGridBufferSize,
        exposureStateBuffer, exposureStateBufferSize);
  }
}

void RendererFrontend::destroyGBufferResources() {
  if (subs_.frameResourceManager)
    subs_.frameResourceManager->destroy();
}

bool RendererFrontend::growExactOitNodePoolIfNeeded(uint32_t imageIndex) {
  if (!subs_.frameResourceManager)
    return false;
  const bool grew = subs_.frameResourceManager->growOitPoolIfNeeded(imageIndex);
  if (grew) {
    vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());
    createFrameResources();
    updateFrameDescriptorSets();
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage(
          "Expanded exact OIT node pool to " +
          std::to_string(subs_.frameResourceManager->oitNodeCapacityFloor()));
    }
  }
  return grew;
}

void RendererFrontend::ensureScreenshotReadbackBuffer(VkExtent2D extent,
                                                      VkFormat format) {
  if (extent.width == 0 || extent.height == 0) {
    throw std::runtime_error("cannot capture a zero-sized swapchain image");
  }
  if (!isSupportedScreenshotFormat(format)) {
    throw std::runtime_error("unsupported swapchain screenshot format");
  }

  const VkDeviceSize requiredSize =
      static_cast<VkDeviceSize>(extent.width) *
      static_cast<VkDeviceSize>(extent.height) * 4u;
  if (screenshot_.readbackBuffer.buffer != VK_NULL_HANDLE &&
      screenshot_.readbackSize == requiredSize &&
      screenshot_.extent.width == extent.width &&
      screenshot_.extent.height == extent.height &&
      screenshot_.format == format) {
    return;
  }

  if (screenshot_.readbackBuffer.buffer != VK_NULL_HANDLE) {
    svc_.allocationManager.destroyBuffer(screenshot_.readbackBuffer);
  }
  screenshot_.readbackBuffer = svc_.allocationManager.createBuffer(
      requiredSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  screenshot_.readbackSize = requiredSize;
  screenshot_.extent = extent;
  screenshot_.format = format;
}

void RendererFrontend::writePendingScreenshotPng() {
  if (!screenshot_.pending) {
    return;
  }
  if (screenshot_.readbackBuffer.buffer == VK_NULL_HANDLE ||
      screenshot_.readbackBuffer.allocation == nullptr ||
      screenshot_.readbackSize == 0) {
    throw std::runtime_error("screenshot readback buffer is not initialized");
  }

  void *mapped = screenshot_.readbackBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     screenshot_.readbackBuffer.allocation, &mapped) !=
        VK_SUCCESS) {
      throw std::runtime_error("failed to map screenshot readback buffer");
    }
    mappedHere = true;
  }

  if (vmaInvalidateAllocation(svc_.allocationManager.memoryManager()->allocator(),
                              screenshot_.readbackBuffer.allocation, 0,
                              screenshot_.readbackSize) != VK_SUCCESS) {
    if (mappedHere) {
      vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     screenshot_.readbackBuffer.allocation);
    }
    throw std::runtime_error("failed to invalidate screenshot readback buffer");
  }

  const auto *src = static_cast<const unsigned char *>(mapped);
  const size_t pixelCount =
      static_cast<size_t>(screenshot_.extent.width) *
      static_cast<size_t>(screenshot_.extent.height);
  std::vector<unsigned char> rgba =
      convertSwapchainBytesToRgba(src, pixelCount, screenshot_.format);

  if (mappedHere) {
    vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                   screenshot_.readbackBuffer.allocation);
  }

  if (!screenshot_.outputPath.parent_path().empty()) {
    std::filesystem::create_directories(screenshot_.outputPath.parent_path());
  }
  const std::string outputPath =
      container::util::pathToUtf8(screenshot_.outputPath);
  const int ok = stbi_write_png(outputPath.c_str(),
                                static_cast<int>(screenshot_.extent.width),
                                static_cast<int>(screenshot_.extent.height), 4,
                                rgba.data(),
                                static_cast<int>(screenshot_.extent.width * 4));
  screenshot_.pending = false;
  if (ok == 0) {
    throw std::runtime_error("failed to write screenshot PNG: " + outputPath);
  }
}

void RendererFrontend::presentSceneControls() {
  if (!subs_.guiManager)
    return;

  if (subs_.gpuCullManager) {
    const auto stats = subs_.gpuCullManager->cullStats();
    subs_.guiManager->setCullStats(stats.totalInputCount,
                                   stats.frustumPassedCount,
                                   stats.occlusionPassedCount);
  }
  if (subs_.lightingManager) {
    subs_.guiManager->setLightCullingStats(
        subs_.lightingManager->lightCullingStats());
    subs_.guiManager->setLightingSettings(
        subs_.lightingManager->lightingSettings());
  }
  if (subs_.rendererTelemetry) {
    subs_.guiManager->setRendererTelemetry(subs_.rendererTelemetry->view());
  }

  // Sync freeze-culling: push debug state into GUI before rendering.
  subs_.guiManager->setFreezeCulling(debugState_.freezeCulling);

  // Sync bloom: push BloomManager settings into GUI before rendering.
  if (subs_.bloomManager) {
    subs_.guiManager->setBloomSettings(
        subs_.bloomManager->enabled(), subs_.bloomManager->threshold(),
        subs_.bloomManager->knee(), subs_.bloomManager->intensity(),
        subs_.bloomManager->filterRadius());
  }

  // Sync render pass toggles: push graph pass list into GUI before rendering.
  if (subs_.frameRecorder) {
    DebugUiPresenter::publishRenderPasses(*subs_.guiManager,
                                          subs_.frameRecorder->graph());
  }

  subs_.guiManager->startFrame();
  const std::vector<container::gpu::PointLightData> emptyPointLights;
  const auto &pointLights = subs_.lightingManager
                                ? subs_.lightingManager->pointLightsSsbo()
                                : emptyPointLights;
  subs_.guiManager->drawSceneControls(
      sceneGraph_,
      [this](const std::string &modelPath, float importScale) {
        return reloadSceneModel(modelPath, importScale);
      },
      [this](float importScale) {
        return reloadSceneModel(container::app::DefaultAppConfig().modelPath,
                                importScale);
      },
      subs_.cameraController ? subs_.cameraController->cameraTransformControls()
                             : container::ui::TransformControls{},
      [this](const container::ui::TransformControls &controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyCameraTransform(
              controls, buffers_.cameraData,
              buffers_.cameras.empty() ? container::gpu::AllocatedBuffer{}
                                       : buffers_.cameras.front());
        for (uint32_t imageIndex = 1;
             imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
             ++imageIndex) {
          updateCameraBuffer(imageIndex);
        }
      },
      subs_.cameraController
          ? subs_.cameraController->nodeTransformControls(sceneState_.rootNode)
          : container::ui::TransformControls{},
      [this](const container::ui::TransformControls &controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyNodeTransform(
              sceneState_.rootNode, sceneState_.rootNode, controls);
        if (subs_.lightingManager)
          subs_.lightingManager->updateLightingData();
        updateObjectBuffer();
      },
      subs_.lightingManager ? subs_.lightingManager->directionalLightPosition()
                            : glm::vec3{0.0f},
      subs_.lightingManager ? subs_.lightingManager->lightingData()
                            : container::gpu::LightingData{},
      pointLights, sceneState_.selectedMeshNode,
      [this](uint32_t nodeIndex) {
        if (subs_.cameraController)
          subs_.cameraController->selectMeshNode(nodeIndex,
                                                 sceneState_.selectedMeshNode);
      },
      subs_.cameraController ? subs_.cameraController->nodeTransformControls(
                                   sceneState_.selectedMeshNode)
                             : container::ui::TransformControls{},
      [this](uint32_t nodeIndex,
             const container::ui::TransformControls &controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyNodeTransform(
              nodeIndex, sceneState_.rootNode, controls);
        if (nodeIndex == sceneState_.rootNode && subs_.lightingManager)
          subs_.lightingManager->updateLightingData();
        updateObjectBuffer();
      });

  // Sync freeze-culling: pull GUI state back into debug state (checkbox may
  // have toggled it).
  const bool guiFreeze = subs_.guiManager->freezeCullingRequested();
  if (guiFreeze != debugState_.freezeCulling) {
    debugState_.freezeCulling = guiFreeze;
    if (!guiFreeze && subs_.gpuCullManager)
      subs_.gpuCullManager->unfreezeCulling();
  }

  // Sync bloom: pull GUI state back into BloomManager.
  if (subs_.bloomManager) {
    subs_.bloomManager->enabled() = subs_.guiManager->bloomEnabled();
    subs_.bloomManager->threshold() = subs_.guiManager->bloomThreshold();
    subs_.bloomManager->knee() = subs_.guiManager->bloomKnee();
    subs_.bloomManager->intensity() = subs_.guiManager->bloomIntensity();
    subs_.bloomManager->filterRadius() = subs_.guiManager->bloomRadius();
  }

  if (subs_.lightingManager) {
    const auto &guiLightingSettings = subs_.guiManager->lightingSettings();
    const auto &currentLightingSettings =
        subs_.lightingManager->lightingSettings();
    const bool lightingSettingsChanged =
        guiLightingSettings.preset != currentLightingSettings.preset ||
        guiLightingSettings.density != currentLightingSettings.density ||
        guiLightingSettings.radiusScale !=
            currentLightingSettings.radiusScale ||
        guiLightingSettings.intensityScale !=
            currentLightingSettings.intensityScale ||
        guiLightingSettings.directionalIntensity !=
            currentLightingSettings.directionalIntensity ||
        guiLightingSettings.environmentIntensity !=
            currentLightingSettings.environmentIntensity;
    if (lightingSettingsChanged) {
      subs_.lightingManager->setLightingSettings(guiLightingSettings);
      subs_.lightingManager->updateLightingData();
      updateFrameDescriptorSets();
    }
  }

  if (subs_.guiManager && subs_.environmentManager) {
    subs_.guiManager->setEnvironmentStatus(
        subs_.environmentManager->environmentStatus());
  }

  // Sync render pass toggles: pull GUI toggle states back into the render
  // graph.
  if (subs_.frameRecorder) {
    auto &graph = subs_.frameRecorder->graph();
    if (DebugUiPresenter::applyRenderPassToggles(*subs_.guiManager, graph)) {
      subs_.guiManager->setStatusMessage(
          "Protected passes stay enabled; dependent optional passes are "
          "disabled automatically.");
    }
  }
}

void RendererFrontend::recordCommandBuffer(VkCommandBuffer commandBuffer,
                                           uint32_t imageIndex) {
  if (!subs_.frameRecorder || !subs_.frameResourceManager) {
    throw std::runtime_error(
        "frameRecorder not initialized or image index out of range");
  }
  if (imageIndex >= subs_.frameResourceManager->frameCount()) {
    throw std::runtime_error(
        "frameRecorder not initialized or image index out of range");
  }
  const auto p = buildFrameRecordParams(imageIndex);
  subs_.frameRecorder->record(commandBuffer, p);
}

FrameRecordParams
RendererFrontend::buildFrameRecordParams(uint32_t imageIndex) {
  FrameRecordParams p{};
  p.runtime.frame = subs_.frameResourceManager->frame(imageIndex);
  p.runtime.imageIndex = imageIndex;
  p.scene.vertexSlice = sceneState_.vertexSlice;
  p.scene.indexSlice = sceneState_.indexSlice;
  p.scene.indexType = sceneState_.indexType;
  p.draws.opaqueDrawCommands = &subs_.sceneController->opaqueDrawCommands();
  p.draws.transparentDrawCommands =
      &subs_.sceneController->transparentDrawCommands();
  p.draws.opaqueSingleSidedDrawCommands =
      &subs_.sceneController->opaqueSingleSidedDrawCommands();
  p.draws.opaqueWindingFlippedDrawCommands =
      &subs_.sceneController->opaqueWindingFlippedDrawCommands();
  p.draws.opaqueDoubleSidedDrawCommands =
      &subs_.sceneController->opaqueDoubleSidedDrawCommands();
  p.draws.transparentSingleSidedDrawCommands =
      &subs_.sceneController->transparentSingleSidedDrawCommands();
  p.draws.transparentWindingFlippedDrawCommands =
      &subs_.sceneController->transparentWindingFlippedDrawCommands();
  p.draws.transparentDoubleSidedDrawCommands =
      &subs_.sceneController->transparentDoubleSidedDrawCommands();
  p.scene.objectData = &subs_.sceneController->objectData();
  p.scene.objectDataRevision = subs_.sceneController->objectDataRevision();
  p.descriptors.sceneDescriptorSet =
      subs_.sceneManager->descriptorSet(imageIndex);
  p.descriptors.lightDescriptorSet =
      subs_.lightingManager
          ? subs_.lightingManager->lightDescriptorSet(imageIndex)
          : VK_NULL_HANDLE;
  p.descriptors.tiledDescriptorSet =
      (subs_.lightingManager && subs_.lightingManager->isTiledLightingReady())
          ? subs_.lightingManager->tiledDescriptorSet()
          : VK_NULL_HANDLE;
  p.camera.cameraBuffer = imageIndex < buffers_.cameras.size()
                              ? buffers_.cameras[imageIndex].buffer
                              : VK_NULL_HANDLE;
  p.camera.cameraBufferSize = sizeof(container::gpu::CameraData);
  p.camera.gBufferSampler = subs_.frameResourceManager
                                ? subs_.frameResourceManager->gBufferSampler()
                                : VK_NULL_HANDLE;
  p.renderPasses = {
      resources_.renderPasses.depthPrepass, resources_.renderPasses.gBuffer,
      resources_.renderPasses.shadow, resources_.renderPasses.lighting,
      resources_.renderPasses.postProcess};
  p.pipeline.layouts = resources_.builtPipelines.layouts;
  p.pipeline.pipelines = resources_.builtPipelines.pipelines;
  p.debug.debugDirectionalOnly = debugState_.directionalOnly;
  p.debug.debugVisualizePointLightStencil =
      debugState_.visualizePointLightStencil;
  p.debug.debugFreezeCulling = debugState_.freezeCulling;
  p.debug.wireframeRasterModeSupported =
      svc_.ctx.wireframeRasterModeSupported;
  p.debug.wireframeWideLinesSupported = svc_.ctx.wireframeWideLinesSupported;
  p.pushConstants = pushConstants_.state();
  p.swapchain.swapChainFramebuffers = &svc_.swapChainManager.framebuffers();
  p.scene.diagCubeObjectIndex = sceneState_.diagCubeObjectIndex;
  const auto *activeCamera = subs_.sceneController
                                 ? subs_.sceneController->world().activeCamera()
                                 : nullptr;
  if (activeCamera) {
    p.camera.nearPlane = activeCamera->nearPlane;
    p.camera.farPlane = activeCamera->farPlane;
  } else if (subs_.cameraController) {
    const auto *perspCam =
        dynamic_cast<const container::scene::PerspectiveCamera *>(
            subs_.cameraController->camera());
    if (perspCam) {
      p.camera.nearPlane = perspCam->nearPlane();
      p.camera.farPlane = perspCam->farPlane();
    }
  }
  if (subs_.shadowManager) {
    p.descriptors.shadowDescriptorSet =
        subs_.shadowManager->descriptorSet(imageIndex);
    p.shadows.shadowFramebuffers = subs_.shadowManager->framebuffers().data();
    p.shadows.shadowData = &subs_.shadowManager->shadowData();
    p.shadows.shadowSettings =
        subs_.guiManager ? subs_.guiManager->shadowSettings()
                         : container::gpu::ShadowSettings{};
    p.shadows.shadowManager = subs_.shadowManager.get();
  }
  if (subs_.shadowCullManager) {
    if (subs_.shadowManager) {
      subs_.shadowCullManager->updateShadowCullDescriptor(
          imageIndex, subs_.shadowManager->shadowCullUbo(imageIndex).buffer,
          sizeof(container::gpu::ShadowCullData));
    }
    p.shadows.shadowCullManager = subs_.shadowCullManager.get();
    p.shadows.useGpuShadowCull = subs_.shadowCullManager->isReady();
  }
  p.shadows.useShadowSecondaryCommandBuffers =
      svc_.commandBufferManager.secondaryWorkerCount() >=
      container::gpu::kShadowCascadeCount;
  for (uint32_t cascadeIndex = 0;
       cascadeIndex < container::gpu::kShadowCascadeCount; ++cascadeIndex) {
    p.shadows.shadowSecondaryCommandBuffers[cascadeIndex] =
        svc_.commandBufferManager.secondaryBuffer(imageIndex, cascadeIndex, 0);
  }
  p.services.gpuCullManager = subs_.gpuCullManager.get();
  p.services.bloomManager = subs_.bloomManager.get();
  p.services.telemetry = subs_.rendererTelemetry.get();
  p.services.gpuProfiler = subs_.renderPassGpuProfiler.get();
  p.postProcess.exposureSettings =
      subs_.guiManager ? subs_.guiManager->exposureSettings()
                       : exposureSettingsFromConfig(svc_.config);
  p.scene.objectBuffer = buffers_.object.buffer;
  p.scene.objectBufferSize =
      sizeof(container::gpu::ObjectData) * buffers_.objectCapacity;
  if (screenshot_.pending) {
    p.screenshot.enabled = true;
    p.screenshot.swapChainImage = svc_.swapChainManager.image(imageIndex);
    p.screenshot.readbackBuffer = screenshot_.readbackBuffer.buffer;
    p.screenshot.extent = svc_.swapChainManager.extent();
  }
  return p;
}

// ---------------------------------------------------------------------------
// Scene helpers
// ---------------------------------------------------------------------------

void RendererFrontend::syncSceneStateFromController() {
  if (!subs_.sceneController)
    return;
  sceneState_.vertexSlice = subs_.sceneController->vertexSlice();
  sceneState_.indexSlice = subs_.sceneController->indexSlice();
}

} // namespace container::renderer
