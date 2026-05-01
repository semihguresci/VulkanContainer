#pragma once

#include <algorithm>

#include "Container/common/CommonMath.h"

namespace container::scene {

class BaseCamera {
 public:
  virtual ~BaseCamera() = default;

  void setPosition(const glm::vec3& position) { position_ = position; }
  [[nodiscard]] const glm::vec3& position() const noexcept { return position_; }
  void setScale(const glm::vec3& scale) {
    scale_ = glm::max(scale, glm::vec3(0.001f));
  }
  [[nodiscard]] const glm::vec3& scale() const noexcept { return scale_; }

  void setYawPitch(float yawDegrees, float pitchDegrees) {
    yawDegrees_ = yawDegrees;
    pitchDegrees_ = clampPitch(pitchDegrees);
  }
  [[nodiscard]] float yawDegrees() const noexcept { return yawDegrees_; }
  [[nodiscard]] float pitchDegrees() const noexcept { return pitchDegrees_; }

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
    front.y = std::sin(glm::radians(pitchDegrees_));
    // RH: camera looks down -Z, so Z component is negated.
    front.z = -std::sin(glm::radians(yawDegrees_)) *
              std::cos(glm::radians(pitchDegrees_));
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
    return container::math::lookAt(position_, position_ + front, up);
  }

  [[nodiscard]] glm::mat4 viewProjection(float aspectRatio) const {
    return projectionMatrix(aspectRatio) * viewMatrix();
  }

  virtual glm::mat4 projectionMatrix(float aspectRatio) const = 0;

 protected:
  glm::vec3 position_{0.0f, 0.0f, 0.0f};
  glm::vec3 scale_{1.0f, 1.0f, 1.0f};
  glm::vec3 worldUp_{0.0f, 1.0f, 0.0f};
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
    return container::math::perspectiveRH_ReverseZ(
        glm::radians(fieldOfViewDegrees_), aspectRatio, nearPlane_, farPlane_);
  }

  [[nodiscard]] float fieldOfViewDegrees() const noexcept { return fieldOfViewDegrees_; }
  void setFieldOfView(float fovDegrees) { fieldOfViewDegrees_ = fovDegrees; }
  [[nodiscard]] float nearPlane() const noexcept { return nearPlane_; }
  [[nodiscard]] float farPlane() const noexcept { return farPlane_; }
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
    return container::math::orthoRH_ReverseZ(-halfWidth, halfWidth, -halfHeight,
                                          halfHeight, nearPlane_, farPlane_);
  }

  void setViewHeight(float height) { viewHeight_ = height; }
  void setNearFar(float nearPlane, float farPlane) {
    nearPlane_ = nearPlane;
    farPlane_ = farPlane;
  }
  [[nodiscard]] float nearPlane() const noexcept { return nearPlane_; }
  [[nodiscard]] float farPlane() const noexcept { return farPlane_; }

 private:
  float viewHeight_{10.0f};
  float nearPlane_{0.1f};
  float farPlane_{100.0f};
};

}  // namespace container::scene
