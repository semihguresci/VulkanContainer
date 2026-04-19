#include "Container/renderer/RendererFrontend.h"
#include "Container/app/AppConfig.h"
#include "Container/renderer/BloomManager.h"
#include "Container/renderer/CameraController.h"
#include "Container/renderer/FrameRecorder.h"
#include "Container/renderer/CommandBufferManager.h"
#include "Container/renderer/EnvironmentManager.h"
#include "Container/renderer/GraphicsPipelineBuilder.h"
#include "Container/renderer/GpuCullManager.h"
#include "Container/renderer/LightingManager.h"
#include "Container/renderer/OitManager.h"
#include "Container/renderer/SceneController.h"
#include "Container/renderer/ShadowManager.h"
#include "Container/renderer/VulkanContextInitializer.h"
#include "Container/utility/AllocationManager.h"
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

#include <stdexcept>
#include <string>
#include <string_view>

namespace container::renderer {

using container::gpu::CameraData;
using container::gpu::LightingData;

namespace {

bool isProtectedRenderPass(std::string_view name) {
  return name == "DepthPrepass" || name == "GBuffer" ||
         name == "DepthToReadOnly" || name == "Lighting" ||
         name == "OitClear" || name == "OitResolve" ||
         name == "PostProcess";
}

bool isRenderPassEnabled(const FrameRecorder* frameRecorder, std::string_view name) {
  if (frameRecorder == nullptr) return false;
  const auto* pass = frameRecorder->graph().findPass(std::string{name});
  return pass != nullptr && pass->enabled;
}

container::ui::RenderPassToggle* findRenderPassToggle(
    std::vector<container::ui::RenderPassToggle>& toggles,
    std::string_view name) {
  for (auto& toggle : toggles) {
    if (toggle.name == name) return &toggle;
  }
  return nullptr;
}

const container::ui::RenderPassToggle* findRenderPassToggle(
    const std::vector<container::ui::RenderPassToggle>& toggles,
    std::string_view name) {
  for (const auto& toggle : toggles) {
    if (toggle.name == name) return &toggle;
  }
  return nullptr;
}

bool isDisabledByDependency(const std::vector<container::ui::RenderPassToggle>& toggles,
                           std::string_view passName,
                           std::initializer_list<std::string_view> dependencyNames) {
  const auto* pass = findRenderPassToggle(toggles, passName);
  if (pass == nullptr || pass->enabled) return false;

  for (std::string_view dependencyName : dependencyNames) {
    const auto* dependency = findRenderPassToggle(toggles, dependencyName);
    if (dependency != nullptr && !dependency->enabled) {
      return true;
    }
  }
  return false;
}

std::string dependencyNoteForPass(
    const std::vector<container::ui::RenderPassToggle>& toggles,
    std::string_view passName) {
  auto makeNote = [](std::string_view dependencyName) {
    return "requires " + std::string(dependencyName);
  };

  auto firstDisabledDependency = [&](std::initializer_list<std::string_view> dependencyNames)
      -> std::string {
    for (std::string_view dependencyName : dependencyNames) {
      const auto* dependency = findRenderPassToggle(toggles, dependencyName);
      if (dependency != nullptr && !dependency->enabled) {
        return makeNote(dependencyName);
      }
    }
    return {};
  };

  if (passName == "HiZGenerate") {
    return firstDisabledDependency({"DepthPrepass"});
  }
  if (passName == "OcclusionCull") {
    return firstDisabledDependency({"FrustumCull", "HiZGenerate"});
  }
  if (passName == "CullStatsReadback") {
    return firstDisabledDependency({"FrustumCull", "OcclusionCull"});
  }
  if (passName == "ShadowCascade0" || passName == "ShadowCascade1" ||
      passName == "ShadowCascade2" || passName == "ShadowCascade3") {
    return firstDisabledDependency({"FrustumCull"});
  }
  if (passName == "TileCull") {
    return firstDisabledDependency({"DepthToReadOnly"});
  }
  if (passName == "GTAO") {
    return firstDisabledDependency({"DepthToReadOnly", "GBuffer"});
  }
  if (passName == "Bloom") {
    return firstDisabledDependency({"Lighting"});
  }

  return {};
}

bool enforceRenderPassDependencies(
    std::vector<container::ui::RenderPassToggle>& toggles) {
  bool changed = false;

  for (auto& toggle : toggles) {
    if (isProtectedRenderPass(toggle.name) && !toggle.enabled) {
      toggle.enabled = true;
      changed = true;
    }
  }

  bool madeProgress = true;
  while (madeProgress) {
    madeProgress = false;

    auto disableDependentIfRequiredOff = [&](std::string_view passName,
                                             std::string_view dependencyName) {
      auto* dependency = findRenderPassToggle(toggles, dependencyName);
      if (dependency == nullptr || dependency->enabled) return;

      auto* pass = findRenderPassToggle(toggles, passName);
      if (pass != nullptr && pass->enabled) {
        pass->enabled = false;
        changed = true;
        madeProgress = true;
      }
    };

    auto disableDependents = [&](std::string_view passName,
                                 std::initializer_list<std::string_view> dependencyNames) {
      for (std::string_view dependencyName : dependencyNames) {
        disableDependentIfRequiredOff(passName, dependencyName);
      }
    };

    disableDependents("HiZGenerate", {"DepthPrepass"});
    disableDependents("OcclusionCull", {"FrustumCull", "HiZGenerate"});
    disableDependents("CullStatsReadback", {"FrustumCull", "OcclusionCull"});
    disableDependents("ShadowCascade0", {"FrustumCull"});
    disableDependents("ShadowCascade1", {"FrustumCull"});
    disableDependents("ShadowCascade2", {"FrustumCull"});
    disableDependents("ShadowCascade3", {"FrustumCull"});
    disableDependents("TileCull", {"DepthToReadOnly"});
    disableDependents("GTAO", {"DepthToReadOnly", "GBuffer"});
    disableDependents("Bloom", {"Lighting"});
  }

  return changed;
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
           *info.inputManager} {
}

RendererFrontend::~RendererFrontend() {
  shutdown();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void RendererFrontend::initialize() {
  subs_.renderPassManager = std::make_unique<RenderPassManager>(svc_.ctx.deviceWrapper);
  subs_.oitManager        = std::make_unique<OitManager>(svc_.ctx.deviceWrapper);
  createRenderPasses();

  subs_.sceneManager = std::make_unique<container::scene::SceneManager>(
      svc_.allocationManager, svc_.pipelineManager, svc_.ctx.deviceWrapper, svc_.config);
  subs_.sceneManager->initialize(svc_.config.modelPath);
  sceneState_.indexType = subs_.sceneManager->indexType();

  subs_.sceneController = std::make_unique<SceneController>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      sceneGraph_, *subs_.sceneManager, nullptr, svc_.config);
  subs_.lightingManager = std::make_unique<LightingManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      subs_.sceneManager.get(), sceneGraph_);
  subs_.shadowManager = std::make_unique<ShadowManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.shadowManager->createResources(resources_.gBufferFormats.depthStencil);
  subs_.shadowManager->createFramebuffers(resources_.renderPasses.shadow);
  subs_.environmentManager = std::make_unique<EnvironmentManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.commandBufferManager.pool());
  subs_.environmentManager->createResources(container::util::executableDirectory());
  {
    const auto exeDir = container::util::executableDirectory();
    const std::filesystem::path hdrPath =
        exeDir / container::app::kDefaultEnvironmentHdrRelativePath;
    if (std::filesystem::exists(hdrPath)) {
      subs_.environmentManager->loadHdrEnvironment(exeDir, hdrPath);
    } else {
      std::println(stderr, "[HDR] Default environment not found: {}",
                   hdrPath.string());
    }
  }
  subs_.gpuCullManager = std::make_unique<GpuCullManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager);
  subs_.gpuCullManager->createResources(container::util::executableDirectory());
  subs_.bloomManager = std::make_unique<BloomManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.commandBufferManager.pool());
  subs_.bloomManager->createResources(container::util::executableDirectory());
  subs_.frameResourceManager = std::make_unique<FrameResourceManager>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.pipelineManager,
      svc_.swapChainManager, svc_.commandBufferManager.pool());
  subs_.frameResourceManager->createDescriptorSetLayouts();
  subs_.frameResourceManager->createGBufferSampler();
  subs_.lightingManager->createDescriptorResources();
  subs_.lightingManager->createTiledResources(
      container::util::executableDirectory(), svc_.swapChainManager.extent());
  createGraphicsPipelines();
  svc_.swapChainManager.createFramebuffers(resources_.renderPasses.postProcess);

  createCamera();

  subs_.guiManager = std::make_unique<container::ui::GuiManager>();
  subs_.guiManager->initialize(
      svc_.ctx.instance, svc_.ctx.deviceWrapper->device(),
      svc_.ctx.deviceWrapper->physicalDevice(),
      svc_.ctx.deviceWrapper->graphicsQueue(),
      svc_.ctx.deviceWrapper->queueFamilyIndices().graphicsFamily.value(),
      resources_.renderPasses.postProcess,
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()),
      svc_.nativeWindow, svc_.config.modelPath);
  subs_.guiManager->setWireframeCapabilities(svc_.ctx.wireframeSupported,
                                        svc_.ctx.wireframeWideLinesSupported);
  if (subs_.sceneController) subs_.sceneController->setGuiManager(subs_.guiManager.get());

  subs_.frameRecorder = std::make_unique<FrameRecorder>(
      svc_.ctx.deviceWrapper, svc_.swapChainManager, *subs_.oitManager,
      subs_.lightingManager.get(), subs_.environmentManager.get(),
      subs_.sceneController.get(), subs_.gpuCullManager.get(),
      subs_.bloomManager.get(),
      subs_.cameraController ? subs_.cameraController->camera() : nullptr,
      subs_.guiManager.get());

  svc_.commandBufferManager.allocate(svc_.swapChainManager.imageCount());
  subs_.frameSyncManager = std::make_unique<container::gpu::FrameSyncManager>(
      svc_.ctx.deviceWrapper->device(), svc_.config.maxFramesInFlight);
  subs_.frameSyncManager->initialize(
      static_cast<uint32_t>(svc_.swapChainManager.imageCount()));
  frame_.imagesInFlight.assign(svc_.swapChainManager.imageCount(), VK_NULL_HANDLE);

  initializeScene();
}

