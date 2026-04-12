#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonMath.h"

#include "Container/utility/SceneData.h"

struct GLFWwindow;

namespace container::scene {
class SceneGraph;
}  // namespace container::scene

namespace container::ui {

enum class GBufferViewMode : uint32_t {
  Lit = 0,
  Albedo = 1,
  Normals = 2,
  Material = 3,
  Depth = 4,
  Emissive = 5,
  Transparency = 6,
  Revealage = 7,
  Overview = 8,
  SurfaceNormals = 9,
  ObjectSpaceNormals = 10,
};

enum class WireframeMode : uint32_t {
  Overlay = 0,
  Full = 1,
};

struct WireframeSettings {
  bool enabled{false};
  WireframeMode mode{WireframeMode::Overlay};
  bool depthTest{true};
  glm::vec3 color{0.0f, 1.0f, 0.0f};
  float lineWidth{1.0f};
  float overlayIntensity{0.85f};
};

struct TransformControls {
  glm::vec3 position{0.0f, 0.0f, 0.0f};
  glm::vec3 rotationDegrees{0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f, 1.0f, 1.0f};
};

class GuiManager {
 public:
  GuiManager() = default;
  ~GuiManager();

  void initialize(VkInstance instance, VkDevice device,
                  VkPhysicalDevice physicalDevice, VkQueue graphicsQueue,
                  uint32_t graphicsQueueFamily, VkRenderPass renderPass,
                  uint32_t imageCount, GLFWwindow* window,
                  const std::string& defaultModelPath);

  void shutdown(VkDevice device);
  void updateSwapchainImageCount(uint32_t imageCount);

  void startFrame();
  void render(VkCommandBuffer commandBuffer);

  void drawSceneControls(
      const container::scene::SceneGraph& sceneGraph,
      const std::function<bool(const std::string&)>& reloadModel,
      const std::function<bool()>& reloadDefault,
      const TransformControls& cameraTransform,
      const std::function<void(const TransformControls&)>& applyCameraTransform,
      const TransformControls& sceneTransform,
      const std::function<void(const TransformControls&)>& applySceneTransform,
      const glm::vec3& directionalLightPosition,
      const container::gpu::LightingData& lightingData,
      uint32_t selectedMeshNode,
      const std::function<void(uint32_t)>& selectMeshNode,
      const TransformControls& meshTransform,
      const std::function<void(uint32_t, const TransformControls&)>&
          applyMeshTransform);

  [[nodiscard]] bool isCapturingInput() const;
  [[nodiscard]] bool showGeometryOverlay() const { return showGeometryOverlay_; }
  [[nodiscard]] bool showLightGizmos() const { return showLightGizmos_; }
  [[nodiscard]] bool showNormalDiagCube() const { return showNormalDiagCube_; }
  [[nodiscard]] bool showNormalValidation() const {
    return normalValidationSettings_.enabled;
  }
  [[nodiscard]] const container::gpu::NormalValidationSettings& normalValidationSettings() const {
    return normalValidationSettings_;
  }
  [[nodiscard]] GBufferViewMode gBufferViewMode() const { return gBufferViewMode_; }
  [[nodiscard]] const WireframeSettings& wireframeSettings() const {
    return wireframeSettings_;
  }
  [[nodiscard]] bool wireframeSupported() const { return wireframeSupported_; }
  [[nodiscard]] const std::string& statusMessage() const { return statusMessage_; }
  void setStatusMessage(std::string status) {
    statusMessage_ = std::move(status);
  }
  void setWireframeCapabilities(bool supported, bool wideLineSupported);

 private:
  void ensureInitialized() const;

  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  bool initialized_{false};
  bool showGeometryOverlay_{false};
  bool showLightGizmos_{true};
  bool showNormalDiagCube_{false};
  bool wireframeSupported_{false};
  bool wireframeWideLineSupported_{false};
  GBufferViewMode gBufferViewMode_{GBufferViewMode::Overview};
  WireframeSettings wireframeSettings_{};
  container::gpu::NormalValidationSettings normalValidationSettings_{};
  std::string gltfPathInput_{};
  std::string defaultModelPath_{
      };
  std::string statusMessage_{};
};

}  // namespace container::ui





