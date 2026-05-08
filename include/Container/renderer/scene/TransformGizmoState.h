#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace container::ui {
enum class TransformAxis : uint32_t;
enum class TransformSpace : uint32_t;
enum class ViewportTool : uint32_t;
}  // namespace container::ui

namespace container::renderer {

struct FrameTransformGizmoState {
  bool visible{false};
  container::ui::ViewportTool tool{};
  container::ui::TransformSpace transformSpace{};
  container::ui::TransformAxis activeAxis{};
  glm::vec3 origin{0.0f};
  float scale{1.0f};
  glm::vec3 axisX{1.0f, 0.0f, 0.0f};
  float radius{1.0f};
  glm::vec3 axisY{0.0f, 1.0f, 0.0f};
  float padding0{0.0f};
  glm::vec3 axisZ{0.0f, 0.0f, 1.0f};
  float padding1{0.0f};
};

}  // namespace container::renderer