bool RendererFrontend::drawFrame(bool& framebufferResized) {
  subs_.frameSyncManager->waitForFrame(frame_.currentFrame);

  // Collect culling statistics from the previous frame (now safe after fence).
  if (subs_.gpuCullManager)
    subs_.gpuCullManager->collectStats();
  if (subs_.lightingManager)
    subs_.lightingManager->collectStats();

  uint32_t imageIndex = 0;
  VkResult result = vkAcquireNextImageKHR(
      svc_.ctx.deviceWrapper->device(), svc_.swapChainManager.swapChain(), UINT64_MAX,
      subs_.frameSyncManager->imageAvailable(frame_.currentFrame), VK_NULL_HANDLE,
      &imageIndex);

  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    handleResize();
    return true;
  } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    throw std::runtime_error("failed to acquire swap chain image!");
  }

  if (frame_.imagesInFlight[imageIndex]) {
    vkWaitForFences(svc_.ctx.deviceWrapper->device(), 1,
                    &frame_.imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
  }

  growExactOitNodePoolIfNeeded(imageIndex);

  subs_.frameSyncManager->resetFence(frame_.currentFrame);
  frame_.imagesInFlight[imageIndex] = subs_.frameSyncManager->fence(frame_.currentFrame);

  updateObjectBuffer();
  presentSceneControls();

  if (subs_.lightingManager && subs_.cameraController) {
    subs_.lightingManager->updateLightingData(subs_.cameraController->camera());
  }

  if (subs_.shadowManager && subs_.lightingManager && subs_.cameraController) {
    const auto& ld = subs_.lightingManager->lightingData();
    const float aspect =
        static_cast<float>(svc_.swapChainManager.extent().width) /
        static_cast<float>(svc_.swapChainManager.extent().height);
    subs_.shadowManager->update(
        subs_.cameraController->camera(), aspect,
        glm::vec3(ld.directionalDirection));
  }

  vkResetCommandBuffer(svc_.commandBufferManager.buffer(imageIndex), 0);
  recordCommandBuffer(svc_.commandBufferManager.buffer(imageIndex), imageIndex);

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

  VkSemaphore waitSemaphores[]     = {subs_.frameSyncManager->imageAvailable(frame_.currentFrame)};
  VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  submitInfo.waitSemaphoreCount    = 1;
  submitInfo.pWaitSemaphores       = waitSemaphores;
  submitInfo.pWaitDstStageMask     = waitStages;

  VkCommandBuffer cmdHandle = svc_.commandBufferManager.buffer(imageIndex);
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers    = &cmdHandle;

  VkSemaphore signalSemaphores[] = {subs_.frameSyncManager->renderFinishedForImage(imageIndex)};
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores    = signalSemaphores;

  if (vkQueueSubmit(svc_.ctx.deviceWrapper->graphicsQueue(), 1, &submitInfo,
                    subs_.frameSyncManager->fence(frame_.currentFrame)) != VK_SUCCESS) {
    throw std::runtime_error("failed to submit draw command buffer!");
  }

  result = svc_.swapChainManager.present(
      svc_.ctx.deviceWrapper->presentQueue(), imageIndex,
      subs_.frameSyncManager->renderFinishedForImage(imageIndex));

  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
      framebufferResized) {
    framebufferResized = false;
    handleResize();
  } else if (result != VK_SUCCESS) {
    throw std::runtime_error("failed to present swap chain image!");
  }

  frame_.currentFrame = (frame_.currentFrame + 1) % svc_.config.maxFramesInFlight;
  return true;
}

