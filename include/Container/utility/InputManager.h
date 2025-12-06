#pragma once

#include <GLFW/glfw3.h>

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <glm/glm.hpp>

namespace utility::camera {
class BaseCamera;
}

namespace utility::input {

class InputManager {
 public:
  InputManager() = default;

  void bindWindow(GLFWwindow* window);
  void setMoveSpeed(float speed) { moveSpeed_ = speed; }
  void setMouseSensitivity(float sensitivity) { mouseSensitivity_ = sensitivity; }

  void setCamera(utility::camera::BaseCamera* camera) { camera_ = camera; }

  // Returns true if the camera state changed during this update tick.
  bool update(float deltaTime);

 private:
  void enqueueMouseDelta(double xpos, double ypos);
  void handleMouseButton(int button, int action);
  void handleKey(int key, int action);

  void applyMouseInput();
  void applyKeyboardInput(float deltaTime);

  static void CursorPositionThunk(GLFWwindow* window, double xpos,
                                  double ypos);
  static void MouseButtonThunk(GLFWwindow* window, int button, int action,
                               int mods);
  static void KeyThunk(GLFWwindow* window, int key, int scancode, int action,
                       int mods);

  GLFWwindow* window_{nullptr};
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

