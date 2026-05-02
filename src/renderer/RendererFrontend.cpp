#include "Container/renderer/RendererFrontend.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Container/app/AppConfig.h"
#include "Container/ecs/World.h"
#include "Container/renderer/BimManager.h"
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
#include "Container/renderer/RenderSurfaceInteractionController.h"
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

namespace container::renderer {

using container::gpu::CameraData;
using container::gpu::LightingData;

namespace {

using TelemetryClock = std::chrono::steady_clock;

float elapsedMilliseconds(
    TelemetryClock::time_point start,
    TelemetryClock::time_point end = TelemetryClock::now()) {
  return std::chrono::duration<float, std::milli>(end - start).count();
}

uint32_t saturatingU32(size_t value) {
  return static_cast<uint32_t>(
      std::min<size_t>(value, std::numeric_limits<uint32_t>::max()));
}

std::string lowerAscii(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

bool isAuxiliaryRenderModelPath(std::string_view path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos) {
    return false;
  }

  const std::string extension = lowerAscii(path.substr(dot));
  return extension == ".bim" || extension == ".ifc" || extension == ".ifcx" ||
         extension == ".usd" || extension == ".usda" || extension == ".usdc" ||
         extension == ".usdz";
}

glm::vec3 arrayToVec3(const std::array<float, 3>& value) {
  return {value[0], value[1], value[2]};
}

container::gpu::ExposureSettings exposureSettingsFromConfig(
    const container::app::AppConfig& config) {
  container::gpu::ExposureSettings settings{};
  if (config.hasManualExposureOverride) {
    settings.mode = container::gpu::kExposureModeManual;
    settings.manualExposure = std::max(config.manualExposure, 0.0f);
  }
  return settings;
}

void applyCameraOverride(container::scene::BaseCamera* camera,
                         const container::app::AppConfig& config) {
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
  if (auto* perspective =
          dynamic_cast<container::scene::PerspectiveCamera*>(camera)) {
    perspective->setFieldOfView(
        std::clamp(config.cameraVerticalFovDegrees, 1.0f, 179.0f));
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

std::vector<unsigned char> convertSwapchainBytesToRgba(const unsigned char* src,
                                                       size_t pixelCount,
                                                       VkFormat format) {
  std::vector<unsigned char> rgba(pixelCount * 4u);
  const bool bgra =
      format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
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

bool decodeDepthReadbackValue(VkFormat format,
                              const void* data,
                              float& outDepth) {
  if (data == nullptr) {
    return false;
  }

  if (format == VK_FORMAT_D32_SFLOAT ||
      format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
    float depth = 0.0f;
    std::memcpy(&depth, data, sizeof(depth));
    if (!std::isfinite(depth)) {
      return false;
    }
    outDepth = std::clamp(depth, 0.0f, 1.0f);
    return true;
  }

  if (format == VK_FORMAT_D24_UNORM_S8_UINT) {
    uint32_t packed = 0u;
    std::memcpy(&packed, data, sizeof(packed));
    outDepth = static_cast<float>(packed & 0x00ffffffu) /
               static_cast<float>(0x00ffffffu);
    return true;
  }

  return false;
}

bool depthHitVisible(float hitDepth, float sampledDepth) {
  constexpr float kDepthVisibilityTolerance = 0.0005f;
  // Reverse-Z stores nearer surfaces as larger values.
  return sampledDepth <= hitDepth + kDepthVisibilityTolerance;
}

enum class GpuPickTargetKind {
  None,
  Scene,
  Bim,
};

struct GpuPickTarget {
  GpuPickTargetKind kind{GpuPickTargetKind::None};
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
};

GpuPickTarget decodeGpuPickId(uint32_t pickId) {
  if (pickId == container::gpu::kPickIdNone) {
    return {};
  }

  const bool isBim =
      (pickId & container::gpu::kPickIdBimMask) != 0u;
  const uint32_t encodedObject =
      pickId & container::gpu::kPickIdObjectMask;
  if (encodedObject == 0u) {
    return {};
  }

  return GpuPickTarget{
      .kind = isBim ? GpuPickTargetKind::Bim : GpuPickTargetKind::Scene,
      .objectIndex = encodedObject - 1u,
  };
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

struct FrameFeatureReadiness {
  bool shadowAtlas{false};
  bool gtao{false};
  bool tileCull{false};
};

bool hasShadowAtlasResources(const ShadowManager* shadowManager) {
  if (shadowManager == nullptr ||
      shadowManager->shadowAtlasArrayView() == VK_NULL_HANDLE ||
      shadowManager->shadowSampler() == VK_NULL_HANDLE) {
    return false;
  }

  return std::ranges::any_of(shadowManager->shadowUbos(), [](const auto& ubo) {
    return ubo.buffer != VK_NULL_HANDLE;
  });
}

bool hasGtaoResources(const EnvironmentManager* environmentManager) {
  return environmentManager != nullptr && environmentManager->isGtaoReady() &&
         environmentManager->isAoEnabled() &&
         environmentManager->aoTextureView() != VK_NULL_HANDLE &&
         environmentManager->aoSampler() != VK_NULL_HANDLE;
}

bool hasTileCullResources(const LightingManager* lightingManager) {
  return lightingManager != nullptr && lightingManager->isTiledLightingReady() &&
         lightingManager->tileGridBuffer() != VK_NULL_HANDLE &&
         lightingManager->tileGridBufferSize() > 0;
}

bool graphPassScheduled(const FrameRecorder* frameRecorder, RenderPassId id) {
  if (frameRecorder == nullptr) {
    return false;
  }
  const auto* status = frameRecorder->graph().executionStatus(id);
  return status != nullptr && status->active;
}

bool anyShadowCascadeScheduled(const FrameRecorder* frameRecorder) {
  return std::ranges::any_of(shadowCascadePassIds(), [frameRecorder](auto id) {
    return graphPassScheduled(frameRecorder, id);
  });
}

FrameFeatureReadiness evaluateFrameFeatureReadiness(
    container::ui::GBufferViewMode displayMode,
    const FrameRecorder* frameRecorder,
    const ShadowManager* shadowManager,
    const EnvironmentManager* environmentManager,
    const LightingManager* lightingManager) {
  FrameFeatureReadiness readiness{};
  readiness.shadowAtlas =
      displayModeRecordsShadowAtlas(displayMode) &&
      anyShadowCascadeScheduled(frameRecorder) &&
      hasShadowAtlasResources(shadowManager);
  readiness.gtao =
      displayModeRecordsGtao(displayMode) &&
      graphPassScheduled(frameRecorder, RenderPassId::GTAO) &&
      hasGtaoResources(environmentManager);
  readiness.tileCull =
      displayModeRecordsTileCull(displayMode) &&
      graphPassScheduled(frameRecorder, RenderPassId::TileCull) &&
      hasTileCullResources(lightingManager);
  return readiness;
}

bool sameVec4(const glm::vec4& lhs, const glm::vec4& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

bool sameMat4(const glm::mat4& lhs, const glm::mat4& rhs) {
  for (int column = 0; column < 4; ++column) {
    if (!sameVec4(lhs[column], rhs[column])) {
      return false;
    }
  }
  return true;
}

bool sameCameraData(const container::gpu::CameraData& lhs,
                    const container::gpu::CameraData& rhs) {
  return sameMat4(lhs.viewProj, rhs.viewProj) &&
         sameMat4(lhs.inverseViewProj, rhs.inverseViewProj) &&
         sameVec4(lhs.cameraWorldPosition, rhs.cameraWorldPosition) &&
         sameVec4(lhs.cameraForward, rhs.cameraForward);
}

}  // namespace

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
      svc_.config.modelPath, svc_.config.importScale,
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  sceneState_.indexType = subs_.sceneManager->indexType();
  activePrimaryModelPath_ = svc_.config.modelPath;
  activePrimaryImportScale_ = svc_.config.importScale;

  if (!svc_.config.bimModelPath.empty()) {
    subs_.bimManager = std::make_unique<BimManager>(svc_.allocationManager);
    subs_.bimManager->loadModel(svc_.config.bimModelPath,
                                svc_.config.bimImportScale,
                                *subs_.sceneManager);
    activeAuxiliaryModelPath_ = svc_.config.bimModelPath;
    activeAuxiliaryImportScale_ = svc_.config.bimImportScale;
  }

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
      subs_.bloomManager.get(), subs_.exposureManager.get(),
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
      "GPU pass profiler: {}", subs_.renderPassGpuProfiler->backendStatus());
  subs_.rendererTelemetry = std::make_unique<RendererTelemetry>();

  initializeScene();
}

bool RendererFrontend::drawFrame(bool& framebufferResized) {
  const auto frameStart = TelemetryClock::now();
  const auto concurrencyPolicy = FrameConcurrencyPolicy::serializedGpuResources(
      "shared object buffer plus GPU cull, tile cull, GTAO, bloom, "
      "exposure, and readback resources are not yet per-frame");
  auto* telemetry = subs_.rendererTelemetry.get();
  if (telemetry) {
    telemetry->beginFrame(frame_.submittedFrameCount, frame_.currentFrame,
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
          .status = std::string(subs_.renderPassGpuProfiler->backendStatus()),
      });
    }
  }

  // Collect culling statistics from the previous frame (now safe after fence).
  phaseStart = TelemetryClock::now();
  if (subs_.gpuCullManager) subs_.gpuCullManager->collectStats();
  if (subs_.lightingManager) subs_.lightingManager->collectStats();
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
    const auto& ld = subs_.lightingManager->lightingData();
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
  } else {
    markDepthVisibilityFrameComplete(imageIndex);
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
      workload.totalDrawCount =
          saturatingU32(static_cast<size_t>(workload.opaqueDrawCount) +
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
  depthVisibility_.valid = false;
  depthVisibility_.renderFence = VK_NULL_HANDLE;
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
  if (subs_.guiManager) subs_.guiManager->updateSwapchainImageCount(imageCount);

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
  if (subs_.sceneManager) {
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.object);
    if (subs_.bimManager && subs_.bimManager->hasScene()) {
      subs_.sceneManager->updateAuxiliaryDescriptorSets(
          buffers_.cameras, subs_.bimManager->objectAllocatedBuffer());
    }
  }
  updateFrameDescriptorSets();
}

void RendererFrontend::processInput(float deltaTime) {
  if (!subs_.cameraController) {
    svc_.inputManager.endFrame();
    return;
  }

  // Input updates the CPU camera object only. GPU camera buffers are uploaded
  // in drawFrame() after all in-flight work has completed, so culling, depth,
  // and G-buffer passes cannot race a previous frame still reading them.
  interactionController_.process(RenderSurfaceInteractionController::Context{
      .inputManager = svc_.inputManager,
      .cameraController = *subs_.cameraController,
      .debugState = debugState_,
      .deltaTime = deltaTime,
      .guiCapturingMouse =
          subs_.guiManager ? subs_.guiManager->isCapturingMouse() : false,
      .guiCapturingKeyboard =
          subs_.guiManager ? subs_.guiManager->isCapturingKeyboard() : false,
      .selectedMeshNode = sceneState_.selectedMeshNode,
      .hasSelection =
          sceneState_.selectedMeshNode !=
              container::scene::SceneGraph::kInvalidNode ||
          selectedBimObjectIndex_ != std::numeric_limits<uint32_t>::max(),
      .selectAtCursor =
          [this](double cursorX, double cursorY) {
            selectMeshNodeAtCursor(cursorX, cursorY);
          },
      .hoverAtCursor =
          [this](double cursorX, double cursorY) {
            hoverMeshNodeAtCursor(cursorX, cursorY);
          },
      .clearHover =
          [this]() {
            clearHoveredMeshNode();
          },
      .clearSelection =
          [this]() {
            clearSelectedMeshNode();
          },
      .transformSelectedByDrag =
          [this](container::ui::ViewportTool tool,
                 container::ui::TransformSpace space,
                 container::ui::TransformAxis axis, double deltaX,
                 double deltaY) {
            transformSelectedNodeByDrag(tool, space, axis, deltaX, deltaY);
          },
      .setStatusMessage =
          [this](std::string message) {
            if (subs_.guiManager) {
              subs_.guiManager->setStatusMessage(std::move(message));
            }
          },
      .onCullingUnfrozen =
          [this]() {
            if (subs_.gpuCullManager) {
              subs_.gpuCullManager->unfreezeCulling();
            }
          },
  });
}

void RendererFrontend::selectMeshNodeAtCursor(double cursorX, double cursorY) {
  if (!subs_.sceneController) {
    return;
  }

  auto selectBimObject = [&](uint32_t objectIndex) {
    sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
    selectedBimObjectIndex_ = objectIndex;
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage(
          "Selected BIM object " + std::to_string(objectIndex));
    }
  };