void RendererFrontend::handleResize() {
  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());

  destroyGBufferResources();
  svc_.swapChainManager.recreate(resources_.renderPasses.postProcess);
  createFrameResources();
  if (subs_.environmentManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.environmentManager->recreateGtaoTextures(ext.width, ext.height);
  }
  if (subs_.bloomManager) {
    const VkExtent2D ext = svc_.swapChainManager.extent();
    subs_.bloomManager->createTextures(ext.width, ext.height);
  }
  resources_.frameResources = subs_.frameResourceManager->frames();
  svc_.commandBufferManager.reallocate(svc_.swapChainManager.imageCount());

  const uint32_t imageCount = static_cast<uint32_t>(svc_.swapChainManager.imageCount());
  if (subs_.guiManager) subs_.guiManager->updateSwapchainImageCount(imageCount);

  subs_.frameSyncManager->recreateRenderFinishedSemaphores(svc_.swapChainManager.imageCount());
  frame_.imagesInFlight.assign(svc_.swapChainManager.imageCount(), VK_NULL_HANDLE);

  updateCameraBuffer();
  updateObjectBuffer();
  updateFrameDescriptorSets();
}

void RendererFrontend::processInput(float deltaTime) {
  if (!subs_.cameraController) return;

  auto toggleFromKey = [this](int key, bool& previousState, auto&& onToggle) {
    if (!svc_.nativeWindow) return;
    const bool keyDown = glfwGetKey(svc_.nativeWindow, key) == GLFW_PRESS;
    if (keyDown && !previousState) onToggle();
    previousState = keyDown;
  };

  toggleFromKey(GLFW_KEY_F6, debugState_.directionalOnlyKeyDown, [this]() {
    debugState_.directionalOnly = !debugState_.directionalOnly;
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage(debugState_.directionalOnly
          ? "Debug: directional-only enabled"
          : "Debug: directional-only disabled");
    }
  });

  toggleFromKey(GLFW_KEY_F7, debugState_.visualizePointLightStencilKeyDown, [this]() {
    debugState_.visualizePointLightStencil = !debugState_.visualizePointLightStencil;
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage(debugState_.visualizePointLightStencil
          ? "Debug: point-light stencil visualization enabled"
          : "Debug: point-light stencil visualization disabled");
    }
  });

  toggleFromKey(GLFW_KEY_F8, debugState_.freezeCullingKeyDown, [this]() {
    debugState_.freezeCulling = !debugState_.freezeCulling;
    if (!debugState_.freezeCulling && subs_.gpuCullManager)
      subs_.gpuCullManager->unfreezeCulling();
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage(debugState_.freezeCulling
          ? "Debug: culling camera frozen (F8 to unfreeze)"
          : "Debug: culling camera unfrozen");
    }
  });

  if (subs_.guiManager && subs_.guiManager->isCapturingInput() &&
      !svc_.inputManager.isLooking()) {
    return;
  }

  const bool cameraChanged = svc_.inputManager.update(deltaTime);
  if (cameraChanged) updateCameraBuffer();
}

