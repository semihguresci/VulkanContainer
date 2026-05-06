#include "Container/renderer/platform/WindowInputBridge.h"

namespace container::renderer {

WindowInputBridge::WindowInputBridge(GLFWwindow*                   nativeWindow,
                                     container::window::InputManager& inputManager,
                                     bool&                         framebufferResized)
    : window_(nativeWindow),
      state_{&inputManager, &framebufferResized} {
  glfwSetWindowUserPointer(window_, &state_);

  glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int, int) {
    auto* s = static_cast<BridgeState*>(glfwGetWindowUserPointer(w));
    if (s) *s->framebufferResized = true;
  });

  glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double x, double y) {
    auto* s = static_cast<BridgeState*>(glfwGetWindowUserPointer(w));
    if (s) s->inputManager->enqueueMouseDelta(x, y);
  });

  glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int btn, int action, int) {
    auto* s = static_cast<BridgeState*>(glfwGetWindowUserPointer(w));
    if (s) s->inputManager->handleMouseButton(btn, action);
  });

  glfwSetScrollCallback(window_, [](GLFWwindow* w, double xoffset,
                                    double yoffset) {
    auto* s = static_cast<BridgeState*>(glfwGetWindowUserPointer(w));
    if (s) s->inputManager->handleScroll(xoffset, yoffset);
  });

  glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int, int action, int) {
    auto* s = static_cast<BridgeState*>(glfwGetWindowUserPointer(w));
    if (s) s->inputManager->handleKey(key, action);
  });

  glfwSetWindowFocusCallback(window_, [](GLFWwindow* w, int focused) {
    auto* s = static_cast<BridgeState*>(glfwGetWindowUserPointer(w));
    if (s) s->inputManager->handleWindowFocus(focused == GLFW_TRUE);
  });
}

WindowInputBridge::~WindowInputBridge() {
  if (!window_) return;
  glfwSetFramebufferSizeCallback(window_, nullptr);
  glfwSetCursorPosCallback(window_,       nullptr);
  glfwSetMouseButtonCallback(window_,     nullptr);
  glfwSetScrollCallback(window_,          nullptr);
  glfwSetKeyCallback(window_,             nullptr);
  glfwSetWindowFocusCallback(window_,     nullptr);
  glfwSetWindowUserPointer(window_,       nullptr);
}

}  // namespace container::renderer