  auto selectSceneNode = [&](uint32_t nodeIndex) {
    sceneState_.selectedMeshNode = nodeIndex;
    selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
    clearHoveredMeshNode();
    selectedDrawCommands_.clear();
    selectedBimDrawCommands_.clear();
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage("Selected node " +
                                         std::to_string(nodeIndex));
    }
  };

  uint32_t gpuPickId = container::gpu::kPickIdNone;
  const bool hasGpuPick = samplePickIdAtCursor(cursorX, cursorY, gpuPickId);
  if (hasGpuPick) {
    const GpuPickTarget target = decodeGpuPickId(gpuPickId);
    if (target.kind == GpuPickTargetKind::Bim && subs_.bimManager &&
        target.objectIndex < subs_.bimManager->objectData().size()) {
      selectBimObject(target.objectIndex);
      return;
    }
    if (target.kind == GpuPickTargetKind::Scene) {
      const uint32_t nodeIndex =
          subs_.sceneController->nodeIndexForObject(target.objectIndex);
      if (nodeIndex != container::scene::SceneGraph::kInvalidNode) {
        selectSceneNode(nodeIndex);
        return;
      }
    }

    clearSelectedMeshNode();
    return;
  }

  SceneNodePickHit sceneHit =
      subs_.sceneController->pickRenderableNodeHit(
          buffers_.cameraData, svc_.swapChainManager.extent(), cursorX,
          cursorY);
  BimPickHit bimHit =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->pickRenderableObject(
                buffers_.cameraData, svc_.swapChainManager.extent(), cursorX,
                cursorY)
          : BimPickHit{};

