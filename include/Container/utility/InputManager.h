#pragma once

#include <GLFW/glfw3.h>

#include <memory>

#include <glm/glm.hpp>

namespace utility::camera {
class BaseCamera;
}

namespace utility::input {

class InputManager {
 public:
  InputManager() = default;

  void setWindow(GLFWwindow* window) { window_ = window; }
  void setMoveSpeed(float speed) { moveSpeed_ = speed; }
  void setMouseSensitivity(float sensitivity) { mouseSensitivity_ = sensitivity; }

  void updateCamera(utility::camera::BaseCamera& camera, float deltaTime);

 private:
  void updateMouseLook(utility::camera::BaseCamera& camera);

  GLFWwindow* window_{nullptr};
  double lastMouseX_{0.0};
  double lastMouseY_{0.0};
  bool firstMouseUpdate_{true};
  float moveSpeed_{3.5f};
  float mouseSensitivity_{0.15f};
};

}  // namespace utility::input

