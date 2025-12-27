#pragma once

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
      const utility::scene::SceneGraph& sceneGraph, uint32_t maxSceneObjects,
      const std::function<void(const glm::mat4&)>& addObject,
      const std::function<void()>& addAutoOffsetObject,
      const std::function<bool(const std::string&)>& reloadModel,
      const std::function<bool()>& reloadDefault);

  bool isCapturingInput() const;
  const std::string& statusMessage() const { return statusMessage_; }
  void setStatusMessage(std::string status) {
    statusMessage_ = std::move(status);
  }

 private:
  void ensureInitialized() const;

  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  bool initialized_{false};
  std::string gltfPathInput_{};
  std::string defaultModelPath_{};
  std::string statusMessage_{};
  glm::vec3 newObjectTranslation_{0.0f, 0.0f, 0.0f};
};

}  // namespace utility::ui
