#pragma once

#include <cctype>
#include <string>
#include <string_view>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace container::geometry {

// Renderer-native assets follow glTF: +X right, +Y up, -Z forward.
[[nodiscard]] inline glm::mat4 zUpForwardYToRendererAxes() {
  glm::mat4 transform(1.0f);
  transform[0] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
  transform[1] = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
  transform[2] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
  transform[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  return transform;
}

[[nodiscard]] inline glm::mat4
usdUpAxisToRendererAxes(std::string_view upAxis) {
  std::string axis(upAxis);
  for (char &c : axis) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return axis == "Z" ? zUpForwardYToRendererAxes() : glm::mat4(1.0f);
}

} // namespace container::geometry
