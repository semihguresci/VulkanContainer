#include "Container/renderer/bim/BimCoordinationOverlay.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace {

using container::renderer::BimCoordinationOverlayBounds;
using container::renderer::BimCoordinationOverlayBuildOptions;
using container::renderer::BimCoordinationOverlayClashPair;
using container::renderer::BimCoordinationOverlayElement;
using container::renderer::BimCoordinationOverlayInputs;
using container::renderer::BimCoordinationOverlayIssuePin;
using container::renderer::BimCoordinationOverlayKind;
using container::renderer::buildBimCoordinationOverlay;
using container::renderer::buildBimCoordinationOverlayMarkerWirePrimitives;

[[nodiscard]] BimCoordinationOverlayBounds bounds(glm::vec3 min,
                                                  glm::vec3 max) {
  return {.min = min, .max = max, .valid = true};
}

[[nodiscard]] BimCoordinationOverlayBuildOptions onlySpaces() {
  return {.spacesEnabled = true,
          .mepXrayEnabled = false,
          .clashesEnabled = false,
          .issuePinsEnabled = false};
}

[[nodiscard]] BimCoordinationOverlayBuildOptions onlyMepXray() {
  return {.spacesEnabled = false,
          .mepXrayEnabled = true,
          .clashesEnabled = false,
          .issuePinsEnabled = false};
}

[[nodiscard]] BimCoordinationOverlayBuildOptions onlyClashes() {
  return {.spacesEnabled = false,
          .mepXrayEnabled = false,
          .clashesEnabled = true,
          .issuePinsEnabled = false};
}

[[nodiscard]] BimCoordinationOverlayBuildOptions onlyIssuePins() {
  return {.spacesEnabled = false,
          .mepXrayEnabled = false,
          .clashesEnabled = false,
          .issuePinsEnabled = true};
}

TEST(BimCoordinationOverlayTests, BuildsTransparentSpaceOverlayFromIfcSpace) {
  const std::array elements{
      BimCoordinationOverlayElement{.objectIndex = 11u,
                                    .ifcClass = "IfcWall",
                                    .name = "Partition",
                                    .bounds = bounds({0.0f, 0.0f, 0.0f},
                                                     {1.0f, 3.0f, 0.3f})},
      BimCoordinationOverlayElement{.objectIndex = 42u,
                                    .ifcClass = "IfcSpace",
                                    .name = "Room 101",
                                    .bounds = bounds({1.0f, 0.0f, 2.0f},
                                                     {5.0f, 3.0f, 8.0f})},
  };

  const auto result = buildBimCoordinationOverlay(
      {.elements = elements, .options = onlySpaces()});

  ASSERT_EQ(result.elementOverlays.size(), 1u);
  const auto &space = result.elementOverlays.front();
  EXPECT_EQ(space.kind, BimCoordinationOverlayKind::Space);
  EXPECT_EQ(space.objectIndex, 42u);
  EXPECT_EQ(space.label, "Room 101");
  EXPECT_LT(space.opacity, 1.0f);
  EXPECT_TRUE(space.transparent);
  EXPECT_EQ(space.bounds.min, glm::vec3(1.0f, 0.0f, 2.0f));
  EXPECT_EQ(space.bounds.max, glm::vec3(5.0f, 3.0f, 8.0f));
  EXPECT_TRUE(result.markers.empty());
}