  float sampledDepth = 0.0f;
  if (sampleDepthAtCursor(cursorX, cursorY, sampledDepth)) {
    if (sceneHit.hit && !depthHitVisible(sceneHit.depth, sampledDepth)) {
      sceneHit.hit = false;
    }
    if (bimHit.hit && !depthHitVisible(bimHit.depth, sampledDepth)) {
      bimHit.hit = false;
    }
  }

  if (!sceneHit.hit && !bimHit.hit) {
    clearSelectedMeshNode();
    return;
  }

  if (bimHit.hit && (!sceneHit.hit || bimHit.distance < sceneHit.distance)) {
    selectBimObject(bimHit.objectIndex);
    return;
  }

  selectSceneNode(sceneHit.nodeIndex);
}

void RendererFrontend::hoverMeshNodeAtCursor(double cursorX, double cursorY) {
  if (!subs_.sceneController) {
    clearHoveredMeshNode();
    return;
  }

  const uint64_t objectRevision = subs_.sceneController->objectDataRevision();
  const uint64_t bimObjectRevision =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->objectDataRevision()
          : 0u;
  if (hoverPickCache_.valid && hoverPickCache_.cursorX == cursorX &&
      hoverPickCache_.cursorY == cursorY &&
      hoverPickCache_.selectedMeshNode == sceneState_.selectedMeshNode &&
      hoverPickCache_.selectedBimObjectIndex == selectedBimObjectIndex_ &&
      hoverPickCache_.objectDataRevision == objectRevision &&
      hoverPickCache_.bimObjectDataRevision == bimObjectRevision &&
      sameCameraData(hoverPickCache_.cameraData, buffers_.cameraData)) {
    return;
  }

  hoverPickCache_ = HoverPickCache{
      .valid = true,
      .cursorX = cursorX,
      .cursorY = cursorY,
      .selectedMeshNode = sceneState_.selectedMeshNode,
      .selectedBimObjectIndex = selectedBimObjectIndex_,
      .objectDataRevision = objectRevision,
      .bimObjectDataRevision = bimObjectRevision,
      .cameraData = buffers_.cameraData,
  };

  const SceneNodePickHit sceneHit =
      subs_.sceneController->pickRenderableNodeHit(
          buffers_.cameraData, svc_.swapChainManager.extent(), cursorX,
          cursorY);
  const BimPickHit bimHit =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->pickRenderableObject(
                buffers_.cameraData, svc_.swapChainManager.extent(), cursorX,
                cursorY)
          : BimPickHit{};

  uint32_t hoveredNode = container::scene::SceneGraph::kInvalidNode;
  uint32_t hoveredBimObject = std::numeric_limits<uint32_t>::max();
  if (bimHit.hit && (!sceneHit.hit || bimHit.distance < sceneHit.distance)) {
    hoveredBimObject = bimHit.objectIndex;
  } else if (sceneHit.hit) {
    hoveredNode = sceneHit.nodeIndex;
  }

  if (hoveredNode == sceneState_.selectedMeshNode) {
    hoveredNode = container::scene::SceneGraph::kInvalidNode;
  }
  if (hoveredBimObject == selectedBimObjectIndex_) {
    hoveredBimObject = std::numeric_limits<uint32_t>::max();
  }
  if (hoveredMeshNode_ == hoveredNode &&
      hoveredBimObjectIndex_ == hoveredBimObject) {
    return;
  }

  hoveredMeshNode_ = hoveredNode;
  hoveredBimObjectIndex_ = hoveredBimObject;
  hoveredDrawCommands_.clear();
  hoveredBimDrawCommands_.clear();
}

void RendererFrontend::clearHoveredMeshNode() {
  hoverPickCache_.valid = false;
  if (hoveredMeshNode_ == container::scene::SceneGraph::kInvalidNode &&
      hoveredBimObjectIndex_ == std::numeric_limits<uint32_t>::max()) {
    return;
  }

  hoveredMeshNode_ = container::scene::SceneGraph::kInvalidNode;
  hoveredBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
  hoveredDrawCommands_.clear();
  hoveredBimDrawCommands_.clear();
}

void RendererFrontend::clearSelectedMeshNode() {
  if (sceneState_.selectedMeshNode ==
          container::scene::SceneGraph::kInvalidNode &&
      selectedBimObjectIndex_ == std::numeric_limits<uint32_t>::max()) {
    return;
  }

  sceneState_.selectedMeshNode = container::scene::SceneGraph::kInvalidNode;
  selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
  clearHoveredMeshNode();
  selectedDrawCommands_.clear();
  selectedBimDrawCommands_.clear();
  if (subs_.guiManager) {
    subs_.guiManager->setStatusMessage("Selection cleared");
  }
}

