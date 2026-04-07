#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "Container/app/AppConfig.h"

#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonGLFW.h"
#include "Container/common/CommonMath.h"

#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"

namespace utility::ui {

enum class GBufferViewMode : uint32_t {
  Lit = 0,
  Albedo = 1,
  Normals = 2,
  Material = 3,
  Depth = 4,
  Emissive = 5,
  Transparency = 6,
  Revealage = 7,
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
      const utility::scene::SceneGraph& sceneGraph,
      const std::function<bool(const std::string&)>& reloadModel,
      const std::function<bool()>& reloadDefault,
      const TransformControls& cameraTransform,
      const std::function<void(const TransformControls&)>& applyCameraTransform,
      const TransformControls& sceneTransform,
      const std::function<void(const TransformControls&)>& applySceneTransform,
      uint32_t selectedMeshNode,
      const std::function<void(uint32_t)>& selectMeshNode,
      const TransformControls& meshTransform,
      const std::function<void(uint32_t, const TransformControls&)>&
          applyMeshTransform);

  [[nodiscard]] bool isCapturingInput() const;
  [[nodiscard]] bool showGeometryOverlay() const { return showGeometryOverlay_; }
  [[nodiscard]] bool showLightGizmos() const { return showLightGizmos_; }
  [[nodiscard]] GBufferViewMode gBufferViewMode() const { return gBufferViewMode_; }
  [[nodiscard]] const std::string& statusMessage() const { return statusMessage_; }
  void setStatusMessage(std::string status) {
    statusMessage_ = std::move(status);
  }

 private:
  void ensureInitialized() const;

  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  bool initialized_{false};
  bool showGeometryOverlay_{false};
  bool showLightGizmos_{true};
  GBufferViewMode gBufferViewMode_{GBufferViewMode::Lit};
  std::string gltfPathInput_{};
  std::string defaultModelPath_{};
  std::string statusMessage_{};
};

}  // namespace utility::ui
