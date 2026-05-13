#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace container::renderer {

struct BimModelCompareBounds {
  bool valid{false};
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
};

struct BimModelCompareElement {
  std::string guid{};
  std::string sourceId{};
  std::string ifcClass{};
  std::string type{};
  std::string storey{};
  std::string material{};
  BimModelCompareBounds bounds{};
};

enum class BimModelCompareChangeKind : uint32_t {
  Added = 0,
  Removed = 1,
  ChangedType = 2,
  ChangedStorey = 3,
  ChangedMaterial = 4,
  ChangedBounds = 5,
};

struct BimModelCompareOptions {
  float boundsTolerance{0.001f};
};

struct BimModelCompareChange {
  BimModelCompareChangeKind kind{BimModelCompareChangeKind::Added};
  std::string identity{};
  size_t beforeIndex{std::numeric_limits<size_t>::max()};
  size_t afterIndex{std::numeric_limits<size_t>::max()};
  std::string beforeValue{};
  std::string afterValue{};
};

struct BimModelCompareResult {
  std::vector<BimModelCompareChange> changes{};
};

[[nodiscard]] BimModelCompareResult compareBimModels(
    std::span<const BimModelCompareElement> before,
    std::span<const BimModelCompareElement> after,
    const BimModelCompareOptions& options = {});

} // namespace container::renderer
