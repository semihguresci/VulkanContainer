#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

#include "Container/renderer/DebugRenderState.h"
#include "Container/renderer/PushConstantBlock.h"
#include "Container/renderer/RendererDeviceCapabilities.h"
#include "Container/renderer/RenderSurfaceInteractionController.h"
#include "Container/renderer/RenderResources.h"
#include "Container/renderer/SceneState.h"
#include "Container/utility/VulkanMemoryManager.h"

struct GLFWwindow;

// Forward declarations — full headers are only needed in RendererFrontend.cpp.
namespace container::app {
struct AppConfig;
}  // namespace container::app

namespace container::renderer {
class BloomManager;
class BimManager;
struct BimDrawFilter;
class CameraController;
class CommandBufferManager;
class EnvironmentManager;
class ExposureManager;
class FrameRecorder;
class FrameResourceManager;
class GraphicsPipelineBuilder;
class GpuCullManager;
class OitManager;
class RenderPassGpuProfiler;
class RenderTechnique;
class RenderTechniqueRegistry;
class RendererTelemetry;
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
class SceneProviderRegistry;
}  // namespace container::scene

namespace container::ui {
class GuiManager;
struct ViewpointSnapshotState;
}

namespace container::window {
class InputManager;
}  // namespace container::window

namespace container::renderer {

// Groups all construction parameters for RendererFrontend.
struct RendererFrontendCreateInfo {
  VulkanContextResult* ctx{nullptr};
  container::gpu::PipelineManager* pipelineManager{nullptr};
  container::gpu::AllocationManager* allocationManager{nullptr};
  container::gpu::SwapChainManager* swapChainManager{nullptr};
  CommandBufferManager* commandBufferManager{nullptr};
  const container::app::AppConfig* config{nullptr};
  GLFWwindow* nativeWindow{nullptr};
  container::window::InputManager* inputManager{nullptr};
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

  // Capture the next submitted swapchain image to an sRGB PNG.
  void requestScreenshot(std::filesystem::path outputPath);

  // Scene operations forwarded from the application.
  bool reloadSceneModel(const std::string& path, float importScale = 1.0f);

  // Shutdown: wait idle and release all Vulkan resources in dependency order.
  void shutdown();

  // Access debug state (allows the application to expose toggles if needed).
  DebugRenderState& debugState() { return debugState_; }
  const DebugRenderState& debugState() const { return debugState_; }

  const SceneState& sceneState() const { return sceneState_; }

 private:
  // Owned subsystems are listed roughly in construction/use order. shutdown()
  // releases them in dependency-aware order because many destructors touch
  // Vulkan objects owned by earlier services.
  struct OwnedSubsystems {
    std::unique_ptr<RenderPassManager> renderPassManager;
    std::unique_ptr<OitManager> oitManager;
    std::unique_ptr<FrameResourceManager> frameResourceManager;
    std::unique_ptr<container::scene::SceneManager> sceneManager;
    std::unique_ptr<BimManager> bimManager;
    std::unique_ptr<SceneController> sceneController;
    std::unique_ptr<CameraController> cameraController;
    std::unique_ptr<LightingManager> lightingManager;
    std::unique_ptr<ShadowCullManager> shadowCullManager;
    std::unique_ptr<ShadowManager> shadowManager;
    std::unique_ptr<EnvironmentManager> environmentManager;
    std::unique_ptr<GpuCullManager> gpuCullManager;
    std::unique_ptr<BloomManager> bloomManager;
    std::unique_ptr<ExposureManager> exposureManager;
    std::unique_ptr<GraphicsPipelineBuilder> pipelineBuilder;
    std::unique_ptr<FrameRecorder> frameRecorder;
    std::unique_ptr<container::scene::SceneProviderRegistry>
        sceneProviderRegistry;
    RendererDeviceCapabilities deviceCapabilities{
        RendererDeviceCapabilities::rasterOnly()};
    std::unique_ptr<RenderTechniqueRegistry> techniqueRegistry;
    RenderTechnique* activeTechnique{nullptr};
    std::unique_ptr<RenderPassGpuProfiler> renderPassGpuProfiler;
    std::unique_ptr<RendererTelemetry> rendererTelemetry;
    std::unique_ptr<container::ui::GuiManager> guiManager;
    std::unique_ptr<container::gpu::FrameSyncManager> frameSyncManager;
  };

  // External services passed in at construction. These must outlive the
  // frontend; they wrap the device, swapchain, allocator, command pool, and
  // app configuration supplied by the application layer.
  struct BorrowedServices {
    VulkanContextResult& ctx;
    container::gpu::PipelineManager& pipelineManager;
    container::gpu::AllocationManager& allocationManager;
    container::gpu::SwapChainManager& swapChainManager;
    CommandBufferManager& commandBufferManager;
    const container::app::AppConfig& config;
    GLFWwindow* nativeWindow{nullptr};
    container::window::InputManager& inputManager;
  };

  OwnedSubsystems subs_;
  BorrowedServices svc_;