void RendererFrontend::transformSelectedNodeByDrag(
    container::ui::ViewportTool tool, container::ui::TransformSpace space,
    container::ui::TransformAxis axis, double deltaX, double deltaY) {
  if (!subs_.cameraController ||
      sceneState_.selectedMeshNode ==
          container::scene::SceneGraph::kInvalidNode) {
    return;
  }

  auto controls =
      subs_.cameraController->nodeTransformControls(sceneState_.selectedMeshNode);
  auto transformAxisVector = [&]() {
    if (space == container::ui::TransformSpace::Local) {
      if (const auto* node = sceneGraph_.getNode(sceneState_.selectedMeshNode)) {
        glm::vec3 candidate{0.0f};
        switch (axis) {
          case container::ui::TransformAxis::X:
            candidate = glm::vec3(node->worldTransform[0]);
            break;
          case container::ui::TransformAxis::Y:
            candidate = glm::vec3(node->worldTransform[1]);
            break;
          case container::ui::TransformAxis::Z:
            candidate = glm::vec3(node->worldTransform[2]);
            break;
          case container::ui::TransformAxis::Free:
            break;
        }
        if (glm::dot(candidate, candidate) > 0.000001f) {
          return glm::normalize(candidate);
        }
      }
    }

    switch (axis) {
      case container::ui::TransformAxis::X:
        return glm::vec3{1.0f, 0.0f, 0.0f};
      case container::ui::TransformAxis::Y:
        return glm::vec3{0.0f, 1.0f, 0.0f};
      case container::ui::TransformAxis::Z:
        return glm::vec3{0.0f, 0.0f, 1.0f};
      case container::ui::TransformAxis::Free:
        return glm::vec3{1.0f, 0.0f, 0.0f};
    }
    return glm::vec3{1.0f, 0.0f, 0.0f};
  };

  switch (tool) {
    case container::ui::ViewportTool::Select:
      return;
    case container::ui::ViewportTool::Translate: {
      const float scaleHint =
          std::max(0.05f, glm::length(controls.scale) * 0.33333334f);
      const float dragScale = scaleHint * 0.01f;
      if (axis != container::ui::TransformAxis::Free) {
        controls.position +=
            transformAxisVector() * static_cast<float>(deltaX - deltaY) *
            dragScale;
        break;
      }

      glm::vec3 horizontal{1.0f, 0.0f, 0.0f};
      glm::vec3 vertical{0.0f, 1.0f, 0.0f};

      if (space == container::ui::TransformSpace::Local) {
        if (const auto* node = sceneGraph_.getNode(sceneState_.selectedMeshNode)) {
          const glm::vec3 localX = glm::vec3(node->worldTransform[0]);
          const glm::vec3 localY = glm::vec3(node->worldTransform[1]);
          if (glm::dot(localX, localX) > 0.000001f) {
            horizontal = glm::normalize(localX);
          }
          if (glm::dot(localY, localY) > 0.000001f) {
            vertical = glm::normalize(localY);
          }
        }
      } else if (auto* camera = subs_.cameraController->camera()) {
        const glm::vec3 front = camera->frontVector();
        const glm::vec3 up = camera->upVector(front);
        horizontal = camera->rightVector(front, up);
        vertical = up;
      }

      controls.position += horizontal * static_cast<float>(deltaX) * dragScale;
      controls.position -= vertical * static_cast<float>(deltaY) * dragScale;
      break;
    }
    case container::ui::ViewportTool::Rotate: {
      const float amount = static_cast<float>(deltaX - deltaY) * 0.25f;
      switch (axis) {
        case container::ui::TransformAxis::X:
          controls.rotationDegrees.x += amount;
          break;
        case container::ui::TransformAxis::Y:
          controls.rotationDegrees.y += amount;
          break;
        case container::ui::TransformAxis::Z:
          controls.rotationDegrees.z += amount;
          break;
        case container::ui::TransformAxis::Free:
          controls.rotationDegrees.y += static_cast<float>(deltaX) * 0.25f;
          controls.rotationDegrees.x += static_cast<float>(deltaY) * 0.25f;
          break;
      }
      break;
    }
    case container::ui::ViewportTool::Scale: {
      const float factor =
          std::clamp(std::exp(static_cast<float>(deltaX - deltaY) * 0.005f),
                     0.25f, 4.0f);
      if (axis == container::ui::TransformAxis::Free) {
        controls.scale = glm::clamp(controls.scale * factor, glm::vec3(0.001f),
                                    glm::vec3(1000.0f));
      } else {
        auto applyScaleAxis = [&](float& component) {
          component = std::clamp(component * factor, 0.001f, 1000.0f);
        };
        switch (axis) {
          case container::ui::TransformAxis::X:
            applyScaleAxis(controls.scale.x);
            break;
          case container::ui::TransformAxis::Y:
            applyScaleAxis(controls.scale.y);
            break;
          case container::ui::TransformAxis::Z:
            applyScaleAxis(controls.scale.z);
            break;
          case container::ui::TransformAxis::Free:
            break;
        }
      }
      break;
    }
  }

  subs_.cameraController->applyNodeTransform(
      sceneState_.selectedMeshNode, sceneState_.rootNode, controls);
  if (sceneState_.selectedMeshNode == sceneState_.rootNode &&
      subs_.lightingManager) {
    subs_.lightingManager->updateLightingData();
  }
  updateObjectBuffer();
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

bool RendererFrontend::reloadSceneModel(const std::string& path,
                                        float importScale) {
  if (!subs_.sceneController || !subs_.sceneManager) return false;

  const auto cameraBuffer = buffers_.cameras.empty()
                                ? container::gpu::AllocatedBuffer{}
                                : buffers_.cameras.front();
  auto reloadPrimary = [&](const std::string& modelPath, float scale) {
    return subs_.sceneController->reloadSceneModel(
        modelPath, scale, buffers_.object, buffers_.objectCapacity,
        cameraBuffer, sceneState_.indexType, sceneState_.rootNode,
        sceneState_.selectedMeshNode, sceneState_.cubeNode);
  };
  auto refreshSceneState = [&](bool resetCamera) {
    syncSceneStateFromController();
    if (subs_.lightingManager) {
      subs_.lightingManager->setRootNode(sceneState_.rootNode);
      subs_.lightingManager->updateLightingData();
      subs_.lightingManager->createLightVolumeGeometry();
    }
    if (resetCamera) {
      if (subs_.cameraController) subs_.cameraController->resetCameraForScene();
      for (uint32_t imageIndex = 0;
           imageIndex < static_cast<uint32_t>(buffers_.cameras.size());
           ++imageIndex) {
        updateCameraBuffer(imageIndex);
      }
    }
    updateObjectBuffer();
    subs_.sceneManager->updateDescriptorSets(buffers_.cameras, buffers_.object);
    if (subs_.bimManager && subs_.bimManager->hasScene()) {
      subs_.sceneManager->updateAuxiliaryDescriptorSets(
          buffers_.cameras, subs_.bimManager->objectAllocatedBuffer());
    }
  };

  if (isAuxiliaryRenderModelPath(path)) {
    const std::string previousPrimaryPath = activePrimaryModelPath_;
    const float previousPrimaryScale = activePrimaryImportScale_;
    const std::string previousAuxiliaryPath = activeAuxiliaryModelPath_;
    const float previousAuxiliaryScale = activeAuxiliaryImportScale_;

    (void)reloadPrimary("", 1.0f);
    if (!subs_.bimManager) {
      subs_.bimManager = std::make_unique<BimManager>(svc_.allocationManager);
    }

    try {
      subs_.bimManager->loadModel(path, importScale, *subs_.sceneManager);
    } catch (const std::exception&) {
      subs_.bimManager->clear();
      (void)reloadPrimary(previousPrimaryPath, previousPrimaryScale);
      if (!previousAuxiliaryPath.empty()) {
        try {
          subs_.bimManager->loadModel(previousAuxiliaryPath,
                                      previousAuxiliaryScale,
                                      *subs_.sceneManager);
        } catch (const std::exception&) {
          subs_.bimManager->clear();
          activeAuxiliaryModelPath_.clear();
        }
      }
      refreshSceneState(true);
      if (subs_.guiManager) {
        subs_.guiManager->setStatusMessage("Failed to load model: " + path);
      }
      return false;
    }

    activePrimaryModelPath_.clear();
    activePrimaryImportScale_ = 1.0f;
    activeAuxiliaryModelPath_ = path;
    activeAuxiliaryImportScale_ = importScale;
    refreshSceneState(true);
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage("Loaded model: " + path);
    }
    return subs_.bimManager->hasScene();
  }

  const bool result = reloadPrimary(path, importScale);
  if (result) {
    activePrimaryModelPath_ = path;
    activePrimaryImportScale_ = importScale;
    activeAuxiliaryModelPath_.clear();
    activeAuxiliaryImportScale_ = 1.0f;

    if (subs_.bimManager) {
      subs_.bimManager->clear();
    }
    if (!svc_.config.bimModelPath.empty()) {
      if (!subs_.bimManager) {
        subs_.bimManager = std::make_unique<BimManager>(svc_.allocationManager);
      }
      subs_.bimManager->loadModel(svc_.config.bimModelPath,
                                  svc_.config.bimImportScale,
                                  *subs_.sceneManager);
      activeAuxiliaryModelPath_ = svc_.config.bimModelPath;
      activeAuxiliaryImportScale_ = svc_.config.bimImportScale;
    }
  }

  refreshSceneState(result);
  return result;
}

