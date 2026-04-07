#pragma once
#include <cstdint>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace common::math {

inline glm::mat4 leftHandedBasisFlip() {
  glm::mat4 flip(1.0f);
  flip[2][2] = -1.0f;
  return flip;
}

inline glm::vec3 toLeftHandedPosition(glm::vec3 value) {
  value.z = -value.z;
  return value;
}

inline glm::vec3 toLeftHandedDirection(glm::vec3 value) {
  value.z = -value.z;
  return value;
}

inline glm::vec4 toLeftHandedTangent(glm::vec4 value) {
  value.z = -value.z;
  value.w = -value.w;
  return value;
}

inline glm::mat4 toLeftHandedTransform(const glm::mat4& transform) {
  const glm::mat4 flip = leftHandedBasisFlip();
  return flip * transform * flip;
}

inline glm::mat4 lookAtLeftHanded(const glm::vec3& eye, const glm::vec3& center,
                                  const glm::vec3& up) {
  const glm::vec3 forward = glm::normalize(center - eye);
  const glm::vec3 right = glm::normalize(glm::cross(up, forward));
  const glm::vec3 actualUp = glm::cross(forward, right);

  glm::mat4 view(1.0f);
  view[0] = glm::vec4(right.x, actualUp.x, forward.x, 0.0f);
  view[1] = glm::vec4(right.y, actualUp.y, forward.y, 0.0f);
  view[2] = glm::vec4(right.z, actualUp.z, forward.z, 0.0f);
  view[3] =
      glm::vec4(-glm::dot(right, eye), -glm::dot(actualUp, eye),
                -glm::dot(forward, eye), 1.0f);
  return view;
}

inline glm::mat4 perspectiveLeftHandedZo(float fovRadians, float aspectRatio,
                                         float nearPlane, float farPlane) {
  const float tanHalfFov = std::tan(fovRadians * 0.5f);

  glm::mat4 projection(0.0f);
  projection[0][0] = 1.0f / (aspectRatio * tanHalfFov);
  projection[1][1] = 1.0f / tanHalfFov;
  projection[2][2] = farPlane / (farPlane - nearPlane);
  projection[2][3] = 1.0f;
  projection[3][2] = -(nearPlane * farPlane) / (farPlane - nearPlane);
  return projection;
}

inline glm::mat4 orthoLeftHandedZo(float left, float right, float bottom,
                                   float top, float nearPlane,
                                   float farPlane) {
  glm::mat4 projection(1.0f);
  projection[0][0] = 2.0f / (right - left);
  projection[1][1] = 2.0f / (top - bottom);
  projection[2][2] = 1.0f / (farPlane - nearPlane);
  projection[3][0] = -(right + left) / (right - left);
  projection[3][1] = -(top + bottom) / (top - bottom);
  projection[3][2] = -nearPlane / (farPlane - nearPlane);
  return projection;
}

}  // namespace common::math