  // ---- grouped state
  // ----------------------------------------------------------
  RenderResources resources_{};
  PushConstantBlock pushConstants_{};

  // GPU buffers backing the camera UBO and per-object SSBO.
  struct SceneBufferState {
    std::vector<container::gpu::AllocatedBuffer> cameras;
    container::gpu::AllocatedBuffer object{};
    size_t objectCapacity{0};
    container::gpu::CameraData cameraData{};
    bool shadowObjectDescriptorReady{false};
  };
  SceneBufferState buffers_{};

  container::scene::SceneGraph sceneGraph_{};
  SceneState sceneState_{};
  DebugRenderState debugState_{};
  RenderSurfaceInteractionController interactionController_{};
  uint32_t hoveredMeshNode_{container::scene::SceneGraph::kInvalidNode};
  uint32_t hoveredBimObjectIndex_{std::numeric_limits<uint32_t>::max()};
  uint32_t selectedBimObjectIndex_{std::numeric_limits<uint32_t>::max()};
  struct SelectionNavigationAnchor {
    bool valid{false};
    glm::vec3 point{0.0f};
    float radius{1.0f};
    uint32_t sceneNode{container::scene::SceneGraph::kInvalidNode};
    uint32_t bimObject{std::numeric_limits<uint32_t>::max()};
  };
  SelectionNavigationAnchor selectionNavigationAnchor_{};
  struct HoverPickCache {
    bool valid{false};
    double cursorX{0.0};
    double cursorY{0.0};
    uint32_t selectedMeshNode{container::scene::SceneGraph::kInvalidNode};
    uint32_t selectedBimObjectIndex{std::numeric_limits<uint32_t>::max()};
    uint64_t objectDataRevision{0};
    uint64_t bimObjectDataRevision{0};
    bool bimTypeFilterEnabled{false};
    std::string bimFilterType{};
    bool bimStoreyFilterEnabled{false};
    std::string bimFilterStorey{};
    bool bimMaterialFilterEnabled{false};
    std::string bimFilterMaterial{};
    bool bimDisciplineFilterEnabled{false};
    std::string bimFilterDiscipline{};
    bool bimPhaseFilterEnabled{false};
    std::string bimFilterPhase{};
    bool bimFireRatingFilterEnabled{false};
    std::string bimFilterFireRating{};
    bool bimLoadBearingFilterEnabled{false};
    std::string bimFilterLoadBearing{};
    bool bimStatusFilterEnabled{false};
    std::string bimFilterStatus{};
    bool bimDrawBudgetEnabled{false};
    uint32_t bimDrawBudgetMaxObjects{0};
    bool bimIsolateSelection{false};
    bool bimHideSelection{false};
    bool bimPointCloudVisible{true};
    bool bimCurvesVisible{true};
    bool sectionPlaneEnabled{false};
    glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
    container::gpu::CameraData cameraData{};
  };
  HoverPickCache hoverPickCache_{};
  std::vector<DrawCommand> hoveredBimDrawCommands_{};
  std::vector<DrawCommand> selectedBimDrawCommands_{};
  std::vector<DrawCommand> hoveredBimNativePointDrawCommands_{};
  std::vector<DrawCommand> selectedBimNativePointDrawCommands_{};
  std::vector<DrawCommand> hoveredBimNativeCurveDrawCommands_{};
  std::vector<DrawCommand> selectedBimNativeCurveDrawCommands_{};
  std::vector<DrawCommand> hoveredDrawCommands_{};
  std::vector<DrawCommand> selectedDrawCommands_{};
  std::string activePrimaryModelPath_{};
  float activePrimaryImportScale_{1.0f};
  std::string activeAuxiliaryModelPath_{};
  float activeAuxiliaryImportScale_{1.0f};

  // Per-frame synchronisation / bookkeeping.
  struct FrameState {
    std::vector<VkFence> imagesInFlight;
    uint32_t currentFrame{0};
    uint64_t submittedFrameCount{0};
  };
  FrameState frame_{};

  struct ScreenshotState {
    std::filesystem::path outputPath{};
    bool pending{false};
    container::gpu::AllocatedBuffer readbackBuffer{};
    VkDeviceSize readbackSize{0};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
  };
  ScreenshotState screenshot_{};