TEST(BimCoordinationOverlayTests,
     BuildsMepXrayOverlayForPipeAndDuctClasses) {
  const std::array elements{
      BimCoordinationOverlayElement{.objectIndex = 1u,
                                    .ifcClass = "IfcWall",
                                    .name = "Wall",
                                    .bounds = bounds({0.0f, 0.0f, 0.0f},
                                                     {1.0f, 2.0f, 0.2f})},
      BimCoordinationOverlayElement{.objectIndex = 2u,
                                    .ifcClass = "IfcPipeSegment",
                                    .name = "CHW pipe",
                                    .bounds = bounds({2.0f, 0.0f, 0.0f},
                                                     {3.0f, 1.0f, 1.0f})},
      BimCoordinationOverlayElement{.objectIndex = 3u,
                                    .ifcClass = "IfcFlowSegment",
                                    .type = "Supply duct",
                                    .bounds = bounds({4.0f, 0.0f, 0.0f},
                                                     {6.0f, 1.0f, 1.0f})},
      BimCoordinationOverlayElement{.objectIndex = 4u,
                                    .ifcClass = "IfcCableCarrierSegment",
                                    .name = "Cable tray",
                                    .bounds = bounds({7.0f, 0.0f, 0.0f},
                                                     {8.0f, 1.0f, 1.0f})},
      BimCoordinationOverlayElement{.objectIndex = 5u,
                                    .ifcClass = "IfcServiceSegment",
                                    .type = "Electrical service",
                                    .bounds = bounds({9.0f, 0.0f, 0.0f},
                                                     {10.0f, 1.0f, 1.0f})},
  };

  const auto result = buildBimCoordinationOverlay(
      {.elements = elements, .options = onlyMepXray()});

  ASSERT_EQ(result.elementOverlays.size(), 4u);
  std::vector<uint32_t> objectIndices;
  for (const auto &overlay : result.elementOverlays) {
    EXPECT_EQ(overlay.kind, BimCoordinationOverlayKind::MepXray);
    EXPECT_TRUE(overlay.transparent);
    EXPECT_GT(overlay.opacity, 0.0f);
    EXPECT_LT(overlay.opacity, 1.0f);
    objectIndices.push_back(overlay.objectIndex);
  }
  std::ranges::sort(objectIndices);
  EXPECT_EQ(objectIndices, (std::vector<uint32_t>{2u, 3u, 4u, 5u}));
}

TEST(BimCoordinationOverlayTests, SkipsMepXrayOverlayWithoutValidBounds) {
  const std::array elements{
      BimCoordinationOverlayElement{.objectIndex = 2u,
                                    .ifcClass = "IfcPipeSegment",
                                    .name = "Unbounded pipe"},
      BimCoordinationOverlayElement{.objectIndex = 3u,
                                    .ifcClass = "IfcDuctSegment",
                                    .name = "Bounded duct",
                                    .bounds = bounds({4.0f, 0.0f, 0.0f},
                                                     {6.0f, 1.0f, 1.0f})},
  };

  const auto result = buildBimCoordinationOverlay(
      {.elements = elements, .options = onlyMepXray()});

  ASSERT_EQ(result.elementOverlays.size(), 1u);
  EXPECT_EQ(result.elementOverlays.front().kind,
            BimCoordinationOverlayKind::MepXray);
  EXPECT_EQ(result.elementOverlays.front().objectIndex, 3u);
}

TEST(BimCoordinationOverlayTests, BuildsClashPinsFromElementPairs) {
  const std::array clashes{
      BimCoordinationOverlayClashPair{.primaryObjectIndex = 12u,
                                      .secondaryObjectIndex = 44u,
                                      .hasPosition = true,
                                      .position = {3.0f, 4.0f, 5.0f},
                                      .label = "Duct vs beam"},
  };

  const auto result = buildBimCoordinationOverlay(
      {.clashPairs = clashes, .options = onlyClashes()});

  ASSERT_EQ(result.markers.size(), 1u);
  const auto &marker = result.markers.front();
  EXPECT_EQ(marker.kind, BimCoordinationOverlayKind::Clash);
  EXPECT_EQ(marker.primaryObjectIndex, 12u);
  EXPECT_EQ(marker.secondaryObjectIndex, 44u);
  EXPECT_EQ(marker.position, glm::vec3(3.0f, 4.0f, 5.0f));
  EXPECT_EQ(marker.label, "Duct vs beam");
  EXPECT_GT(marker.color.r, marker.color.g);
}

TEST(BimCoordinationOverlayTests,
     ClashPinsWithoutPositionUseElementBoundsMidpoint) {
  const std::array elements{
      BimCoordinationOverlayElement{.objectIndex = 12u,
                                    .ifcClass = "IfcDuctSegment",
                                    .bounds = bounds({0.0f, 0.0f, 0.0f},
                                                     {2.0f, 2.0f, 2.0f})},
      BimCoordinationOverlayElement{.objectIndex = 44u,
                                    .ifcClass = "IfcBeam",
                                    .bounds = bounds({4.0f, 2.0f, 0.0f},
                                                     {6.0f, 4.0f, 2.0f})},
  };
  const std::array clashes{
      BimCoordinationOverlayClashPair{.primaryObjectIndex = 12u,
                                      .secondaryObjectIndex = 44u,
                                      .label = "Duct vs beam"},
  };

  const auto result = buildBimCoordinationOverlay(
      {.elements = elements, .clashPairs = clashes, .options = onlyClashes()});

  ASSERT_EQ(result.markers.size(), 1u);
  EXPECT_EQ(result.markers.front().kind, BimCoordinationOverlayKind::Clash);
  EXPECT_EQ(result.markers.front().position, glm::vec3(3.0f, 2.0f, 1.0f));
}

