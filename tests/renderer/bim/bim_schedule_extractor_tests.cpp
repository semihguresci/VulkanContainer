#include "Container/renderer/bim/BimScheduleExtractor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

namespace {

using container::renderer::BimScheduleBounds;
using container::renderer::BimScheduleElement;
using container::renderer::BimScheduleRow;
using container::renderer::buildBimScheduleByIfcClassAndStorey;
using container::renderer::buildBimScheduleByTypeAndStorey;
using container::renderer::buildBimScheduleMaterialTotals;

[[nodiscard]] BimScheduleBounds bounds(glm::vec3 min, glm::vec3 max) {
  return {.valid = true, .min = min, .max = max};
}

[[nodiscard]] const BimScheduleRow* findRow(
    const std::vector<BimScheduleRow>& rows, std::string_view key,
    std::string_view storey) {
  const auto found = std::ranges::find_if(
      rows, [key, storey](const BimScheduleRow& row) {
        return row.key == key && row.storey == storey;
      });
  return found == rows.end() ? nullptr : &*found;
}

TEST(BimScheduleExtractorTests, GroupsCountsByIfcClassAndStorey) {
  const std::array elements{
      BimScheduleElement{.ifcClass = "IfcWall", .storey = "Level 01"},
      BimScheduleElement{.ifcClass = "IfcWall", .storey = "Level 01"},
      BimScheduleElement{.ifcClass = "IfcWall", .storey = "Level 02"},
      BimScheduleElement{.ifcClass = "IfcDoor", .storey = "Level 01"},
  };

  const auto rows = buildBimScheduleByIfcClassAndStorey(elements);

  ASSERT_EQ(rows.size(), 3u);
  const BimScheduleRow* wallLevel1 = findRow(rows, "IfcWall", "Level 01");
  ASSERT_NE(wallLevel1, nullptr);
  EXPECT_EQ(wallLevel1->count, 2u);
  const BimScheduleRow* wallLevel2 = findRow(rows, "IfcWall", "Level 02");
  ASSERT_NE(wallLevel2, nullptr);
  EXPECT_EQ(wallLevel2->count, 1u);
  const BimScheduleRow* doorLevel1 = findRow(rows, "IfcDoor", "Level 01");
  ASSERT_NE(doorLevel1, nullptr);
  EXPECT_EQ(doorLevel1->count, 1u);
}

TEST(BimScheduleExtractorTests, GroupsCountsByTypeAndStorey) {
  const std::array elements{
      BimScheduleElement{.ifcClass = "IfcWall",
                         .type = "Basic Wall",
                         .storey = "Level 01"},
      BimScheduleElement{.ifcClass = "IfcWall",
                         .type = "Basic Wall",
                         .storey = "Level 01"},
      BimScheduleElement{.ifcClass = "IfcWall",
                         .type = "Curtain Wall",
                         .storey = "Level 01"},
      BimScheduleElement{.ifcClass = "IfcSlab",
                         .type = "Floor Slab",
                         .storey = "Level 02"},
  };

  const auto rows = buildBimScheduleByTypeAndStorey(elements);

  ASSERT_EQ(rows.size(), 3u);
  const BimScheduleRow* basicWall = findRow(rows, "Basic Wall", "Level 01");
  ASSERT_NE(basicWall, nullptr);
  EXPECT_EQ(basicWall->count, 2u);
  const BimScheduleRow* curtainWall =
      findRow(rows, "Curtain Wall", "Level 01");
  ASSERT_NE(curtainWall, nullptr);
  EXPECT_EQ(curtainWall->count, 1u);
}

TEST(BimScheduleExtractorTests, BuildsMaterialTotalsByMaterialName) {
  const std::array elements{
      BimScheduleElement{.ifcClass = "IfcWall",
                         .material = "Concrete",
                         .bounds = bounds({0.0f, 0.0f, 0.0f},
                                          {1.0f, 2.0f, 3.0f})},
      BimScheduleElement{.ifcClass = "IfcSlab",
                         .material = "Concrete",
                         .bounds = bounds({0.0f, 0.0f, 0.0f},
                                          {2.0f, 2.0f, 1.0f})},
      BimScheduleElement{.ifcClass = "IfcWindow", .material = "Glass"},
  };

  const auto rows = buildBimScheduleMaterialTotals(elements);

  ASSERT_EQ(rows.size(), 2u);
  const BimScheduleRow* concrete = findRow(rows, "Concrete", "");
  ASSERT_NE(concrete, nullptr);
  EXPECT_EQ(concrete->material, "Concrete");
  EXPECT_EQ(concrete->count, 2u);
  EXPECT_FLOAT_EQ(concrete->estimatedVolume, 10.0f);
  const BimScheduleRow* glass = findRow(rows, "Glass", "");
  ASSERT_NE(glass, nullptr);
  EXPECT_EQ(glass->count, 1u);
}

TEST(BimScheduleExtractorTests, EstimatesAreaAndVolumeFromBoundsBoxes) {
  const std::array elements{
      BimScheduleElement{.ifcClass = "IfcSpace",
                         .storey = "Level 01",
                         .bounds = bounds({0.0f, 0.0f, 0.0f},
                                          {2.0f, 3.0f, 4.0f})},
  };

  const auto rows = buildBimScheduleByIfcClassAndStorey(elements);

  ASSERT_EQ(rows.size(), 1u);
  EXPECT_FLOAT_EQ(rows.front().estimatedArea, 52.0f);
  EXPECT_FLOAT_EQ(rows.front().estimatedVolume, 24.0f);
}

} // namespace
