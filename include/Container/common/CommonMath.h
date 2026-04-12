#pragma once

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Right-handed coordinate system throughout (glTF native).
// Vulkan clip space: Y-down, depth [0,1].
//   - Y flip:    proj[1][1] *= -1 (done in Camera::viewProjection)
//   - Reverse-Z: depth cleared to 0, compare GREATER_OR_EQUAL.
//                The helpers below map near -> 1 and far -> 0 explicitly.

namespace container::math {

// Standard RH view matrix (camera looks down -Z).
[[nodiscard]] inline glm::mat4 lookAt(const glm::vec3& eye,
                                      const glm::vec3& center,
                                      const glm::vec3& up) {
  return glm::lookAt(eye, center, up);
}

// RH reverse-Z perspective for Vulkan zero-to-one depth.
[[nodiscard]] inline glm::mat4 perspectiveRH_ReverseZ(float fovRadians,
                                                      float aspectRatio,
                                                      float nearPlane,
                                                      float farPlane) {
  const float tanHalfFov = std::tan(fovRadians * 0.5f);

  glm::mat4 result(0.0f);
  result[0][0] = 1.0f / (aspectRatio * tanHalfFov);
  result[1][1] = 1.0f / tanHalfFov;
  result[2][2] = nearPlane / (farPlane - nearPlane);
  result[2][3] = -1.0f;
  result[3][2] = (farPlane * nearPlane) / (farPlane - nearPlane);
  return result;
}

// RH reverse-Z orthographic projection for Vulkan zero-to-one depth.
[[nodiscard]] inline glm::mat4 orthoRH_ReverseZ(float left, float right,
                                                float bottom, float top,
                                                float nearPlane,
                                                float farPlane) {
  glm::mat4 result(1.0f);
  result[0][0] = 2.0f / (right - left);
  result[1][1] = 2.0f / (top - bottom);
  result[2][2] = 1.0f / (farPlane - nearPlane);
  result[3][0] = -(right + left) / (right - left);
  result[3][1] = -(top + bottom) / (top - bottom);
  result[3][2] = farPlane / (farPlane - nearPlane);
  return result;
}

}  // namespace container::math
