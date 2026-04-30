#pragma once

#include "Container/renderer/DebugRenderState.h"
#include "Container/renderer/PushConstantBlock.h"
#include "Container/renderer/RenderResources.h"
#include "Container/renderer/SceneState.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

// Forward declarations — full headers are only needed in RendererFrontend.cpp.
namespace container::app {
struct AppConfig;
}  // namespace container::app

namespace container::renderer {
class BloomManager;
class CameraController;
class CommandBufferManager;
class EnvironmentManager;
class FrameRecorder;
class FrameResourceManager;
class GraphicsPipelineBuilder;
class GpuCullManager;
class OitManager;
class SceneController;
class ShadowCullManager;
class ShadowManager;
struct VulkanContextResult;
}  // namespace container::renderer

namespace container::gpu {
class AllocationManager;
class FrameSyncManager;
class PipelineManager;
class SwapChainManager;
}  // namespace container::gpu

namespace container::scene {
class SceneGraph;
class SceneManager;
}  // namespace container::scene

namespace container::ui {
class GuiManager;
}

namespace container::window {
class InputManager;
}  // namespace container::window

namespace container::renderer {

// Groups all construction parameters for RendererFrontend.
struct RendererFrontendCreateInfo {
  VulkanContextResult*                 ctx{nullptr};
  container::gpu::PipelineManager*  pipelineManager{nullptr};
  container::gpu::AllocationManager*  allocationManager{nullptr};
  container::gpu::SwapChainManager*           swapChainManager{nullptr};
  CommandBufferManager*                commandBufferManager{nullptr};
  const container::app::AppConfig*                config{nullptr};
  GLFWwindow*                          nativeWindow{nullptr};
  container::window::InputManager*        inputManager{nullptr};
};

// RendererFrontend owns the renderer-facing lifetime graph. The application
// handles window/input setup, while this class creates render passes, frame
// resources, scene systems, pipelines, and per-frame submission state.
class RendererFrontend {
 public:
  explicit RendererFrontend(RendererFrontendCreateInfo info);

  ~RendererFrontend();
  RendererFrontend(const RendererFrontend&) = delete;
  RendererFrontend& operator=(const RendererFrontend&) = delete;

  // Full initialization: render passes → pipelines → scene → sync primitives.
  void initialize();

  // Submit one frame. Returns false if swap chain needs recreation (caller
  // should set framebufferResized = false and call handleResize()).
  bool drawFrame(bool& framebufferResized);

  // Handle window resize / suboptimal swapchain.
  void handleResize();

  // Process keyboard / camera input for this tick.
  void processInput(float deltaTime);

  // Scene operations forwarded from the application.
  bool reloadSceneModel(const std::string& path);

  // Shutdown: wait idle and release all Vulkan resources in dependency order.
  void shutdown();

  // Access debug state (allows the application to expose toggles if needed).
  DebugRenderState&       debugState()       { return debugState_; }
  const DebugRenderState& debugState() const { return debugState_; }

  const SceneState& sceneState() const { return sceneState_; }

 private:
  // Owned subsystems are listed roughly in construction/use order. shutdown()
  // releases them in dependency-aware order because many destructors touch
  // Vulkan objects owned by earlier services.
  struct OwnedSubsystems {
    std::unique_ptr<RenderPassManager>                  renderPassManager;
    std::unique_ptr<OitManager>                         oitManager;
    std::unique_ptr<FrameResourceManager>               frameResourceManager;
    std::unique_ptr<container::scene::SceneManager>     sceneManager;
    std::unique_ptr<SceneController>                    sceneController;
    std::unique_ptr<CameraController>                   cameraController;
    std::unique_ptr<LightingManager>                    lightingManager;
    std::unique_ptr<ShadowCullManager>                  shadowCullManager;
    std::unique_ptr<ShadowManager>                       shadowManager;
    std::unique_ptr<EnvironmentManager>                  environmentManager;
    std::unique_ptr<GpuCullManager>                      gpuCullManager;
    std::unique_ptr<BloomManager>                          bloomManager;
    std::unique_ptr<GraphicsPipelineBuilder>             pipelineBuilder;
    std::unique_ptr<FrameRecorder>                      frameRecorder;
    std::unique_ptr<container::ui::GuiManager>          guiManager;
    std::unique_ptr<container::gpu::FrameSyncManager>   frameSyncManager;
  };

  // External services passed in at construction. These must outlive the
  // frontend; they wrap the device, swapchain, allocator, command pool, and
  // app configuration supplied by the application layer.
  struct BorrowedServices {
    VulkanContextResult&                  ctx;
    container::gpu::PipelineManager&     pipelineManager;
    container::gpu::AllocationManager&   allocationManager;
    container::gpu::SwapChainManager&    swapChainManager;
    CommandBufferManager&                commandBufferManager;
    const container::app::AppConfig&     config;
    GLFWwindow*                          nativeWindow{nullptr};
    container::window::InputManager&     inputManager;
  };

  OwnedSubsystems   subs_;
  BorrowedServices   svc_;

  // ---- grouped state ----------------------------------------------------------
  RenderResources   resources_{};
  PushConstantBlock pushConstants_{};

  // GPU buffers backing the camera UBO and per-object SSBO.
  struct SceneBufferState {
    std::vector<container::gpu::AllocatedBuffer> cameras;
    container::gpu::AllocatedBuffer object{};
    size_t                          objectCapacity{0};
    container::gpu::CameraData      cameraData{};
  };
  SceneBufferState buffers_{};

  container::scene::SceneGraph sceneGraph_{};
  SceneState                 sceneState_{};
  DebugRenderState           debugState_{};

  // Per-frame synchronisation / bookkeeping.
  struct FrameState {
    std::vector<VkFence> imagesInFlight;
    uint32_t             currentFrame{0};
  };
  FrameState frame_{};

  // ---- internal init helpers --------------------------------------------------
  void createRenderPasses();
  void createGraphicsPipelines();
  void createCamera();
  void initializeScene();
  void buildSceneGraph();
  void createSceneBuffers();
  void createGeometryBuffers();
  void createFrameResources();
  void ensureCameraBuffers();

  // ---- per-frame helpers ------------------------------------------------------
  void updateCameraBuffer(uint32_t imageIndex);
  void updateObjectBuffer();
  void updateFrameDescriptorSets(uint32_t imageIndex = UINT32_MAX);
  void destroyGBufferResources();
  bool growExactOitNodePoolIfNeeded(uint32_t imageIndex);
  void presentSceneControls();
  void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
  [[nodiscard]] FrameRecordParams buildFrameRecordParams(uint32_t imageIndex);

  // ---- scene helpers ----------------------------------------------------------
  void syncSceneStateFromController();
};

}  // namespace container::renderer
