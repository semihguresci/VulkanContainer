#include "Container/renderer/bim/BimMeasurementSnapping.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

namespace container::renderer {
namespace {

constexpr float kRadiansToDegrees = 57.29577951308232f;

[[nodiscard]] bool IsFinite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] bool IsAllowed(BimSnapKind kind,
                             std::span<const BimSnapKind> allowedKinds) {
  return kind != BimSnapKind::None &&
         std::ranges::find(allowedKinds, kind) != allowedKinds.end();
}

[[nodiscard]] uint32_t SnapPriority(BimSnapKind kind) {
  switch (kind) {
  case BimSnapKind::Vertex:
    return 0u;
  case BimSnapKind::EdgeMidpoint:
    return 1u;
  case BimSnapKind::FaceCenter:
    return 2u;
  case BimSnapKind::BoundsCorner:
    return 3u;
  case BimSnapKind::BoundsCenter:
    return 4u;
  case BimSnapKind::FloorElevation:
    return 5u;
  case BimSnapKind::None:
    break;
  }
  return std::numeric_limits<uint32_t>::max();
}

[[nodiscard]] float Length(glm::vec3 value) {
  return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

[[nodiscard]] glm::dvec3 ToDvec3(glm::vec3 value) {
  return {static_cast<double>(value.x), static_cast<double>(value.y),
          static_cast<double>(value.z)};
}

void AppendCandidate(std::vector<BimSnapCandidate> &candidates,
                     const BimBoundsSnapInput &bounds, BimSnapKind kind,
                     glm::vec3 position, std::string label) {
  if (!IsFinite(position)) {
    return;
  }

  candidates.push_back(BimSnapCandidate{.kind = kind,
                                        .objectIndex = bounds.objectIndex,
                                        .worldPosition = position,
                                        .screenDistancePixels =
                                            bounds.screenDistancePixels,
                                        .label = std::move(label)});
}

[[nodiscard]] std::string LabelWithSuffix(const BimBoundsSnapInput &bounds,
                                          std::string_view suffix) {
  if (bounds.label.empty()) {
    return std::string(suffix);
  }
  std::string label = bounds.label;
  label += ' ';
  label += suffix;
  return label;
}

} // namespace

std::optional<BimSnapCandidate> BestBimSnapCandidate(
    std::span<const BimSnapCandidate> candidates,
    float maxScreenDistancePixels, std::span<const BimSnapKind> allowedKinds) {
  if (allowedKinds.empty() || maxScreenDistancePixels < 0.0f ||
      !std::isfinite(maxScreenDistancePixels)) {
    return std::nullopt;
  }

  const BimSnapCandidate *best = nullptr;
  for (const BimSnapCandidate &candidate : candidates) {
    if (!IsAllowed(candidate.kind, allowedKinds) ||
        !std::isfinite(candidate.screenDistancePixels) ||
        candidate.screenDistancePixels > maxScreenDistancePixels ||
        !IsFinite(candidate.worldPosition)) {
      continue;
    }

    if (best == nullptr) {
      best = &candidate;
      continue;
    }

    const uint32_t candidatePriority = SnapPriority(candidate.kind);
    const uint32_t bestPriority = SnapPriority(best->kind);
    if (candidatePriority < bestPriority ||
        (candidatePriority == bestPriority &&
         (candidate.screenDistancePixels < best->screenDistancePixels ||
          (candidate.screenDistancePixels == best->screenDistancePixels &&
           candidate.objectIndex < best->objectIndex)))) {
      best = &candidate;
    }
  }

  if (best == nullptr) {
    return std::nullopt;
  }
  return *best;
}

BimMeasurementResult ComputeBimMeasurement(glm::vec3 a, glm::vec3 b) {
  if (!IsFinite(a) || !IsFinite(b)) {
    return {};
  }

  const glm::vec3 delta = b - a;
  const float horizontalDistance =
      std::sqrt(delta.x * delta.x + delta.z * delta.z);
  BimMeasurementResult result{};
  result.distance = Length(delta);
  result.horizontalDistance = horizontalDistance;
  result.elevationDelta = delta.y;
  result.angleDegrees =
      (horizontalDistance == 0.0f && delta.y == 0.0f)
          ? 0.0f
          : std::atan2(delta.y, horizontalDistance) * kRadiansToDegrees;
  return result;
}

float ComputeBimAngleDegrees(glm::vec3 a, glm::vec3 vertex, glm::vec3 c) {
  if (!IsFinite(a) || !IsFinite(vertex) || !IsFinite(c)) {
    return 0.0f;
  }

  const glm::vec3 ab = a - vertex;
  const glm::vec3 cb = c - vertex;
  const float abLength = Length(ab);
  const float cbLength = Length(cb);
  if (abLength <= 0.0f || cbLength <= 0.0f) {
    return 0.0f;
  }

  const float dot =
      (ab.x * cb.x + ab.y * cb.y + ab.z * cb.z) / (abLength * cbLength);
  return std::acos(std::clamp(dot, -1.0f, 1.0f)) * kRadiansToDegrees;
}

float ComputeBimPolygonArea(std::span<const glm::vec3> points,
                            glm::vec3 normalHint) {
  if (points.size() < 3u) {
    return 0.0f;
  }

  glm::dvec3 areaVector{0.0};
  for (size_t i = 0; i < points.size(); ++i) {
    const glm::vec3 current = points[i];
    const glm::vec3 next = points[(i + 1u) % points.size()];
    if (!IsFinite(current) || !IsFinite(next)) {
      return 0.0f;
    }
    areaVector += glm::cross(ToDvec3(current), ToDvec3(next));
  }

  const double hintLength = glm::length(ToDvec3(normalHint));
  if (std::isfinite(hintLength) && hintLength > 0.0) {
    const glm::dvec3 normal = ToDvec3(normalHint) / hintLength;
    return static_cast<float>(std::abs(glm::dot(areaVector, normal)) * 0.5);
  }

  return static_cast<float>(glm::length(areaVector) * 0.5);
}

std::vector<BimSnapCandidate>
BuildBimBoundsSnapCandidates(const BimBoundsSnapInput &bounds) {
  if (!IsFinite(bounds.min) || !IsFinite(bounds.max) ||
      (bounds.includeFloorElevation && !std::isfinite(bounds.floorElevation)) ||
      bounds.min.x > bounds.max.x || bounds.min.y > bounds.max.y ||
      bounds.min.z > bounds.max.z) {
    return {};
  }

  const glm::vec3 center = (bounds.min + bounds.max) * 0.5f;
  std::vector<BimSnapCandidate> candidates;
  candidates.reserve(28u);

  const std::array<float, 2> xs{bounds.min.x, bounds.max.x};
  const std::array<float, 2> ys{bounds.min.y, bounds.max.y};
  const std::array<float, 2> zs{bounds.min.z, bounds.max.z};

  for (const float x : xs) {
    for (const float y : ys) {
      for (const float z : zs) {
        AppendCandidate(candidates, bounds, BimSnapKind::BoundsCorner,
                        {x, y, z}, LabelWithSuffix(bounds, "corner"));
      }
    }
  }

  for (const float y : ys) {
    for (const float z : zs) {
      AppendCandidate(candidates, bounds, BimSnapKind::EdgeMidpoint,
                      {center.x, y, z}, LabelWithSuffix(bounds, "edge"));
    }
  }
  for (const float x : xs) {
    for (const float z : zs) {
      AppendCandidate(candidates, bounds, BimSnapKind::EdgeMidpoint,
                      {x, center.y, z}, LabelWithSuffix(bounds, "edge"));
    }
  }
  for (const float x : xs) {
    for (const float y : ys) {
      AppendCandidate(candidates, bounds, BimSnapKind::EdgeMidpoint,
                      {x, y, center.z}, LabelWithSuffix(bounds, "edge"));
    }
  }

  for (const float x : xs) {
    AppendCandidate(candidates, bounds, BimSnapKind::FaceCenter,
                    {x, center.y, center.z}, LabelWithSuffix(bounds, "face"));
  }
  for (const float y : ys) {
    AppendCandidate(candidates, bounds, BimSnapKind::FaceCenter,
                    {center.x, y, center.z}, LabelWithSuffix(bounds, "face"));
  }
  for (const float z : zs) {
    AppendCandidate(candidates, bounds, BimSnapKind::FaceCenter,
                    {center.x, center.y, z}, LabelWithSuffix(bounds, "face"));
  }

  AppendCandidate(candidates, bounds, BimSnapKind::BoundsCenter, center,
                  LabelWithSuffix(bounds, "center"));
  if (bounds.includeFloorElevation) {
    AppendCandidate(candidates, bounds, BimSnapKind::FloorElevation,
                    {center.x, bounds.floorElevation, center.z},
                    LabelWithSuffix(bounds, "floor"));
  }

  return candidates;
}

} // namespace container::renderer
