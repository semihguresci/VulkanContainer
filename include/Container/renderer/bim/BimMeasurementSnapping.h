#pragma once

#include "Container/common/CommonMath.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace container::renderer {

enum class BimSnapKind : uint32_t {
  None,
  Vertex,
  EdgeMidpoint,
  FaceCenter,
  BoundsCorner,
  BoundsCenter,
  FloorElevation,
};

struct BimSnapCandidate {
  BimSnapKind kind{BimSnapKind::None};
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  glm::vec3 worldPosition{0.0f};
  float screenDistancePixels{std::numeric_limits<float>::max()};
  std::string label{};
};

struct BimMeasurementResult {
  float distance{0.0f};
  float horizontalDistance{0.0f};
  float elevationDelta{0.0f};
  float angleDegrees{0.0f};
  float polygonArea{0.0f};
};

struct BimBoundsSnapInput {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  float floorElevation{0.0f};
  float screenDistancePixels{std::numeric_limits<float>::max()};
  std::string label{};
  bool includeFloorElevation{true};
};

[[nodiscard]] std::optional<BimSnapCandidate> BestBimSnapCandidate(
    std::span<const BimSnapCandidate> candidates,
    float maxScreenDistancePixels, std::span<const BimSnapKind> allowedKinds);

[[nodiscard]] BimMeasurementResult ComputeBimMeasurement(glm::vec3 a,
                                                         glm::vec3 b);

[[nodiscard]] float ComputeBimAngleDegrees(glm::vec3 a, glm::vec3 vertex,
                                           glm::vec3 c);

[[nodiscard]] float ComputeBimPolygonArea(std::span<const glm::vec3> points,
                                          glm::vec3 normalHint);

[[nodiscard]] std::vector<BimSnapCandidate>
BuildBimBoundsSnapCandidates(const BimBoundsSnapInput &bounds);

} // namespace container::renderer
