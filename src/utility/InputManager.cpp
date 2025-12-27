#include <Container/utility/Camera.h>
#include <Container/utility/InputManager.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>

namespace utility::input {

bool InputManager::update(float deltaTime) {
  if (!camera_) return false;

  const double originalDeltaX = pendingMouseDeltaX_;
  const double originalDeltaY = pendingMouseDeltaY_;
  const bool hadMouseDelta = (originalDeltaX != 0.0) || (originalDeltaY != 0.0);

  applyMouseInput();
  applyKeyboardInput(deltaTime);

  const bool moved = !pressedKeys_.empty();
  const bool mouseUpdated = hadMouseDelta;
  return moved || mouseUpdated;
}

void InputManager::enqueueMouseDelta(double xpos, double ypos) {
  if (firstMouseUpdate_) {
    lastMouseX_ = xpos;
    lastMouseY_ = ypos;
    firstMouseUpdate_ = false;
    return;
  }

  const double xoffset = xpos - lastMouseX_;
  const double yoffset = lastMouseY_ - ypos;
  lastMouseX_ = xpos;
  lastMouseY_ = ypos;

  if (!rightMouseDown_) return;

  pendingMouseDeltaX_ += xoffset;
  pendingMouseDeltaY_ += yoffset;
}

void InputManager::handleMouseButton(int button, int action) {
  if (button != GLFW_MOUSE_BUTTON_RIGHT) return;
  rightMouseDown_ = (action == GLFW_PRESS) || (action == GLFW_REPEAT);
}

void InputManager::handleKey(int key, int action) {
  if (action == GLFW_PRESS || action == GLFW_REPEAT) {
    pressedKeys_.insert(key);
  } else if (action == GLFW_RELEASE) {
    pressedKeys_.erase(key);
  }
}

void InputManager::applyMouseInput() {
  if (!camera_) return;

  if (!rightMouseDown_) {
    pendingMouseDeltaX_ = 0.0;
    pendingMouseDeltaY_ = 0.0;
    return;
  }

  if (pendingMouseDeltaX_ == 0.0 && pendingMouseDeltaY_ == 0.0) return;

  camera_->addYawPitch(
      static_cast<float>(pendingMouseDeltaX_) * mouseSensitivity_,
      static_cast<float>(pendingMouseDeltaY_) * mouseSensitivity_);

  pendingMouseDeltaX_ = 0.0;
  pendingMouseDeltaY_ = 0.0;
}

void InputManager::applyKeyboardInput(float deltaTime) {
  if (!camera_) return;
  if (pressedKeys_.empty()) return;

  const glm::vec3 front = camera_->frontVector();
  const glm::vec3 up = camera_->upVector(front);
  const glm::vec3 right = camera_->rightVector(front, up);
  const float velocity = moveSpeed_ * deltaTime;

  if (pressedKeys_.count(GLFW_KEY_W)) camera_->move(front, velocity);
  if (pressedKeys_.count(GLFW_KEY_S)) camera_->move(-front, velocity);
  if (pressedKeys_.count(GLFW_KEY_A)) camera_->move(-right, velocity);
  if (pressedKeys_.count(GLFW_KEY_D)) camera_->move(right, velocity);
  if (pressedKeys_.count(GLFW_KEY_E)) camera_->move(up, velocity);
  if (pressedKeys_.count(GLFW_KEY_Q)) camera_->move(-up, velocity);
}

}  // namespace utility::input