void RendererFrontend::shutdown() {
  if (!svc_.ctx.deviceWrapper) return;

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

  subs_.bimManager.reset();
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

  for (auto& cameraBuffer : buffers_.cameras) {
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
  if (depthVisibility_.readbackBuffer.buffer != VK_NULL_HANDLE) {
    svc_.allocationManager.destroyBuffer(depthVisibility_.readbackBuffer);
  }
  depthVisibility_.readbackSize = 0;
  depthVisibility_.valid = false;
  depthVisibility_.renderFence = VK_NULL_HANDLE;
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
      resources_.gBufferFormats.emissive, resources_.gBufferFormats.specular,
      resources_.gBufferFormats.pickId);
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
  const PipelineRenderPasses rp{resources_.renderPasses.depthPrepass,
                                resources_.renderPasses.bimDepthPrepass,
                                resources_.renderPasses.gBuffer,
                                resources_.renderPasses.bimGBuffer,
                                resources_.renderPasses.transparentPick,
                                resources_.renderPasses.shadow,
                                resources_.renderPasses.lighting,
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
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    subs_.sceneManager->updateAuxiliaryDescriptorSets(
        buffers_.cameras, subs_.bimManager->objectAllocatedBuffer());
  }
  updateFrameDescriptorSets();

  container::log::ContainerLogger::instance().renderer()->info(
      "Initializing Vulkan renderer");
  container::log::ContainerLogger::instance().vulkan()->debug(
      "Debugging Vulkan initialization");
}

void RendererFrontend::buildSceneGraph() {
  if (!subs_.sceneController) return;
  subs_.sceneController->buildSceneGraph(
      sceneState_.rootNode, sceneState_.selectedMeshNode, sceneState_.cubeNode);
  if (subs_.lightingManager)
    subs_.lightingManager->setRootNode(sceneState_.rootNode);
}

void RendererFrontend::ensureCameraBuffers() {
  const size_t imageCount = svc_.swapChainManager.imageCount();
  if (buffers_.cameras.size() == imageCount) return;

  for (auto& cameraBuffer : buffers_.cameras) {
    if (cameraBuffer.buffer != VK_NULL_HANDLE) {
      svc_.allocationManager.destroyBuffer(cameraBuffer);
    }
  }

  buffers_.cameras.assign(imageCount, {});
  for (auto& cameraBuffer : buffers_.cameras) {
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
  if (!subs_.sceneController) return;
  subs_.sceneController->createGeometryBuffers();
  syncSceneStateFromController();
}

void RendererFrontend::createFrameResources() {
  if (subs_.lightingManager) {
    subs_.lightingManager->resizeTiledResources(svc_.swapChainManager.extent());
  }
  subs_.frameResourceManager->create(
      resources_.gBufferFormats, resources_.renderPasses.depthPrepass,
      resources_.renderPasses.bimDepthPrepass, resources_.renderPasses.gBuffer,
      resources_.renderPasses.bimGBuffer,
      resources_.renderPasses.transparentPick, resources_.renderPasses.lighting,
      buffers_.cameras, buffers_.object);
}

// ---------------------------------------------------------------------------
// Per-frame helpers
// ---------------------------------------------------------------------------

void RendererFrontend::updateCameraBuffer(uint32_t imageIndex) {
  if (!subs_.cameraController || imageIndex >= buffers_.cameras.size()) return;
  subs_.cameraController->updateCameraBuffer(buffers_.cameraData,
                                             buffers_.cameras[imageIndex]);
}

void RendererFrontend::updateObjectBuffer() {
  if (!subs_.sceneController) return;
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
    const auto displayMode = currentDisplayMode(subs_.guiManager.get());
    const FrameFeatureReadiness featureReadiness =
        evaluateFrameFeatureReadiness(displayMode, subs_.frameRecorder.get(),
                                      subs_.shadowManager.get(),
                                      subs_.environmentManager.get(),
                                      subs_.lightingManager.get());

    if (subs_.lightingManager) {
      auto& lightingData = subs_.lightingManager->lightingData();
      lightingData.shadowEnabled = featureReadiness.shadowAtlas ? 1u : 0u;
      lightingData.gtaoEnabled = featureReadiness.gtao ? 1u : 0u;
      lightingData.tileCullEnabled = featureReadiness.tileCull ? 1u : 0u;
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
    if (featureReadiness.gtao && subs_.environmentManager) {
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
    if (featureReadiness.tileCull && subs_.lightingManager) {
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
        bloomSampler, tileGridBuffer, tileGridBufferSize, exposureStateBuffer,
        exposureStateBufferSize);
  }
}

void RendererFrontend::destroyGBufferResources() {
  if (subs_.frameResourceManager) subs_.frameResourceManager->destroy();
}

bool RendererFrontend::growExactOitNodePoolIfNeeded(uint32_t imageIndex) {
  if (!subs_.frameResourceManager) return false;
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

  const VkDeviceSize requiredSize = static_cast<VkDeviceSize>(extent.width) *
                                    static_cast<VkDeviceSize>(extent.height) *
                                    4u;
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

  void* mapped = screenshot_.readbackBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     screenshot_.readbackBuffer.allocation,
                     &mapped) != VK_SUCCESS) {
      throw std::runtime_error("failed to map screenshot readback buffer");
    }
    mappedHere = true;
  }

  if (vmaInvalidateAllocation(
          svc_.allocationManager.memoryManager()->allocator(),
          screenshot_.readbackBuffer.allocation, 0,
          screenshot_.readbackSize) != VK_SUCCESS) {
    if (mappedHere) {
      vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     screenshot_.readbackBuffer.allocation);
    }
    throw std::runtime_error("failed to invalidate screenshot readback buffer");
  }

  const auto* src = static_cast<const unsigned char*>(mapped);
  const size_t pixelCount = static_cast<size_t>(screenshot_.extent.width) *
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
  const int ok = stbi_write_png(
      outputPath.c_str(), static_cast<int>(screenshot_.extent.width),
      static_cast<int>(screenshot_.extent.height), 4, rgba.data(),
      static_cast<int>(screenshot_.extent.width * 4));
  screenshot_.pending = false;
  if (ok == 0) {
    throw std::runtime_error("failed to write screenshot PNG: " + outputPath);
  }
}

