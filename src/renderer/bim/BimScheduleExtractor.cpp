#include "Container/renderer/bim/BimScheduleExtractor.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

namespace container::renderer {
namespace {

constexpr std::string_view kUnassigned = "(unassigned)";
constexpr std::string_view kUnclassified = "(unclassified)";

[[nodiscard]] bool finiteVec3(const glm::vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] bool validBounds(const BimScheduleBounds& bounds) {
  return bounds.valid && finiteVec3(bounds.min) && finiteVec3(bounds.max) &&
         bounds.max.x >= bounds.min.x && bounds.max.y >= bounds.min.y &&
         bounds.max.z >= bounds.min.z;
}

[[nodiscard]] std::string normalizedOr(std::string_view value,
                                       std::string_view fallback) {
  return value.empty() ? std::string(fallback) : std::string(value);
}

[[nodiscard]] std::string classKey(const BimScheduleElement& element) {
  if (!element.ifcClass.empty()) {
    return element.ifcClass;
  }
  if (!element.type.empty()) {
    return element.type;
  }
  return std::string(kUnclassified);
}

[[nodiscard]] std::string typeKey(const BimScheduleElement& element) {
  if (!element.type.empty()) {
    return element.type;
  }
  if (!element.ifcClass.empty()) {
    return element.ifcClass;
  }
  return std::string(kUnclassified);
}

using ScheduleGroupKey = std::tuple<std::string, std::string, std::string>;

void addElementToRow(BimScheduleRow& row,
                     const BimScheduleElement& element) {
  ++row.count;
  row.estimatedArea += estimateBimScheduleBoundsArea(element.bounds);
  row.estimatedVolume += estimateBimScheduleBoundsVolume(element.bounds);
}

template <typename KeySelector>
[[nodiscard]] std::vector<BimScheduleRow> buildGroupedSchedule(
    std::span<const BimScheduleElement> elements, KeySelector keySelector,
    bool includeMaterial) {
  std::map<ScheduleGroupKey, BimScheduleRow> groupedRows;
  for (const BimScheduleElement& element : elements) {
    const std::string key = keySelector(element);
    const std::string storey =
        includeMaterial ? std::string{} : normalizedOr(element.storey, kUnassigned);
    const std::string material =
        includeMaterial ? normalizedOr(element.material, kUnassigned)
                        : std::string{};
    BimScheduleRow& row = groupedRows[{key, storey, material}];
    row.key = key;
    row.storey = storey;
    row.material = material;
    addElementToRow(row, element);
  }

  std::vector<BimScheduleRow> rows;
  rows.reserve(groupedRows.size());
  for (auto& [unusedKey, row] : groupedRows) {
    rows.push_back(std::move(row));
  }
  return rows;
}

} // namespace

float estimateBimScheduleBoundsArea(const BimScheduleBounds& bounds) {
  if (!validBounds(bounds)) {
    return 0.0f;
  }
  const glm::vec3 size = bounds.max - bounds.min;
  return 2.0f * (size.x * size.y + size.x * size.z + size.y * size.z);
}

float estimateBimScheduleBoundsVolume(const BimScheduleBounds& bounds) {
  if (!validBounds(bounds)) {
    return 0.0f;
  }
  const glm::vec3 size = bounds.max - bounds.min;
  return size.x * size.y * size.z;
}

std::vector<BimScheduleRow> buildBimScheduleByIfcClassAndStorey(
    std::span<const BimScheduleElement> elements) {
  return buildGroupedSchedule(elements, classKey, false);
}

std::vector<BimScheduleRow> buildBimScheduleByTypeAndStorey(
    std::span<const BimScheduleElement> elements) {
  return buildGroupedSchedule(elements, typeKey, false);
}

std::vector<BimScheduleRow> buildBimScheduleMaterialTotals(
    std::span<const BimScheduleElement> elements) {
  return buildGroupedSchedule(
      elements,
      [](const BimScheduleElement& element) {
        return normalizedOr(element.material, kUnassigned);
      },
      true);
}

} // namespace container::renderer
