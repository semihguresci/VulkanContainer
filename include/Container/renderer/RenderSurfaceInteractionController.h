#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <string>

#include "Container/utility/GuiManager.h"

namespace container::window {
class InputManager;
struct InputFrameSnapshot;
}  // namespace container::window

namespace container::renderer {

class CameraController;
struct DebugRenderState;

// Owns render-space input routing. It decides when raw input belongs to ImGui,
// an active viewport gesture, camera navigation, or debug shortcuts.
class RenderSurfaceInteractionController {
 public:
  struct Context {
    container::window::InputManager& inputManager;
    CameraController& cameraController;
    DebugRenderState& debugState;
    float deltaTime{0.0f};
    bool guiCapturingMouse{false};
    bool guiCapturingKeyboard{false};
    uint32_t selectedMeshNode{std::numeric_limits<uint32_t>::max()};
    bool hasSelection{false};
    std::function<void(double, double)> selectAtCursor{};
    std::function<void(double, double)> hoverAtCursor{};
    std::function<void()> clearHover{};
    std::function<void()> clearSelection{};
    std::function<void(container::ui::ViewportTool,
                       container::ui::TransformSpace,
                       container::ui::TransformAxis,
                       double,
                       double)> transformSelectedByDrag{};
    std::function<void(std::string)> setStatusMessage{};
    std::function<void()> onCullingUnfrozen{};
  };

  void process(Context context);
  [[nodiscard]] const container::ui::ViewportInteractionState& state() const {
    return state_;
  }
  void setTool(container::ui::ViewportTool tool);
  void setTransformSpace(container::ui::TransformSpace transformSpace);
  void setTransformAxis(container::ui::TransformAxis transformAxis);

 private:
  void handleDebugShortcuts(const container::window::InputFrameSnapshot& input,
                            Context& context, bool shortcutsAllowed);
  void handleToolShortcuts(const container::window::InputFrameSnapshot& input,
                           Context& context, bool shortcutsAllowed);
  void handleLookOwnership(const container::window::InputFrameSnapshot& input,
                           Context& context);
  void handleViewportActions(
      const container::window::InputFrameSnapshot& input, Context& context,
      bool viewportInputAllowed);

  void setGesture(container::ui::ViewportGesture gesture);

  container::ui::ViewportInteractionState state_{};
  int activePointerButton_{-1};
};

}  // namespace container::renderer