void RendererFrontend::ensureDepthVisibilityReadbackBuffer() {
  constexpr VkDeviceSize kDepthReadbackSize = sizeof(uint32_t);
  if (depthVisibility_.readbackBuffer.buffer != VK_NULL_HANDLE &&
      depthVisibility_.readbackSize == kDepthReadbackSize) {
    return;
  }

  if (depthVisibility_.readbackBuffer.buffer != VK_NULL_HANDLE) {
    svc_.allocationManager.destroyBuffer(depthVisibility_.readbackBuffer);
  }

  depthVisibility_.readbackBuffer = svc_.allocationManager.createBuffer(
      kDepthReadbackSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  depthVisibility_.readbackSize = kDepthReadbackSize;
}

void RendererFrontend::markDepthVisibilityFrameComplete(uint32_t imageIndex) {
  depthVisibility_.valid = false;
  depthVisibility_.renderFence = VK_NULL_HANDLE;
  if (!subs_.frameResourceManager ||
      imageIndex >= subs_.frameResourceManager->frameCount()) {
    return;
  }

  const auto* frame = subs_.frameResourceManager->frame(imageIndex);
  if (!frame || frame->depthStencil.image == VK_NULL_HANDLE ||
      frame_.imagesInFlight.size() <= imageIndex) {
    return;
  }

  depthVisibility_.imageIndex = imageIndex;
  depthVisibility_.extent = svc_.swapChainManager.extent();
  depthVisibility_.format = resources_.gBufferFormats.depthStencil;
  depthVisibility_.cameraData = buffers_.cameraData;
  depthVisibility_.objectDataRevision =
      subs_.sceneController ? subs_.sceneController->objectDataRevision() : 0u;
  depthVisibility_.bimObjectDataRevision =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->objectDataRevision()
          : 0u;
  depthVisibility_.renderFence = frame_.imagesInFlight[imageIndex];
  depthVisibility_.valid = true;
}

bool RendererFrontend::depthVisibilityFrameMatchesCurrentState() const {
  if (!depthVisibility_.valid || !subs_.sceneController) {
    return false;
  }

  const VkExtent2D extent = svc_.swapChainManager.extent();
  if (depthVisibility_.extent.width != extent.width ||
      depthVisibility_.extent.height != extent.height ||
      depthVisibility_.extent.width == 0 ||
      depthVisibility_.extent.height == 0) {
    return false;
  }
  if (!sameCameraData(depthVisibility_.cameraData, buffers_.cameraData)) {
    return false;
  }
  if (depthVisibility_.objectDataRevision !=
      subs_.sceneController->objectDataRevision()) {
    return false;
  }

  const uint64_t bimRevision =
      (subs_.bimManager && subs_.bimManager->hasScene())
          ? subs_.bimManager->objectDataRevision()
          : 0u;
  return depthVisibility_.bimObjectDataRevision == bimRevision;
}

bool RendererFrontend::sampleDepthAtCursor(double cursorX,
                                           double cursorY,
                                           float& outDepth) {
  if (!depthVisibilityFrameMatchesCurrentState() ||
      !subs_.frameResourceManager ||
      depthVisibility_.imageIndex >= subs_.frameResourceManager->frameCount()) {
    return false;
  }

  const auto* frame =
      subs_.frameResourceManager->frame(depthVisibility_.imageIndex);
  if (!frame || frame->depthStencil.image == VK_NULL_HANDLE ||
      cursorX < 0.0 || cursorY < 0.0 ||
      cursorX >= static_cast<double>(depthVisibility_.extent.width) ||
      cursorY >= static_cast<double>(depthVisibility_.extent.height)) {
    return false;
  }

  if (depthVisibility_.renderFence != VK_NULL_HANDLE) {
    if (vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1,
                        &depthVisibility_.renderFence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
      return false;
    }
  }

  ensureDepthVisibilityReadbackBuffer();
  if (depthVisibility_.readbackBuffer.buffer == VK_NULL_HANDLE ||
      depthVisibility_.readbackBuffer.allocation == nullptr) {
    return false;
  }

  const uint32_t x = std::min<uint32_t>(
      static_cast<uint32_t>(std::floor(cursorX)),
      depthVisibility_.extent.width - 1u);
  const uint32_t y = std::min<uint32_t>(
      static_cast<uint32_t>(std::floor(cursorY)),
      depthVisibility_.extent.height - 1u);

  VkCommandBufferAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocInfo.commandPool = svc_.commandBufferManager.pool();
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  if (vkAllocateCommandBuffers(svc_.ctx.deviceWrapper->device(), &allocInfo,
                               &commandBuffer) != VK_SUCCESS) {
    return false;
  }

  auto freeCommandBuffer = [&]() {
    if (commandBuffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(svc_.ctx.deviceWrapper->device(),
                           svc_.commandBufferManager.pool(), 1,
                           &commandBuffer);
      commandBuffer = VK_NULL_HANDLE;
    }
  };

  VkCommandBufferBeginInfo beginInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_READ_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = frame->depthStencil.image;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy copyRegion{};
  copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  copyRegion.imageSubresource.layerCount = 1;
  copyRegion.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y),
                            0};
  copyRegion.imageExtent = {1u, 1u, 1u};
  vkCmdCopyImageToBuffer(commandBuffer, frame->depthStencil.image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         depthVisibility_.readbackBuffer.buffer, 1,
                         &copyRegion);

  VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.buffer = depthVisibility_.readbackBuffer.buffer;
  hostRead.offset = 0;
  hostRead.size = depthVisibility_.readbackSize;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead,
                       0, nullptr);

  VkImageMemoryBarrier toReadOnly = toTransfer;
  toReadOnly.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toReadOnly.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_SHADER_READ_BIT;
  toReadOnly.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toReadOnly.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toReadOnly);

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  if (vkQueueSubmit(svc_.ctx.deviceWrapper->graphicsQueue(), 1, &submitInfo,
                    VK_NULL_HANDLE) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }
  vkQueueWaitIdle(svc_.ctx.deviceWrapper->graphicsQueue());
  freeCommandBuffer();

  void* mapped = depthVisibility_.readbackBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     depthVisibility_.readbackBuffer.allocation,
                     &mapped) != VK_SUCCESS) {
      return false;
    }
    mappedHere = true;
  }

  const VkResult invalidateResult = vmaInvalidateAllocation(
      svc_.allocationManager.memoryManager()->allocator(),
      depthVisibility_.readbackBuffer.allocation, 0,
      depthVisibility_.readbackSize);
  const bool decoded =
      invalidateResult == VK_SUCCESS &&
      decodeDepthReadbackValue(depthVisibility_.format, mapped, outDepth);

  if (mappedHere) {
    vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                   depthVisibility_.readbackBuffer.allocation);
  }
  return decoded;
}