bool RendererFrontend::reloadSceneModel(const std::string& path) {
  if (!subs_.sceneController) return false;
  const bool result = subs_.sceneController->reloadSceneModel(
      path, buffers_.object, buffers_.objectCapacity, buffers_.camera,
      sceneState_.indexType, sceneState_.rootNode,
      sceneState_.selectedMeshNode, sceneState_.cubeNode);
  syncSceneStateFromController();
  if (subs_.lightingManager) {
    subs_.lightingManager->setRootNode(sceneState_.rootNode);
    subs_.lightingManager->updateLightingData();
    subs_.lightingManager->createLightVolumeGeometry();
  }
  if (result) {
    if (subs_.cameraController) subs_.cameraController->resetCameraForScene();
    updateCameraBuffer();
  }
  updateObjectBuffer();
  if (subs_.sceneManager) subs_.sceneManager->updateDescriptorSet(buffers_.camera, buffers_.object);
  return result;
}

void RendererFrontend::shutdown() {
  if (!svc_.ctx.deviceWrapper) return;

  vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());

  subs_.frameSyncManager.reset();
  subs_.frameRecorder.reset();

  destroyGBufferResources();
  subs_.frameResourceManager.reset();

  if (subs_.guiManager) {
    subs_.guiManager->shutdown(svc_.ctx.deviceWrapper->device());
    subs_.guiManager.reset();
  }

  subs_.renderPassManager.reset();
  resources_.renderPasses = {};

  subs_.sceneController.reset();
  subs_.sceneManager.reset();
  subs_.lightingManager.reset();
  subs_.shadowManager.reset();
  subs_.environmentManager.reset();
  subs_.bloomManager.reset();
  subs_.oitManager.reset();
  subs_.cameraController.reset();
  subs_.pipelineBuilder.reset();

  if (buffers_.camera.buffer != VK_NULL_HANDLE)
    svc_.allocationManager.destroyBuffer(buffers_.camera);
  if (buffers_.object.buffer != VK_NULL_HANDLE)
    svc_.allocationManager.destroyBuffer(buffers_.object);
}

// ---------------------------------------------------------------------------
// Init helpers
// ---------------------------------------------------------------------------

