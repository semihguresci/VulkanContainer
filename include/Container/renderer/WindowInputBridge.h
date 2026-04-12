#pragma once

#include "Container/common/CommonGLFW.h"
#include "Container/utility/InputManager.h"

namespace container::renderer {

// Registers GLFW window callbacks and forwards input events to InputManager.
// Owns no Vulkan resources; lifetime must not exceed the window's lifetime.
//
// Usage:
//   WindowInputBridge bridge{nativeWindow, inputManager, framebufferResized};
//   // Callbacks remain active until bridge is destroyed.
class WindowInputBridge {
 public:
  // nativeWindow        : the GLFW window to attach callbacks to.
  // inputManager        : receives mouse / keyboard / focus events.
  // framebufferResized  : set to true on framebuffer-resize events.
  WindowInputBridge(GLFWwindow*                     nativeWindow,
                    container::window::InputManager&   inputManager,
                    bool&                           framebufferResized);

  ~WindowInputBridge();

  WindowInputBridge(const WindowInputBridge&)            = delete;
  WindowInputBridge& operator=(const WindowInputBridge&) = delete;

 private:
  // Lightweight per-window state forwarded to callbacks via user pointer.
  struct BridgeState {
    container::window::InputManager* inputManager{nullptr};
    bool*                         framebufferResized{nullptr};
  };

  GLFWwindow* window_{nullptr};
  BridgeState state_{};
};

}  // namespace container::renderer
