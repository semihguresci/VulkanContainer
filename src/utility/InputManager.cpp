#include <Container/utility/InputManager.h>

#include <Container/utility/Camera.h>

#include <glm/glm.hpp>

namespace utility::input {

void InputManager::updateMouseLook(utility::camera::BaseCamera& camera) {
  if (window_ == nullptr) return;

  double xpos = 0.0;
  double ypos = 0.0;
  glfwGetCursorPos(window_, &xpos, &ypos);

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

  if (glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) != GLFW_PRESS) {
    return;
  }

  camera.addYawPitch(static_cast<float>(xoffset) * mouseSensitivity_,
                     static_cast<float>(yoffset) * mouseSensitivity_);
}

void InputManager::updateCamera(utility::camera::BaseCamera& camera,
                                float deltaTime) {
  if (window_ == nullptr) return;

  updateMouseLook(camera);

  const glm::vec3 front = camera.frontVector();
  const glm::vec3 up = camera.upVector(front);
  const glm::vec3 right = camera.rightVector(front, up);

  const float velocity = moveSpeed_ * deltaTime;
  if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS) {
    camera.move(front, velocity);
  }
  if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS) {
    camera.move(-front, velocity);
  }
  if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS) {
    camera.move(-right, velocity);
  }
  if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS) {
    camera.move(right, velocity);
  }
  if (glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS) {
    camera.move(up, velocity);
  }
  if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) {
    camera.move(-up, velocity);
  }
}

}  // namespace utility::input