void RendererFrontend::createRenderPasses() {
  resources_.gBufferFormats.depthStencil = subs_.renderPassManager->findDepthStencilFormat();
  subs_.renderPassManager->create(
      svc_.swapChainManager.imageFormat(),
      resources_.gBufferFormats.depthStencil,
      resources_.gBufferFormats.sceneColor,
      resources_.gBufferFormats.albedo,
      resources_.gBufferFormats.normal,
      resources_.gBufferFormats.material,
      resources_.gBufferFormats.emissive);
  resources_.renderPasses = subs_.renderPassManager->passes();
}

void RendererFrontend::createGraphicsPipelines() {
  if (!subs_.pipelineBuilder) {
    subs_.pipelineBuilder = std::make_unique<GraphicsPipelineBuilder>(
        svc_.ctx.deviceWrapper, svc_.pipelineManager,
        svc_.ctx.wireframeRasterModeSupported);
  }
  const PipelineDescriptorLayouts descLayouts{
      subs_.sceneManager->descriptorSetLayout(),
      subs_.frameResourceManager->lightingLayout(),
      subs_.lightingManager->lightDescriptorSetLayout(),
      subs_.lightingManager->isTiledLightingReady()
          ? subs_.lightingManager->tiledDescriptorSetLayout()
          : subs_.lightingManager->lightDescriptorSetLayout(),
      subs_.shadowManager->descriptorSetLayout(),
      subs_.frameResourceManager->postProcessLayout(),
      subs_.frameResourceManager->oitLayout()};
  const PipelineRenderPasses rp{
      resources_.renderPasses.depthPrepass,
      resources_.renderPasses.gBuffer,
      resources_.renderPasses.shadow,
      resources_.renderPasses.lighting,
      resources_.renderPasses.postProcess};
  resources_.builtPipelines = subs_.pipelineBuilder->build(
      container::util::executableDirectory(), descLayouts, rp);
}

void RendererFrontend::createCamera() {
  subs_.cameraController = std::make_unique<CameraController>(
      svc_.ctx.deviceWrapper, svc_.allocationManager, svc_.swapChainManager,
      sceneGraph_, subs_.sceneManager.get(), svc_.inputManager);
  subs_.cameraController->createCamera();
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
  subs_.sceneManager->updateDescriptorSet(buffers_.camera, buffers_.object);

  container::log::ContainerLogger::instance().renderer()->info(
      "Initializing Vulkan renderer");
  container::log::ContainerLogger::instance().vulkan()->debug(
      "Debugging Vulkan initialization");
}

void RendererFrontend::buildSceneGraph() {
  if (!subs_.sceneController) return;
  subs_.sceneController->buildSceneGraph(
      sceneState_.rootNode, sceneState_.selectedMeshNode, sceneState_.cubeNode);
  if (subs_.lightingManager) subs_.lightingManager->setRootNode(sceneState_.rootNode);
}

void RendererFrontend::createSceneBuffers() {
  if (buffers_.camera.buffer == VK_NULL_HANDLE) {
    buffers_.camera = svc_.allocationManager.createBuffer(
        sizeof(container::gpu::CameraData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT);
  }
  if (subs_.sceneController) {
    subs_.sceneController->createSceneBuffers(buffers_.camera, buffers_.object,
                                         buffers_.objectCapacity);
  }
  updateCameraBuffer();
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
      resources_.gBufferFormats,
      resources_.renderPasses.depthPrepass, resources_.renderPasses.gBuffer,
      resources_.renderPasses.lighting,
      buffers_.camera, buffers_.object);
  resources_.frameResources = subs_.frameResourceManager->frames();
}

// ---------------------------------------------------------------------------
// Per-frame helpers
// ---------------------------------------------------------------------------

void RendererFrontend::updateCameraBuffer() {
  if (subs_.cameraController)
    subs_.cameraController->updateCameraBuffer(buffers_.cameraData, buffers_.camera);
}

void RendererFrontend::updateObjectBuffer() {
  if (!subs_.sceneController) return;
  const bool recreated = subs_.sceneController->updateObjectBuffer(
      buffers_.object, buffers_.objectCapacity, buffers_.camera);
  if (recreated && subs_.sceneManager)
    subs_.sceneManager->updateDescriptorSet(buffers_.camera, buffers_.object);
  sceneState_.diagCubeObjectIndex = subs_.sceneController->diagCubeObjectIndex();
}

