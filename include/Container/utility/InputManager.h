// InputManager.h
#pragma once

#include <unordered_set>

namespace utility::camera {
class BaseCamera;
}

namespace utility::input {

class InputManager {
 public:
  InputManager() = default;

  void setMoveSpeed(float speed) { moveSpeed_ = speed; }
  void setMouseSensitivity(float sensitivity) {
    mouseSensitivity_ = sensitivity;
  }
  void setCamera(utility::camera::BaseCamera* camera) { camera_ = camera; }

  bool update(float deltaTime);

  void enqueueMouseDelta(double xpos, double ypos);
  void handleMouseButton(int button, int action);
  void handleKey(int key, int action);

 private:
  void applyMouseInput();
  void applyKeyboardInput(float deltaTime);

  utility::camera::BaseCamera* camera_{nullptr};
  std::unordered_set<int> pressedKeys_{};

  double lastMouseX_{0.0};
  double lastMouseY_{0.0};
  bool firstMouseUpdate_{true};
  bool rightMouseDown_{false};
  double pendingMouseDeltaX_{0.0};
  double pendingMouseDeltaY_{0.0};

  float moveSpeed_{3.5f};
  float mouseSensitivity_{0.15f};
};

}  // namespace utility::input
