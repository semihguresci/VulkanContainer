#include "Container/app/Application.h"

#include "Container/common/CommonGLFW.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/InputManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/WindowManager.h"

#include "Container/renderer/resources/CommandBufferManager.h"
#include "Container/renderer/core/RendererFrontend.h"
#include "Container/renderer/platform/VulkanContext.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/platform/WindowInputBridge.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace container::app {

Application::Application(AppConfig config)
    : config_(std::move(config)) {}

Application::~Application() {
  if (vulkanContext_) {
    try { cleanup(); } catch (...) {}
  }
}

void Application::run() {
  initWindow();
  initVulkan();
  mainLoop();
  cleanup();
}

// ---------------------------------------------------------------------------
// Lifecycle stages
// ---------------------------------------------------------------------------

void Application::initWindow() {
  windowManager_ = std::make_unique<container::window::WindowManager>();
  container::window::WindowManager::WindowConfig windowConfig{};
  windowConfig.width = config_.windowWidth;
  windowConfig.height = config_.windowHeight;
  windowConfig.title = "Vulkan";
  windowConfig.visible = config_.windowVisible;
  windowConfig.focused = config_.windowVisible;
  windowConfig.focusOnShow = config_.windowVisible;
  window_ = windowManager_->createWindow(windowConfig);

  GLFWwindow* nativeWindow = window_->getNativeWindow();
  inputManager_ = std::make_unique<container::window::InputManager>();
  inputManager_->setWindow(nativeWindow);
  windowInputBridge_ = std::make_unique<container::renderer::WindowInputBridge>(
      nativeWindow, *inputManager_, framebufferResized_);

  onInitWindow();
}

void Application::initVulkan() {
  const container::renderer::VulkanContextInitializer ctxInit{config_};
  vulkanContext_ = std::make_unique<container::renderer::VulkanContext>(
      ctxInit.initialize(
          windowManager_->getRequiredInstanceExtensions(),
          window_->getNativeWindow()),
      config_.enableValidationLayers);

  auto& ctx = vulkanContext_->result();

  pipelineManager_ = std::make_unique<container::gpu::PipelineManager>(
      ctx.deviceWrapper->device());

  swapChainManager_ = std::make_unique<container::gpu::SwapChainManager>(
      window_->getNativeWindow(),
      ctx.deviceWrapper->physicalDevice(),
      ctx.deviceWrapper->device(),
      ctx.surface);
  swapChainManager_->initialize();

  commandBufferManager_ = std::make_unique<container::renderer::CommandBufferManager>(
      ctx.deviceWrapper,
      ctx.deviceWrapper->queueFamilyIndices().graphicsFamily.value());

  allocationManager_ = std::make_unique<container::gpu::AllocationManager>();
  allocationManager_->initialize(
      ctx.instance,
      ctx.deviceWrapper->physicalDevice(),
      ctx.deviceWrapper->device(),
      ctx.deviceWrapper->graphicsQueue(),
      commandBufferManager_->pool(), config_);

  renderer_ = std::make_unique<container::renderer::RendererFrontend>(
      container::renderer::RendererFrontendCreateInfo{
          .ctx                  = &ctx,
          .pipelineManager      = pipelineManager_.get(),
          .allocationManager    = allocationManager_.get(),
          .swapChainManager     = swapChainManager_.get(),
          .commandBufferManager = commandBufferManager_.get(),
          .config               = &config_,
          .nativeWindow         = window_->getNativeWindow(),
          .inputManager         = inputManager_.get()});
  renderer_->initialize();

  onInitVulkan();
}

void Application::mainLoop() {
  if (!config_.screenshotCapturePath.empty()) {
    screenshotCaptureLoop();
    return;
  }

  lastFrameTimeSeconds_ = windowManager_->getTime();
  while (!window_->shouldClose()) {
    const double now = windowManager_->getTime();
    const float  dt  = static_cast<float>(now - lastFrameTimeSeconds_);
    lastFrameTimeSeconds_ = now;

    window_->pollEvents();
    renderer_->processInput(dt);
    renderer_->drawFrame(framebufferResized_);
  }
  vkDeviceWaitIdle(vulkanContext_->result().deviceWrapper->device());

  onMainLoop();
}

void Application::screenshotCaptureLoop() {
  const uint32_t captureFrame =
      std::max(config_.screenshotCaptureFrame,
               config_.screenshotWarmupFrames + 1u);
  const float fixedDt = config_.screenshotFixedTimestepSeconds > 0.0f
                            ? config_.screenshotFixedTimestepSeconds
                            : 1.0f / 60.0f;

  for (uint32_t frameNumber = 1; frameNumber <= captureFrame &&
                                     !window_->shouldClose();
       ++frameNumber) {
    window_->pollEvents();
    renderer_->processInput(fixedDt);
    if (frameNumber == captureFrame) {
      renderer_->requestScreenshot(config_.screenshotCapturePath);
    }
    renderer_->drawFrame(framebufferResized_);
  }
  vkDeviceWaitIdle(vulkanContext_->result().deviceWrapper->device());

  onMainLoop();
}

void Application::cleanup() {
  onCleanup();

  renderer_.reset();
  allocationManager_.reset();
  commandBufferManager_.reset();
  pipelineManager_.reset();
  swapChainManager_.reset();

  if (vulkanContext_) {
    vulkanContext_->result().deviceWrapper.reset();
    vulkanContext_.reset();
  }

  windowInputBridge_.reset();
  inputManager_.reset();
  window_.reset();
  windowManager_.reset();
}

// ---------------------------------------------------------------------------
// Virtual override points — default implementations are no-ops.
// ---------------------------------------------------------------------------

void Application::onInitWindow() {}
void Application::onInitVulkan() {}
void Application::onMainLoop() {}
void Application::onCleanup() {}

}  // namespace container::app
