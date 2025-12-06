#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace utility::camera {

class BaseCamera {
 public:
  virtual ~BaseCamera() = default;

  void setPosition(const glm::vec3& position) { position_ = position; }
  [[nodiscard]] const glm::vec3& position() const { return position_; }

  void setYawPitch(float yawDegrees, float pitchDegrees) {
    yawDegrees_ = yawDegrees;
    pitchDegrees_ = clampPitch(pitchDegrees);
  }

  void addYawPitch(float yawOffset, float pitchOffset) {
    yawDegrees_ += yawOffset;
    pitchDegrees_ = clampPitch(pitchDegrees_ + pitchOffset);
  }

  void move(const glm::vec3& direction, float distance) {
    position_ += direction * distance;
  }

  [[nodiscard]] glm::vec3 frontVector() const {
    glm::vec3 front{};
    front.x = std::cos(glm::radians(yawDegrees_)) *
              std::cos(glm::radians(pitchDegrees_));
    front.y = std::sin(glm::radians(yawDegrees_)) *
              std::cos(glm::radians(pitchDegrees_));
    front.z = std::sin(glm::radians(pitchDegrees_));
    return glm::normalize(front);
  }

  [[nodiscard]] glm::vec3 upVector(const glm::vec3& front) const {
    const glm::vec3 right = glm::normalize(glm::cross(front, worldUp_));
    return glm::normalize(glm::cross(right, front));
  }

  [[nodiscard]] glm::vec3 rightVector(const glm::vec3& front,
                                      const glm::vec3& up) const {
    return glm::normalize(glm::cross(front, up));
  }

  [[nodiscard]] glm::mat4 viewMatrix() const {
    const glm::vec3 front = frontVector();
    const glm::vec3 up = upVector(front);
    return glm::lookAt(position_, position_ + front, up);
  }

  [[nodiscard]] glm::mat4 viewProjection(float aspectRatio) const {
    glm::mat4 proj = projectionMatrix(aspectRatio);
    proj[1][1] *= -1.0f;
    return proj * viewMatrix();
  }

  virtual glm::mat4 projectionMatrix(float aspectRatio) const = 0;

 protected:
  glm::vec3 position_{0.0f, 0.0f, 0.0f};
  glm::vec3 worldUp_{0.0f, 0.0f, 1.0f};
  float yawDegrees_{-135.0f};
  float pitchDegrees_{-35.0f};

 private:
  static float clampPitch(float pitchDegrees) {
    return std::clamp(pitchDegrees, -89.0f, 89.0f);
  }
};

class PerspectiveCamera : public BaseCamera {
 public:
  glm::mat4 projectionMatrix(float aspectRatio) const override {
    return glm::perspective(glm::radians(fieldOfViewDegrees_), aspectRatio,
                            nearPlane_, farPlane_);
  }

  void setFieldOfView(float fovDegrees) { fieldOfViewDegrees_ = fovDegrees; }
  void setNearFar(float nearPlane, float farPlane) {
    nearPlane_ = nearPlane;
    farPlane_ = farPlane;
  }

 private:
  float fieldOfViewDegrees_{60.0f};
  float nearPlane_{0.1f};
  float farPlane_{100.0f};
};

class OrthographicCamera : public BaseCamera {
 public:
  glm::mat4 projectionMatrix(float aspectRatio) const override {
    const float halfHeight = 0.5f * viewHeight_;
    const float halfWidth = halfHeight * aspectRatio;
    return glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight,
                      nearPlane_, farPlane_);
  }

  void setViewHeight(float height) { viewHeight_ = height; }
  void setNearFar(float nearPlane, float farPlane) {
    nearPlane_ = nearPlane;
    farPlane_ = farPlane;
  }

 private:
  float viewHeight_{10.0f};
  float nearPlane_{0.1f};
  float farPlane_{100.0f};
};

}  // namespace utility::camera

