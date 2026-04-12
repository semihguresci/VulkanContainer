// InputManager.h
#pragma once

#include <unordered_set>

struct GLFWwindow;

namespace container::scene {
class BaseCamera;
}

namespace container::window {

class InputManager {
 public:
  InputManager() = default;

  void setMoveSpeed(float speed) { moveSpeed_ = speed; }
  void setMouseSensitivity(float sensitivity) {
    mouseSensitivity_ = sensitivity;
  }
  void setCamera(container::scene::BaseCamera* camera) { camera_ = camera; }
  void setWindow(GLFWwindow* window);

  [[nodiscard]] bool isLooking() const { return lookModeActive_; }

  bool update(float deltaTime);

  void enqueueMouseDelta(double xpos, double ypos);
  void handleMouseButton(int button, int action);
  void handleKey(int key, int action);
  void handleWindowFocus(bool focused);

 private:
  void applyMouseInput();
  void applyKeyboardInput(float deltaTime);
  void setLookMode(bool enabled);

  container::scene::BaseCamera* camera_{nullptr};
  GLFWwindow* window_{nullptr};
  std::unordered_set<int> pressedKeys_{};

  double lastMouseX_{0.0};
  double lastMouseY_{0.0};
  bool firstMouseUpdate_{true};
  bool lookModeActive_{false};
  double pendingMouseDeltaX_{0.0};
  double pendingMouseDeltaY_{0.0};

  float moveSpeed_{3.5f};
  float mouseSensitivity_{0.10f};
};

}  // namespace container::window