TEST(BimCoordinationOverlayTests,
     SkipsClashPinsWithoutFiniteExplicitOrBoundsDerivedPosition) {
  const std::array elements{
      BimCoordinationOverlayElement{.objectIndex = 12u,
                                    .ifcClass = "IfcDuctSegment"},
      BimCoordinationOverlayElement{.objectIndex = 44u,
                                    .ifcClass = "IfcBeam"},
  };
  const std::array clashes{
      BimCoordinationOverlayClashPair{
          .primaryObjectIndex = 12u,
          .secondaryObjectIndex = 44u,
          .hasPosition = true,
          .position = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f},
          .label = "Missing position"},
  };

  const auto result = buildBimCoordinationOverlay(
      {.elements = elements, .clashPairs = clashes, .options = onlyClashes()});

  EXPECT_TRUE(result.markers.empty());
}

TEST(BimCoordinationOverlayTests, SupportsManualBcfIssuePins) {
  const std::array issuePins{
      BimCoordinationOverlayIssuePin{.position = {9.0f, 2.0f, 1.0f},
                                     .color = {0.1f, 0.6f, 1.0f},
                                     .label = "BCF-17",
                                     .primaryObjectIndex = 88u},
  };

  const auto result = buildBimCoordinationOverlay(
      {.issuePins = issuePins, .options = onlyIssuePins()});

  ASSERT_EQ(result.markers.size(), 1u);
  const auto &marker = result.markers.front();
  EXPECT_EQ(marker.kind, BimCoordinationOverlayKind::IssuePin);
  EXPECT_EQ(marker.primaryObjectIndex, 88u);
  EXPECT_EQ(marker.secondaryObjectIndex,
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(marker.position, glm::vec3(9.0f, 2.0f, 1.0f));
  EXPECT_EQ(marker.color, glm::vec3(0.1f, 0.6f, 1.0f));
  EXPECT_EQ(marker.label, "BCF-17");
}

TEST(BimCoordinationOverlayTests,
     BuildsGpuRenderableMarkerWirePrimitivesFromIssueAndClashPins) {
  const std::array markers{
      container::renderer::BimCoordinationOverlayMarker{
          .kind = BimCoordinationOverlayKind::IssuePin,
          .position = {2.0f, 3.0f, 4.0f},
          .color = {0.1f, 0.6f, 1.0f},
          .label = "BCF-17",
          .primaryObjectIndex = 12u},
      container::renderer::BimCoordinationOverlayMarker{
          .kind = BimCoordinationOverlayKind::Clash,
          .position = {8.0f, 1.0f, -2.0f},
          .color = {1.0f, 0.15f, 0.05f},
          .label = "Duct vs beam",
          .primaryObjectIndex = 21u,
          .secondaryObjectIndex = 33u},
  };

  const auto primitives =
      buildBimCoordinationOverlayMarkerWirePrimitives(markers, 0.5f);

  ASSERT_EQ(primitives.size(), 2u);
  EXPECT_EQ(primitives[0].kind, BimCoordinationOverlayKind::IssuePin);
  EXPECT_EQ(primitives[0].primaryObjectIndex, 12u);
  EXPECT_EQ(primitives[0].positions.size(), 6u);
  EXPECT_EQ(primitives[0].indices.size(), 24u);
  EXPECT_EQ(primitives[0].positions[0], glm::vec3(2.0f, 3.5f, 4.0f));
  EXPECT_EQ(primitives[0].positions[1], glm::vec3(2.0f, 2.5f, 4.0f));
  EXPECT_EQ(primitives[1].kind, BimCoordinationOverlayKind::Clash);
  EXPECT_EQ(primitives[1].secondaryObjectIndex, 33u);
  EXPECT_EQ(primitives[1].color, glm::vec3(1.0f, 0.15f, 0.05f));
}

} // namespace