void RendererFrontend::updateFrameDescriptorSets() {
  if (subs_.frameResourceManager) {
    if (subs_.lightingManager) {
      auto& lightingData = subs_.lightingManager->lightingData();
      const bool shadowEnabled = isRenderPassEnabled(subs_.frameRecorder.get(), "ShadowCascade0") ||
                                 isRenderPassEnabled(subs_.frameRecorder.get(), "ShadowCascade1") ||
                                 isRenderPassEnabled(subs_.frameRecorder.get(), "ShadowCascade2") ||
                                 isRenderPassEnabled(subs_.frameRecorder.get(), "ShadowCascade3");
      const bool gtaoEnabled = isRenderPassEnabled(subs_.frameRecorder.get(), "GTAO");
      const bool tileCullEnabled = isRenderPassEnabled(subs_.frameRecorder.get(), "TileCull");
      lightingData.featureFlags[0] = shadowEnabled ? 1u : 0u;
      lightingData.featureFlags[1] = gtaoEnabled ? 1u : 0u;
      lightingData.featureFlags[2] = tileCullEnabled ? 1u : 0u;
      SceneController::writeToBuffer(
          svc_.allocationManager, subs_.lightingManager->lightingBuffer(),
          &lightingData, sizeof(container::gpu::LightingData));
    }

    VkImageView  shadowView    = VK_NULL_HANDLE;
    VkSampler    shadowSampler = VK_NULL_HANDLE;
    const container::gpu::AllocatedBuffer* shadowUbo = nullptr;
    if (subs_.shadowManager) {
      shadowView    = subs_.shadowManager->shadowAtlasArrayView();
      shadowSampler = subs_.shadowManager->shadowSampler();
      shadowUbo     = &subs_.shadowManager->shadowUbo();
    }
    VkImageView irradianceView  = VK_NULL_HANDLE;
    VkImageView prefilteredView = VK_NULL_HANDLE;
    VkImageView brdfLutView     = VK_NULL_HANDLE;
    VkSampler   envSampler      = VK_NULL_HANDLE;
    VkSampler   brdfLutSampler  = VK_NULL_HANDLE;
    VkImageView aoTextureView   = VK_NULL_HANDLE;
    VkSampler   aoSampler       = VK_NULL_HANDLE;
    if (subs_.environmentManager && subs_.environmentManager->isReady()) {
      irradianceView  = subs_.environmentManager->irradianceView();
      prefilteredView = subs_.environmentManager->prefilteredView();
      brdfLutView     = subs_.environmentManager->brdfLutView();
      envSampler      = subs_.environmentManager->envSampler();
      brdfLutSampler  = subs_.environmentManager->brdfLutSampler();
    }
    if (subs_.environmentManager && subs_.environmentManager->isGtaoReady() &&
        isRenderPassEnabled(subs_.frameRecorder.get(), "GTAO")) {
      aoTextureView = subs_.environmentManager->aoTextureView();
      aoSampler     = subs_.environmentManager->aoSampler();
    }
    VkImageView bloomTextureView = VK_NULL_HANDLE;
    VkSampler   bloomSampler     = VK_NULL_HANDLE;
    if (subs_.bloomManager && subs_.bloomManager->isReady()) {
      bloomTextureView = subs_.bloomManager->bloomResultView();
      bloomSampler     = subs_.bloomManager->bloomSampler();
    }
    VkBuffer     tileGridBuffer     = VK_NULL_HANDLE;
    VkDeviceSize tileGridBufferSize = 0;
    if (subs_.lightingManager && subs_.lightingManager->isTiledLightingReady() &&
        isRenderPassEnabled(subs_.frameRecorder.get(), "TileCull")) {
      tileGridBuffer     = subs_.lightingManager->tileGridBuffer();
      tileGridBufferSize = subs_.lightingManager->tileGridBufferSize();
    }
    subs_.frameResourceManager->updateDescriptorSets(
        buffers_.camera, buffers_.object,
        shadowView, shadowSampler, shadowUbo,
        irradianceView, prefilteredView, brdfLutView,
        envSampler, brdfLutSampler,
        aoTextureView, aoSampler,
        bloomTextureView, bloomSampler,
        tileGridBuffer, tileGridBufferSize);
  }
}

void RendererFrontend::destroyGBufferResources() {
  if (subs_.frameResourceManager) subs_.frameResourceManager->destroy();
  resources_.frameResources.clear();
}

