#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

namespace container::renderer {

struct BimScheduleBounds {
  bool valid{false};
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
};

struct BimScheduleElement {
  std::string guid{};
  std::string sourceId{};
  std::string ifcClass{};
  std::string type{};
  std::string storey{};
  std::string material{};
  BimScheduleBounds bounds{};
};

struct BimScheduleRow {
  std::string key{};
  std::string storey{};
  std::string material{};
  uint32_t count{0};
  float estimatedArea{0.0f};
  float estimatedVolume{0.0f};
};

[[nodiscard]] float estimateBimScheduleBoundsArea(
    const BimScheduleBounds& bounds);
[[nodiscard]] float estimateBimScheduleBoundsVolume(
    const BimScheduleBounds& bounds);

[[nodiscard]] std::vector<BimScheduleRow> buildBimScheduleByIfcClassAndStorey(
    std::span<const BimScheduleElement> elements);
[[nodiscard]] std::vector<BimScheduleRow> buildBimScheduleByTypeAndStorey(
    std::span<const BimScheduleElement> elements);
[[nodiscard]] std::vector<BimScheduleRow> buildBimScheduleMaterialTotals(
    std::span<const BimScheduleElement> elements);

} // namespace container::renderer
