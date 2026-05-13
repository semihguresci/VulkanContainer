#include "Container/renderer/bim/BimMeasurementSnapping.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

using container::renderer::BestBimSnapCandidate;
using container::renderer::BimBoundsSnapInput;
using container::renderer::BimMeasurementResult;
using container::renderer::BimSnapCandidate;
using container::renderer::BimSnapKind;
using container::renderer::BuildBimBoundsSnapCandidates;
using container::renderer::ComputeBimAngleDegrees;
using container::renderer::ComputeBimMeasurement;
using container::renderer::ComputeBimPolygonArea;

TEST(BimMeasurementSnappingTests, PrefersClosestVertexWithinPixelRadius) {
  const std::array candidates{
      BimSnapCandidate{.kind = BimSnapKind::Vertex,
                       .objectIndex = 1u,
                       .worldPosition = {1.0f, 0.0f, 0.0f},
                       .screenDistancePixels = 8.0f,
                       .label = "far vertex"},
      BimSnapCandidate{.kind = BimSnapKind::Vertex,
                       .objectIndex = 2u,
                       .worldPosition = {2.0f, 0.0f, 0.0f},
                       .screenDistancePixels = 2.0f,
                       .label = "near vertex"},
      BimSnapCandidate{.kind = BimSnapKind::Vertex,
                       .objectIndex = 3u,
                       .worldPosition = {3.0f, 0.0f, 0.0f},
                       .screenDistancePixels = 12.0f,
                       .label = "outside vertex"}};
  const std::array allowed{BimSnapKind::Vertex};

  const std::optional<BimSnapCandidate> best =
      BestBimSnapCandidate(candidates, 10.0f, allowed);

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->objectIndex, 2u);
  EXPECT_EQ(best->label, "near vertex");

  const std::optional<BimSnapCandidate> outside =
      BestBimSnapCandidate(candidates, 1.0f, allowed);
  EXPECT_FALSE(outside.has_value());
}

TEST(BimMeasurementSnappingTests, FallsBackToEdgeMidpointThenBoundsCenter) {
  const std::array candidates{
      BimSnapCandidate{.kind = BimSnapKind::Vertex,
                       .objectIndex = 1u,
                       .worldPosition = {1.0f, 0.0f, 0.0f},
                       .screenDistancePixels = 1.0f,
                       .label = "disallowed vertex"},
      BimSnapCandidate{.kind = BimSnapKind::BoundsCenter,
                       .objectIndex = 2u,
                       .worldPosition = {2.0f, 0.0f, 0.0f},
                       .screenDistancePixels = 2.0f,
                       .label = "closer bounds center"},
      BimSnapCandidate{.kind = BimSnapKind::EdgeMidpoint,
                       .objectIndex = 3u,
                       .worldPosition = {3.0f, 0.0f, 0.0f},
                       .screenDistancePixels = 7.0f,
                       .label = "priority edge midpoint"}};
  const std::array allowed{BimSnapKind::EdgeMidpoint,
                           BimSnapKind::BoundsCenter};

  const std::optional<BimSnapCandidate> best =
      BestBimSnapCandidate(candidates, 10.0f, allowed);

  ASSERT_TRUE(best.has_value());
  EXPECT_EQ(best->kind, BimSnapKind::EdgeMidpoint);
  EXPECT_EQ(best->label, "priority edge midpoint");

  const std::array onlyBounds{BimSnapKind::BoundsCenter};
  const std::optional<BimSnapCandidate> fallback =
      BestBimSnapCandidate(candidates, 10.0f, onlyBounds);

  ASSERT_TRUE(fallback.has_value());
  EXPECT_EQ(fallback->kind, BimSnapKind::BoundsCenter);
}

TEST(BimMeasurementSnappingTests, ReportsDistanceAngleAreaAndElevationDelta) {
  const BimMeasurementResult measurement =
      ComputeBimMeasurement({0.0f, 1.0f, 0.0f}, {3.0f, 5.0f, 4.0f});

  EXPECT_NEAR(measurement.distance, std::sqrt(41.0f), 1.0e-5f);
  EXPECT_NEAR(measurement.horizontalDistance, 5.0f, 1.0e-5f);
  EXPECT_NEAR(measurement.elevationDelta, 4.0f, 1.0e-5f);
  EXPECT_NEAR(measurement.angleDegrees, 38.659809f, 1.0e-4f);

  EXPECT_NEAR(ComputeBimAngleDegrees({1.0f, 0.0f, 0.0f},
                                     {0.0f, 0.0f, 0.0f},
                                     {0.0f, 0.0f, 1.0f}),
              90.0f, 1.0e-5f);

  const std::vector<glm::vec3> polygon{{0.0f, 0.0f, 0.0f},
                                       {4.0f, 0.0f, 0.0f},
                                       {4.0f, 0.0f, 3.0f},
                                       {0.0f, 0.0f, 3.0f}};
  EXPECT_NEAR(ComputeBimPolygonArea(polygon, {0.0f, 1.0f, 0.0f}), 12.0f,
              1.0e-5f);
}

TEST(BimMeasurementSnappingTests,
     BuildsBoundsCandidatesWhenMissingFloorElevationIsDisabled) {
  BimBoundsSnapInput bounds{};
  bounds.objectIndex = 42u;
  bounds.min = {-1.0f, 0.0f, -2.0f};
  bounds.max = {3.0f, 4.0f, 2.0f};
  bounds.floorElevation = std::numeric_limits<float>::quiet_NaN();
  bounds.includeFloorElevation = false;

  const std::vector<BimSnapCandidate> candidates =
      BuildBimBoundsSnapCandidates(bounds);

  EXPECT_EQ(candidates.size(), 27u);
  EXPECT_TRUE(std::ranges::any_of(candidates, [](const BimSnapCandidate &snap) {
    return snap.kind == BimSnapKind::BoundsCenter;
  }));
  EXPECT_FALSE(std::ranges::any_of(candidates, [](const BimSnapCandidate &snap) {
    return snap.kind == BimSnapKind::FloorElevation;
  }));
}

TEST(BimMeasurementSnappingTests,
     ComputesTrueThreeDimensionalTriangleAreaWithoutProjectionNormal) {
  const std::array<glm::vec3, 3> verticalTriangle{
      glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 3.0f, 0.0f},
      glm::vec3{0.0f, 0.0f, 4.0f}};

  EXPECT_NEAR(ComputeBimPolygonArea(verticalTriangle, {0.0f, 0.0f, 0.0f}),
              6.0f, 1.0e-5f);
  EXPECT_NEAR(ComputeBimPolygonArea(verticalTriangle, {0.0f, 1.0f, 0.0f}),
              0.0f, 1.0e-5f);
}
