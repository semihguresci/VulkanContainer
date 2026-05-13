#include "Container/renderer/bim/BimModelCompare.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

namespace {

using container::renderer::BimModelCompareBounds;
using container::renderer::BimModelCompareChangeKind;
using container::renderer::BimModelCompareElement;
using container::renderer::compareBimModels;

[[nodiscard]] BimModelCompareBounds bounds(glm::vec3 min, glm::vec3 max) {
  return {.valid = true, .min = min, .max = max};
}

[[nodiscard]] bool hasChange(
    const std::vector<container::renderer::BimModelCompareChange>& changes,
    BimModelCompareChangeKind kind, std::string_view identity) {
  return std::ranges::any_of(changes, [kind, identity](const auto& change) {
    return change.kind == kind && change.identity == identity;
  });
}

TEST(BimModelCompareTests, DetectsAddedAndRemovedByGuidOrSourceId) {
  const std::array before{
      BimModelCompareElement{.guid = "guid-wall",
                             .type = "IfcWall",
                             .storey = "Level 01"},
      BimModelCompareElement{.sourceId = "#legacy-door",
                             .type = "IfcDoor",
                             .storey = "Level 01"},
  };
  const std::array after{
      BimModelCompareElement{.guid = "guid-wall",
                             .type = "IfcWall",
                             .storey = "Level 01"},
      BimModelCompareElement{.sourceId = "#new-window",
                             .type = "IfcWindow",
                             .storey = "Level 02"},
  };

  const auto result = compareBimModels(before, after);

  ASSERT_EQ(result.changes.size(), 2u);
  EXPECT_TRUE(hasChange(result.changes, BimModelCompareChangeKind::Removed,
                        "#legacy-door"));
  EXPECT_TRUE(hasChange(result.changes, BimModelCompareChangeKind::Added,
                        "#new-window"));
}

TEST(BimModelCompareTests, DetectsChangedTypeStoreyMaterialAndBounds) {
  const std::array before{
      BimModelCompareElement{.guid = "guid-wall",
                             .type = "IfcWall",
                             .storey = "Level 01",
                             .material = "Concrete",
                             .bounds = bounds({0.0f, 0.0f, 0.0f},
                                              {1.0f, 1.0f, 1.0f})},
  };
  const std::array after{
      BimModelCompareElement{.guid = "guid-wall",
                             .type = "IfcDoor",
                             .storey = "Level 02",
                             .material = "Timber",
                             .bounds = bounds({0.0f, 0.0f, 0.0f},
                                              {2.0f, 1.0f, 1.0f})},
  };

  const auto result = compareBimModels(before, after);

  ASSERT_EQ(result.changes.size(), 4u);
  EXPECT_TRUE(hasChange(result.changes, BimModelCompareChangeKind::ChangedType,
                        "guid-wall"));
  EXPECT_TRUE(hasChange(result.changes, BimModelCompareChangeKind::ChangedStorey,
                        "guid-wall"));
  EXPECT_TRUE(hasChange(result.changes,
                        BimModelCompareChangeKind::ChangedMaterial,
                        "guid-wall"));
  EXPECT_TRUE(hasChange(result.changes, BimModelCompareChangeKind::ChangedBounds,
                        "guid-wall"));
}

TEST(BimModelCompareTests, IgnoresBoundsWithinTolerance) {
  const std::array before{
      BimModelCompareElement{.guid = "guid-slab",
                             .type = "IfcSlab",
                             .bounds = bounds({0.0f, 0.0f, 0.0f},
                                              {1.0f, 1.0f, 1.0f})},
  };
  const std::array after{
      BimModelCompareElement{.guid = "guid-slab",
                             .type = "IfcSlab",
                             .bounds = bounds({0.0001f, 0.0f, 0.0f},
                                              {1.0001f, 1.0f, 1.0f})},
  };

  const auto result = compareBimModels(
      before, after,
      container::renderer::BimModelCompareOptions{.boundsTolerance = 0.001f});

  EXPECT_TRUE(result.changes.empty());
}

TEST(BimModelCompareTests, MatchesChangedGuidByStableSourceId) {
  const std::array before{
      BimModelCompareElement{.guid = "old-guid",
                             .sourceId = "#stable-door",
                             .type = "IfcDoor",
                             .storey = "Level 01",
                             .material = "Timber"},
  };
  const std::array after{
      BimModelCompareElement{.guid = "new-guid",
                             .sourceId = "#stable-door",
                             .type = "IfcDoor",
                             .storey = "Level 02",
                             .material = "Timber"},
  };

  const auto result = compareBimModels(before, after);

  ASSERT_EQ(result.changes.size(), 1u);
  EXPECT_TRUE(hasChange(result.changes, BimModelCompareChangeKind::ChangedStorey,
                        "#stable-door"));
}

TEST(BimModelCompareTests, DuplicateIdentitiesAreMatchedByOccurrence) {
  const std::array before{
      BimModelCompareElement{.guid = "dup-guid",
                             .type = "IfcWall",
                             .storey = "Level 01"},
      BimModelCompareElement{.guid = "dup-guid",
                             .type = "IfcDoor",
                             .storey = "Level 01"},
  };
  const std::array after{
      BimModelCompareElement{.guid = "dup-guid",
                             .type = "IfcWall",
                             .storey = "Level 01"},
      BimModelCompareElement{.guid = "dup-guid",
                             .type = "IfcWindow",
                             .storey = "Level 01"},
  };

  const auto result = compareBimModels(before, after);

  ASSERT_EQ(result.changes.size(), 1u);
  EXPECT_EQ(result.changes.front().kind,
            BimModelCompareChangeKind::ChangedType);
  EXPECT_EQ(result.changes.front().identity, "dup-guid#2");
  EXPECT_EQ(result.changes.front().beforeIndex, 1u);
  EXPECT_EQ(result.changes.front().afterIndex, 1u);
}

TEST(BimModelCompareTests, DuplicateIdentityCountDifferencesAreReported) {
  const std::array before{
      BimModelCompareElement{.sourceId = "#dup-source", .type = "IfcWall"},
      BimModelCompareElement{.sourceId = "#dup-source", .type = "IfcDoor"},
  };
  const std::array after{
      BimModelCompareElement{.sourceId = "#dup-source", .type = "IfcWall"},
  };

  const auto result = compareBimModels(before, after);

  ASSERT_EQ(result.changes.size(), 1u);
  EXPECT_EQ(result.changes.front().kind, BimModelCompareChangeKind::Removed);
  EXPECT_EQ(result.changes.front().identity, "#dup-source#2");
  EXPECT_EQ(result.changes.front().beforeIndex, 1u);
}

} // namespace