bool RendererFrontend::growExactOitNodePoolIfNeeded(uint32_t imageIndex) {
  if (!subs_.frameResourceManager) return false;
  const bool grew = subs_.frameResourceManager->growOitPoolIfNeeded(
      imageIndex, frame_.exactOitNodeCapacityFloor);
  if (grew) {
    vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device());
    createFrameResources();
    if (subs_.guiManager) {
      subs_.guiManager->setStatusMessage(
          "Expanded exact OIT node pool to " +
          std::to_string(frame_.exactOitNodeCapacityFloor));
    }
  }
  return grew;
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

  // Sync freeze-culling: push debug state into GUI before rendering.
  subs_.guiManager->setFreezeCulling(debugState_.freezeCulling);

  // Sync bloom: push BloomManager settings into GUI before rendering.
  if (subs_.bloomManager) {
    subs_.guiManager->setBloomSettings(
        subs_.bloomManager->enabled(),
        subs_.bloomManager->threshold(),
        subs_.bloomManager->knee(),
        subs_.bloomManager->intensity(),
        subs_.bloomManager->filterRadius());
  }

  // Sync render pass toggles: push graph pass list into GUI before rendering.
  if (subs_.frameRecorder) {
    auto& graph = subs_.frameRecorder->graph();
    std::vector<container::ui::RenderPassToggle> passList;
    passList.reserve(graph.passes().size());
    for (const auto& node : graph.passes()) {
      container::ui::RenderPassToggle toggle{};
      toggle.name = node.name;
      toggle.enabled = node.enabled;
      toggle.locked = isProtectedRenderPass(node.name);
      passList.push_back(std::move(toggle));
    }
    subs_.guiManager->setRenderPassList(passList);
  }

  subs_.guiManager->startFrame();
  subs_.guiManager->drawSceneControls(
      sceneGraph_,
      [this](const std::string& modelPath) { return reloadSceneModel(modelPath); },
      [this]() { return reloadSceneModel(container::app::DefaultAppConfig().modelPath); },
      subs_.cameraController ? subs_.cameraController->cameraTransformControls()
                        : container::ui::TransformControls{},
      [this](const container::ui::TransformControls& controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyCameraTransform(controls, buffers_.cameraData, buffers_.camera);
      },
      subs_.cameraController ? subs_.cameraController->nodeTransformControls(sceneState_.rootNode)
                        : container::ui::TransformControls{},
      [this](const container::ui::TransformControls& controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyNodeTransform(
              sceneState_.rootNode, sceneState_.rootNode, controls);
        if (subs_.lightingManager) subs_.lightingManager->updateLightingData();
        updateObjectBuffer();
      },
      subs_.lightingManager ? subs_.lightingManager->directionalLightPosition() : glm::vec3{0.0f},
      subs_.lightingManager ? subs_.lightingManager->lightingData()             : container::gpu::LightingData{},
      sceneState_.selectedMeshNode,
      [this](uint32_t nodeIndex) {
        if (subs_.cameraController)
          subs_.cameraController->selectMeshNode(nodeIndex, sceneState_.selectedMeshNode);
      },
      subs_.cameraController
          ? subs_.cameraController->nodeTransformControls(sceneState_.selectedMeshNode)
          : container::ui::TransformControls{},
      [this](uint32_t nodeIndex, const container::ui::TransformControls& controls) {
        if (subs_.cameraController)
          subs_.cameraController->applyNodeTransform(
              nodeIndex, sceneState_.rootNode, controls);
        if (nodeIndex == sceneState_.rootNode && subs_.lightingManager)
          subs_.lightingManager->updateLightingData();
        updateObjectBuffer();
      });

  // Sync freeze-culling: pull GUI state back into debug state (checkbox may have toggled it).
  const bool guiFreeze = subs_.guiManager->freezeCullingRequested();
  if (guiFreeze != debugState_.freezeCulling) {
    debugState_.freezeCulling = guiFreeze;
    if (!guiFreeze && subs_.gpuCullManager)
      subs_.gpuCullManager->unfreezeCulling();
  }

  // Sync bloom: pull GUI state back into BloomManager.
  if (subs_.bloomManager) {
    subs_.bloomManager->enabled()      = subs_.guiManager->bloomEnabled();
    subs_.bloomManager->threshold()    = subs_.guiManager->bloomThreshold();
    subs_.bloomManager->knee()         = subs_.guiManager->bloomKnee();
    subs_.bloomManager->intensity()    = subs_.guiManager->bloomIntensity();
    subs_.bloomManager->filterRadius() = subs_.guiManager->bloomRadius();
  }

  if (subs_.lightingManager) {
    const auto& guiLightingSettings = subs_.guiManager->lightingSettings();
    const auto& currentLightingSettings =
        subs_.lightingManager->lightingSettings();
    const bool lightingSettingsChanged =
        guiLightingSettings.preset != currentLightingSettings.preset ||
        guiLightingSettings.density != currentLightingSettings.density ||
        guiLightingSettings.radiusScale != currentLightingSettings.radiusScale ||
        guiLightingSettings.intensityScale != currentLightingSettings.intensityScale ||
        guiLightingSettings.directionalIntensity !=
            currentLightingSettings.directionalIntensity;
    if (lightingSettingsChanged) {
      subs_.lightingManager->setLightingSettings(guiLightingSettings);
      subs_.lightingManager->updateLightingData();
    }
  }

  // Sync render pass toggles: pull GUI toggle states back into the render graph.
  if (subs_.frameRecorder) {
    auto& graph = subs_.frameRecorder->graph();
    auto& toggles = subs_.guiManager->renderPassToggles();
    if (enforceRenderPassDependencies(toggles)) {
      subs_.guiManager->setStatusMessage(
          "Protected passes stay enabled; dependent optional passes are disabled automatically.");
    }
    for (auto& toggle : toggles) {
      toggle.locked = isProtectedRenderPass(toggle.name);
      toggle.autoDisabled = !toggle.locked && !toggle.enabled && !dependencyNoteForPass(toggles, toggle.name).empty();
      toggle.dependencyNote = toggle.autoDisabled ? dependencyNoteForPass(toggles, toggle.name) : std::string{};
    }
    for (const auto& toggle : toggles) {
      if (auto* node = graph.findPass(toggle.name)) {
        node->enabled = toggle.enabled;
      }
    }
  }
}