  struct DepthVisibilityState {
    container::gpu::AllocatedBuffer readbackBuffer{};
    VkDeviceSize readbackSize{0};
    uint32_t imageIndex{std::numeric_limits<uint32_t>::max()};
    VkExtent2D extent{};
    VkFormat format{VK_FORMAT_UNDEFINED};
    container::gpu::CameraData cameraData{};
    uint64_t objectDataRevision{0};
    uint64_t bimObjectDataRevision{0};
    bool sectionPlaneEnabled{false};
    glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
    bool bimTypeFilterEnabled{false};
    std::string bimFilterType{};
    bool bimStoreyFilterEnabled{false};
    std::string bimFilterStorey{};
    bool bimMaterialFilterEnabled{false};
    std::string bimFilterMaterial{};
    bool bimDisciplineFilterEnabled{false};
    std::string bimFilterDiscipline{};
    bool bimPhaseFilterEnabled{false};
    std::string bimFilterPhase{};
    bool bimFireRatingFilterEnabled{false};
    std::string bimFilterFireRating{};
    bool bimLoadBearingFilterEnabled{false};
    std::string bimFilterLoadBearing{};
    bool bimStatusFilterEnabled{false};
    std::string bimFilterStatus{};
    bool bimDrawBudgetEnabled{false};
    uint32_t bimDrawBudgetMaxObjects{0};
    bool bimIsolateSelection{false};
    bool bimHideSelection{false};
    bool bimPointCloudVisible{true};
    bool bimCurvesVisible{true};
    bool transparentPickDepthValid{false};
    uint32_t selectedBimObjectIndex{std::numeric_limits<uint32_t>::max()};
    VkFence renderFence{VK_NULL_HANDLE};
    bool valid{false};
  };
  DepthVisibilityState depthVisibility_{};

  struct TransformDragSession {
    bool active{false};
    uint32_t nodeIndex{std::numeric_limits<uint32_t>::max()};
    container::ui::ViewportTool tool{container::ui::ViewportTool::Select};
    container::ui::TransformSpace space{container::ui::TransformSpace::World};
    container::ui::TransformAxis axis{container::ui::TransformAxis::Free};
    bool snapEnabled{false};
    container::ui::TransformControls startControls{};
    glm::vec3 origin{0.0f};
    float gizmoScale{1.0f};
    glm::vec3 axisX{1.0f, 0.0f, 0.0f};
    glm::vec3 axisY{0.0f, 1.0f, 0.0f};
    glm::vec3 axisZ{0.0f, 0.0f, 1.0f};
    double accumulatedDeltaX{0.0};
    double accumulatedDeltaY{0.0};
  };
  TransformDragSession transformDragSession_{};

  // ---- internal init helpers
  // --------------------------------------------------
  void createRenderPasses();
  void createGraphicsPipelines();
  void createCamera();
  void syncCameraSelectionPivotOverride();
  void initializeScene();
  void buildSceneGraph();
  void createSceneBuffers();
  void createGeometryBuffers();
  void createFrameResources();
  void ensureCameraBuffers();

  // ---- per-frame helpers
  // ------------------------------------------------------
  void updateCameraBuffer(uint32_t imageIndex);
  void updateObjectBuffer();
  void applyBimSemanticColorMode();
  void updateFrameDescriptorSets(uint32_t imageIndex = UINT32_MAX);
  void destroyGBufferResources();
  bool growExactOitNodePoolIfNeeded(uint32_t imageIndex);
  void ensureScreenshotReadbackBuffer(VkExtent2D extent, VkFormat format);
  void writePendingScreenshotPng();
  void ensureDepthVisibilityReadbackBuffer();
  void markDepthVisibilityFrameComplete(uint32_t imageIndex);
  [[nodiscard]] bool sampleDepthAtCursor(double cursorX,
                                         double cursorY,
                                         float& outDepth);
  [[nodiscard]] bool samplePickDepthAtCursor(double cursorX,
                                             double cursorY,
                                             float& outDepth);
  [[nodiscard]] bool sampleDepthAtCursor(double cursorX,
                                         double cursorY,
                                         float& outDepth,
                                         bool pickDepth);
  [[nodiscard]] bool samplePickIdAtCursor(double cursorX,
                                          double cursorY,
                                          uint32_t& outPickId);
  [[nodiscard]] bool depthVisibilityFrameMatchesCurrentState() const;
  [[nodiscard]] BimDrawFilter currentBimDrawFilter() const;
  [[nodiscard]] bool bimObjectVisibleByLayer(uint32_t objectIndex) const;
  [[nodiscard]] container::ui::ViewpointSnapshotState
  currentViewpointSnapshot() const;
  bool restoreViewpointSnapshot(
      const container::ui::ViewpointSnapshotState& snapshot);
  void presentSceneControls();
  void selectMeshNodeAtCursor(double cursorX, double cursorY);
  void hoverMeshNodeAtCursor(double cursorX, double cursorY);
  void clearHoveredMeshNode();
  void clearSelectedMeshNode();
  void transformSelectedNodeByDrag(container::ui::ViewportTool tool,
                                   container::ui::TransformSpace space,
                                   container::ui::TransformAxis axis,
                                   bool snapEnabled,
                                   double deltaX, double deltaY);
  [[nodiscard]] std::optional<container::ui::TransformAxis>
  pickTransformGizmoAxisAtCursor(double cursorX, double cursorY) const;
  void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex);
  [[nodiscard]] FrameRecordParams buildFrameRecordParams(uint32_t imageIndex);
  [[nodiscard]] FrameTransformGizmoState buildTransformGizmoState() const;

  // ---- scene helpers
  // ----------------------------------------------------------
  void syncSceneStateFromController();
};

}  // namespace container::renderer
