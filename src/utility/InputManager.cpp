#include <Container/utility/Camera.h>
#include <Container/utility/InputManager.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>

namespace utility::input {

void InputManager::setWindow(GLFWwindow* window) {
  window_ = window;
  if (window_) {
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
  }
}

bool InputManager::update(float deltaTime) {
  if (!camera_) return false;

  const bool hadMouseDelta =
      (pendingMouseDeltaX_ != 0.0) || (pendingMouseDeltaY_ != 0.0);

  applyMouseInput();
  applyKeyboardInput(deltaTime);

  const bool moved = !pressedKeys_.empty();
  return moved || hadMouseDelta;
}

void InputManager::enqueueMouseDelta(double xpos, double ypos) {
  if (!lookModeActive_) return;

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

  pendingMouseDeltaX_ += xoffset;
  pendingMouseDeltaY_ += yoffset;
}

void InputManager::handleMouseButton(int button, int action) {
  if (button != GLFW_MOUSE_BUTTON_RIGHT) return;

  if (action == GLFW_PRESS) {
    setLookMode(true);
  } else if (action == GLFW_RELEASE) {
    setLookMode(false);
  }
}

void InputManager::handleKey(int key, int action) {
  if (action == GLFW_PRESS || action == GLFW_REPEAT) {
    pressedKeys_.insert(key);
  } else if (action == GLFW_RELEASE) {
    pressedKeys_.erase(key);
  }
}

void InputManager::handleWindowFocus(bool focused) {
  if (focused) return;

  pressedKeys_.clear();
  setLookMode(false);
}

void InputManager::applyMouseInput() {
  if (!camera_) return;

  if (!lookModeActive_) {
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
  const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
  const glm::vec3 cameraUp = camera_->upVector(front);

  glm::vec3 right = camera_->rightVector(front, cameraUp);
  right.y = 0.0f;
  if (glm::dot(right, right) > 0.000001f) {
    right = glm::normalize(right);
  } else {
    right = glm::vec3(1.0f, 0.0f, 0.0f);
  }

  glm::vec3 forward = front;
  forward.y = 0.0f;
  if (glm::dot(forward, forward) > 0.000001f) {
    forward = glm::normalize(forward);
  } else {
    forward = glm::normalize(glm::cross(right, worldUp));
  }

  float velocity = moveSpeed_ * deltaTime;
  if (pressedKeys_.count(GLFW_KEY_LEFT_SHIFT) ||
      pressedKeys_.count(GLFW_KEY_RIGHT_SHIFT)) {
    velocity *= 2.5f;
  }

  if (pressedKeys_.count(GLFW_KEY_W)) camera_->move(forward, velocity);
  if (pressedKeys_.count(GLFW_KEY_S)) camera_->move(-forward, velocity);
  if (pressedKeys_.count(GLFW_KEY_A)) camera_->move(-right, velocity);
  if (pressedKeys_.count(GLFW_KEY_D)) camera_->move(right, velocity);
  if (pressedKeys_.count(GLFW_KEY_SPACE) || pressedKeys_.count(GLFW_KEY_E)) {
    camera_->move(worldUp, velocity);
  }
  if (pressedKeys_.count(GLFW_KEY_LEFT_CONTROL) ||
      pressedKeys_.count(GLFW_KEY_RIGHT_CONTROL) ||
      pressedKeys_.count(GLFW_KEY_Q)) {
    camera_->move(-worldUp, velocity);
  }
}

void InputManager::setLookMode(bool enabled) {
  lookModeActive_ = enabled;
  pendingMouseDeltaX_ = 0.0;
  pendingMouseDeltaY_ = 0.0;
  firstMouseUpdate_ = true;

  if (!window_) return;

  glfwSetInputMode(window_, GLFW_CURSOR,
                   enabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

  if (glfwRawMouseMotionSupported()) {
    glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION,
                     enabled ? GLFW_TRUE : GLFW_FALSE);
  }
}

}  // namespace utility::input
