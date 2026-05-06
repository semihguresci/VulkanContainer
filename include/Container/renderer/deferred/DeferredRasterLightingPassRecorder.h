#pragma once

#include "Container/common/CommonVulkan.h"

#include <array>

namespace container::scene {
class BaseCamera;
} // namespace container::scene

namespace container::ui {
class GuiManager;
} // namespace container::ui

namespace container::renderer {

class DebugOverlayRenderer;
class LightingManager;
class SceneController;
struct FrameRecordParams;

struct DeferredRasterLightingPassServices {
  VkExtent2D framebufferExtent{};
  const LightingManager *lightingManager{nullptr};
  const SceneController *sceneController{nullptr};
  const container::scene::BaseCamera *camera{nullptr};
  const container::ui::GuiManager *guiManager{nullptr};
  const DebugOverlayRenderer *debugOverlay{nullptr};
  bool tileCullPassActive{false};
};

class DeferredRasterLightingPassRecorder {
public:
  explicit DeferredRasterLightingPassRecorder(
      DeferredRasterLightingPassServices services);

  void record(VkCommandBuffer cmd, const FrameRecordParams &p,
              VkDescriptorSet sceneSet,
              const std::array<VkDescriptorSet, 2> &lightingSets,
              const std::array<VkDescriptorSet, 4> &transparentSets) const;

private:
  DeferredRasterLightingPassServices services_{};
};

} // namespace container::renderer
