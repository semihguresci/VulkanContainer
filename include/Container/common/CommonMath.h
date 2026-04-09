#pragma once
#include <cstdint>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Right-handed coordinate system throughout (glTF native).
// Vulkan clip space: Y-down, depth [0,1].
//   - Y flip:      proj[1][1] *= -1  (done in Camera::viewProjection)
//   - Reverse-Z:   depth cleared to 0, compare GREATER_OR_EQUAL.
//                  near maps to 1, far maps to 0 — achieved by swapping
//                  near/far in the standard RH_ZO formulas.

namespace common::math {

// Standard RH view matrix (camera looks down -Z).
[[nodiscard]] inline glm::mat4 lookAt(const glm::vec3& eye,
                                      const glm::vec3& center,
                                      const glm::vec3& up) {
  return glm::lookAt(eye, center, up);
}

// RH reverse-Z perspective for Vulkan.
// Swapping near/far in perspectiveRH_ZO maps near→1 and far→0,
// matching the GREATER_OR_EQUAL depth compare and 0.0 depth clear.
[[nodiscard]] inline glm::mat4 perspectiveRH_ReverseZ(float fovRadians,
                                                      float aspectRatio,
                                                      float nearPlane,
                                                      float farPlane) {
  return glm::perspectiveRH_ZO(fovRadians, aspectRatio, farPlane, nearPlane);
}

// RH reverse-Z orthographic for Vulkan.
[[nodiscard]] inline glm::mat4 orthoRH_ReverseZ(float left, float right,
                                                float bottom, float top,
                                                float nearPlane,
                                                float farPlane) {
  return glm::orthoRH_ZO(left, right, bottom, top, farPlane, nearPlane);
}

}  // namespace common::math