bool RendererFrontend::samplePickIdAtCursor(double cursorX,
                                            double cursorY,
                                            uint32_t& outPickId) {
  if (!depthVisibilityFrameMatchesCurrentState() ||
      !subs_.frameResourceManager ||
      depthVisibility_.imageIndex >= subs_.frameResourceManager->frameCount()) {
    return false;
  }

  const auto* frame =
      subs_.frameResourceManager->frame(depthVisibility_.imageIndex);
  if (!frame || frame->pickId.image == VK_NULL_HANDLE ||
      cursorX < 0.0 || cursorY < 0.0 ||
      cursorX >= static_cast<double>(depthVisibility_.extent.width) ||
      cursorY >= static_cast<double>(depthVisibility_.extent.height)) {
    return false;
  }

  if (depthVisibility_.renderFence != VK_NULL_HANDLE) {
    if (vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1,
                        &depthVisibility_.renderFence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
      return false;
    }
  }

  ensureDepthVisibilityReadbackBuffer();
  if (depthVisibility_.readbackBuffer.buffer == VK_NULL_HANDLE ||
      depthVisibility_.readbackBuffer.allocation == nullptr) {
    return false;
  }

  const uint32_t x = std::min<uint32_t>(
      static_cast<uint32_t>(std::floor(cursorX)),
      depthVisibility_.extent.width - 1u);
  const uint32_t y = std::min<uint32_t>(
      static_cast<uint32_t>(std::floor(cursorY)),
      depthVisibility_.extent.height - 1u);

  VkCommandBufferAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocInfo.commandPool = svc_.commandBufferManager.pool();
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  if (vkAllocateCommandBuffers(svc_.ctx.deviceWrapper->device(), &allocInfo,
                               &commandBuffer) != VK_SUCCESS) {
    return false;
  }

  auto freeCommandBuffer = [&]() {
    if (commandBuffer != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(svc_.ctx.deviceWrapper->device(),
                           svc_.commandBufferManager.pool(), 1,
                           &commandBuffer);
      commandBuffer = VK_NULL_HANDLE;
    }
  };

  VkCommandBufferBeginInfo beginInfo{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_READ_BIT;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toTransfer.image = frame->pickId.image;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy copyRegion{};
  copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copyRegion.imageSubresource.layerCount = 1;
  copyRegion.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y),
                            0};
  copyRegion.imageExtent = {1u, 1u, 1u};
  vkCmdCopyImageToBuffer(commandBuffer, frame->pickId.image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         depthVisibility_.readbackBuffer.buffer, 1,
                         &copyRegion);

  VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  hostRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  hostRead.buffer = depthVisibility_.readbackBuffer.buffer;
  hostRead.offset = 0;
  hostRead.size = depthVisibility_.readbackSize;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead,
                       0, nullptr);

  VkImageMemoryBarrier toReadOnly = toTransfer;
  toReadOnly.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toReadOnly.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toReadOnly.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toReadOnly.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toReadOnly);

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }

  VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;
  if (vkQueueSubmit(svc_.ctx.deviceWrapper->graphicsQueue(), 1, &submitInfo,
                    VK_NULL_HANDLE) != VK_SUCCESS) {
    freeCommandBuffer();
    return false;
  }
  vkQueueWaitIdle(svc_.ctx.deviceWrapper->graphicsQueue());
  freeCommandBuffer();

  void* mapped = depthVisibility_.readbackBuffer.allocation_info.pMappedData;
  bool mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(svc_.allocationManager.memoryManager()->allocator(),
                     depthVisibility_.readbackBuffer.allocation,
                     &mapped) != VK_SUCCESS) {
      return false;
    }
    mappedHere = true;
  }

  const VkResult invalidateResult = vmaInvalidateAllocation(
      svc_.allocationManager.memoryManager()->allocator(),
      depthVisibility_.readbackBuffer.allocation, 0,
      depthVisibility_.readbackSize);
  if (invalidateResult == VK_SUCCESS) {
    std::memcpy(&outPickId, mapped, sizeof(outPickId));
  }

  if (mappedHere) {
    vmaUnmapMemory(svc_.allocationManager.memoryManager()->allocator(),
                   depthVisibility_.readbackBuffer.allocation);
  }
  return invalidateResult == VK_SUCCESS;
}