void RendererFrontend::recordCommandBuffer(VkCommandBuffer commandBuffer,
                                           uint32_t imageIndex) {
  if (!subs_.frameRecorder || imageIndex >= resources_.frameResources.size()) {
    throw std::runtime_error(
        "frameRecorder not initialized or image index out of range");
  }
  const auto p = buildFrameRecordParams(imageIndex);
  subs_.frameRecorder->record(commandBuffer, p);
}

FrameRecordParams RendererFrontend::buildFrameRecordParams(uint32_t imageIndex) {
  FrameRecordParams p{};
  p.frame                           = &resources_.frameResources[imageIndex];
  p.imageIndex                      = imageIndex;
  p.vertexSlice                     = sceneState_.vertexSlice;
  p.indexSlice                      = sceneState_.indexSlice;
  p.indexType                       = sceneState_.indexType;
  p.opaqueDrawCommands              = &subs_.sceneController->opaqueDrawCommands();
  p.transparentDrawCommands         = &subs_.sceneController->transparentDrawCommands();
  p.opaqueSingleSidedDrawCommands   = &subs_.sceneController->opaqueSingleSidedDrawCommands();
  p.opaqueDoubleSidedDrawCommands   = &subs_.sceneController->opaqueDoubleSidedDrawCommands();
  p.transparentSingleSidedDrawCommands =
      &subs_.sceneController->transparentSingleSidedDrawCommands();
  p.transparentDoubleSidedDrawCommands =
      &subs_.sceneController->transparentDoubleSidedDrawCommands();
  p.sceneDescriptorSet              = subs_.sceneManager->descriptorSet();
  p.lightDescriptorSet              = subs_.lightingManager
                                        ? subs_.lightingManager->lightDescriptorSet()
                                        : VK_NULL_HANDLE;
  p.tiledDescriptorSet              = (subs_.lightingManager &&
                                       subs_.lightingManager->isTiledLightingReady())
                                        ? subs_.lightingManager->tiledDescriptorSet()
                                        : VK_NULL_HANDLE;
  p.cameraBuffer                    = buffers_.camera.buffer;
  p.cameraBufferSize                = sizeof(container::gpu::CameraData);
  p.gBufferSampler                  = subs_.frameResourceManager
                                        ? subs_.frameResourceManager->gBufferSampler()
                                        : VK_NULL_HANDLE;
  p.renderPasses = {
      resources_.renderPasses.depthPrepass,
      resources_.renderPasses.gBuffer,
      resources_.renderPasses.shadow,
      resources_.renderPasses.lighting,
      resources_.renderPasses.postProcess};
  p.layouts                         = resources_.builtPipelines.layouts;
  p.pipelines                       = resources_.builtPipelines.pipelines;
  p.debugDirectionalOnly            = debugState_.directionalOnly;
  p.debugVisualizePointLightStencil = debugState_.visualizePointLightStencil;
  p.debugFreezeCulling              = debugState_.freezeCulling;
  p.wireframeRasterModeSupported    = svc_.ctx.wireframeRasterModeSupported;
  p.wireframeWideLinesSupported     = svc_.ctx.wireframeWideLinesSupported;
  p.pushConstants                   = pushConstants_.state();
  p.swapChainFramebuffers           = &svc_.swapChainManager.framebuffers();
  p.diagCubeObjectIndex             = sceneState_.diagCubeObjectIndex;
  if (subs_.cameraController) {
    const auto* perspCam = dynamic_cast<const container::scene::PerspectiveCamera*>(
        subs_.cameraController->camera());
    if (perspCam) {
      p.cameraNear = perspCam->nearPlane();
      p.cameraFar  = perspCam->farPlane();
    }
  }
  if (subs_.shadowManager) {
    p.shadowDescriptorSet  = subs_.shadowManager->descriptorSet();
    p.shadowFramebuffers   = subs_.shadowManager->framebuffers().data();
    p.shadowData           = &subs_.shadowManager->shadowData();
  }
  p.gpuCullManager = subs_.gpuCullManager.get();
  p.bloomManager     = subs_.bloomManager.get();
  p.objectBuffer     = buffers_.object.buffer;
  p.objectBufferSize = sizeof(container::gpu::ObjectData) * buffers_.objectCapacity;
  return p;
}

// ---------------------------------------------------------------------------
// Scene helpers
// ---------------------------------------------------------------------------

void RendererFrontend::syncSceneStateFromController() {
  if (!subs_.sceneController) return;
  sceneState_.vertexSlice = subs_.sceneController->vertexSlice();
  sceneState_.indexSlice  = subs_.sceneController->indexSlice();
}

}  // namespace container::renderer