void RendererFrontend::presentSceneControls() {
  if (!subs_.guiManager) return;

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
  const auto& pointLights = subs_.lightingManager
                                ? subs_.lightingManager->pointLightsSsbo()
                                : emptyPointLights;
  subs_.guiManager->drawSceneControls(
      sceneGraph_,
      [this](const std::string& modelPath, float importScale) {
        return reloadSceneModel(modelPath, importScale);
      },
      [this](float importScale) {
        return reloadSceneModel(container::app::DefaultAppConfig().modelPath,
                                importScale);
      },
      subs_.cameraController ? subs_.cameraController->cameraTransformControls()
                             : container::ui::TransformControls{},
      [this](const container::ui::TransformControls& controls) {
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
      [this](const container::ui::TransformControls& controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyNodeTransform(
              sceneState_.rootNode, sceneState_.rootNode, controls);
        if (subs_.lightingManager) subs_.lightingManager->updateLightingData();
        updateObjectBuffer();
      },
      subs_.lightingManager ? subs_.lightingManager->directionalLightPosition()
                            : glm::vec3{0.0f},
      subs_.lightingManager ? subs_.lightingManager->lightingData()
                            : container::gpu::LightingData{},
      pointLights, sceneState_.selectedMeshNode,
      [this](uint32_t nodeIndex) {
        if (nodeIndex == container::scene::SceneGraph::kInvalidNode) {
          clearSelectedMeshNode();
          return;
        }
        if (subs_.cameraController) {
          subs_.cameraController->selectMeshNode(nodeIndex,
                                                 sceneState_.selectedMeshNode);
          selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
          clearHoveredMeshNode();
          selectedDrawCommands_.clear();
          selectedBimDrawCommands_.clear();
          if (subs_.guiManager) {
            subs_.guiManager->setStatusMessage(
                "Selected node " + std::to_string(sceneState_.selectedMeshNode));
          }
        }
      },
      subs_.cameraController ? subs_.cameraController->nodeTransformControls(
                                   sceneState_.selectedMeshNode)
                             : container::ui::TransformControls{},
      [this](uint32_t nodeIndex,
             const container::ui::TransformControls& controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyNodeTransform(
              nodeIndex, sceneState_.rootNode, controls);
        if (nodeIndex == sceneState_.rootNode && subs_.lightingManager)
          subs_.lightingManager->updateLightingData();
        updateObjectBuffer();
      });

  subs_.guiManager->drawViewportInteractionControls(
      interactionController_.state(),
      [this](container::ui::ViewportTool tool) {
        interactionController_.setTool(tool);
      },
      [this](container::ui::TransformSpace transformSpace) {
        interactionController_.setTransformSpace(transformSpace);
      },
      [this](container::ui::TransformAxis transformAxis) {
        interactionController_.setTransformAxis(transformAxis);
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
    const auto& guiLightingSettings = subs_.guiManager->lightingSettings();
    const auto& currentLightingSettings =
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
    auto& graph = subs_.frameRecorder->graph();
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

FrameRecordParams RendererFrontend::buildFrameRecordParams(
    uint32_t imageIndex) {
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
  subs_.sceneController->collectDrawCommandsForNode(
      hoveredMeshNode_, hoveredDrawCommands_);
  p.draws.hoveredDrawCommands = &hoveredDrawCommands_;
  subs_.sceneController->collectDrawCommandsForNode(
      sceneState_.selectedMeshNode, selectedDrawCommands_);
  p.draws.selectedDrawCommands = &selectedDrawCommands_;
  p.scene.objectData = &subs_.sceneController->objectData();
  p.scene.objectDataRevision = subs_.sceneController->objectDataRevision();
  p.descriptors.sceneDescriptorSet =
      subs_.sceneManager->descriptorSet(imageIndex);
  if (subs_.bimManager && subs_.bimManager->hasScene()) {
    p.bim.scene.vertexSlice = subs_.bimManager->vertexSlice();
    p.bim.scene.indexSlice = subs_.bimManager->indexSlice();
    p.bim.scene.indexType = subs_.bimManager->indexType();
    p.bim.scene.objectData = &subs_.bimManager->objectData();
    p.bim.scene.objectDataRevision = subs_.bimManager->objectDataRevision();
    p.bim.scene.objectBuffer = subs_.bimManager->objectBuffer();
    p.bim.scene.objectBufferSize = subs_.bimManager->objectBufferSize();
    p.bim.draws.opaqueDrawCommands = &subs_.bimManager->opaqueDrawCommands();
    p.bim.draws.opaqueSingleSidedDrawCommands =
        &subs_.bimManager->opaqueSingleSidedDrawCommands();
    p.bim.draws.opaqueWindingFlippedDrawCommands =
        &subs_.bimManager->opaqueWindingFlippedDrawCommands();
    p.bim.draws.opaqueDoubleSidedDrawCommands =
        &subs_.bimManager->opaqueDoubleSidedDrawCommands();
    p.bim.draws.transparentDrawCommands =
        &subs_.bimManager->transparentDrawCommands();
    p.bim.draws.transparentSingleSidedDrawCommands =
        &subs_.bimManager->transparentSingleSidedDrawCommands();
    p.bim.draws.transparentWindingFlippedDrawCommands =
        &subs_.bimManager->transparentWindingFlippedDrawCommands();
    p.bim.draws.transparentDoubleSidedDrawCommands =
        &subs_.bimManager->transparentDoubleSidedDrawCommands();
    subs_.bimManager->collectDrawCommandsForObject(
        hoveredBimObjectIndex_, hoveredBimDrawCommands_);
    p.bim.draws.hoveredDrawCommands = &hoveredBimDrawCommands_;
    subs_.bimManager->collectDrawCommandsForObject(
        selectedBimObjectIndex_, selectedBimDrawCommands_);
    p.bim.draws.selectedDrawCommands = &selectedBimDrawCommands_;
    p.bim.sceneDescriptorSet =
        subs_.sceneManager->auxiliaryDescriptorSet(imageIndex);
  }
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
  p.renderPasses = {resources_.renderPasses.depthPrepass,
                    resources_.renderPasses.bimDepthPrepass,
                    resources_.renderPasses.gBuffer,
                    resources_.renderPasses.bimGBuffer,
                    resources_.renderPasses.transparentPick,
                    resources_.renderPasses.shadow,
                    resources_.renderPasses.lighting,
                    resources_.renderPasses.postProcess};
  p.pipeline.layouts = resources_.builtPipelines.layouts;
  p.pipeline.pipelines = resources_.builtPipelines.pipelines;
  p.debug.debugDirectionalOnly = debugState_.directionalOnly;
  p.debug.debugVisualizePointLightStencil =
      debugState_.visualizePointLightStencil;
  p.debug.debugFreezeCulling = debugState_.freezeCulling;
  p.debug.wireframeRasterModeSupported = svc_.ctx.wireframeRasterModeSupported;
  p.debug.wireframeWideLinesSupported = svc_.ctx.wireframeWideLinesSupported;
  p.pushConstants = pushConstants_.state();
  p.swapchain.swapChainFramebuffers = &svc_.swapChainManager.framebuffers();
  p.scene.diagCubeObjectIndex = sceneState_.diagCubeObjectIndex;
  const auto* activeCamera = subs_.sceneController
                                 ? subs_.sceneController->world().activeCamera()
                                 : nullptr;
  if (activeCamera) {
    p.camera.nearPlane = activeCamera->nearPlane;
    p.camera.farPlane = activeCamera->farPlane;
  } else if (subs_.cameraController) {
    const auto* perspCam =
        dynamic_cast<const container::scene::PerspectiveCamera*>(
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
    p.shadows.shadowSettings = subs_.guiManager
                                   ? subs_.guiManager->shadowSettings()
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
  if (!subs_.sceneController) return;
  sceneState_.vertexSlice = subs_.sceneController->vertexSlice();
  sceneState_.indexSlice = subs_.sceneController->indexSlice();
}

}  // namespace container::renderer
