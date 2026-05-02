#include "Container/geometry/DotBimLoader.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

TEST(DotBimLoader, ParsesMeshElementTransformAndColor) {
  constexpr const char *kDotBimJson = R"json(
{
  "schema_version": "1.1.0",
  "meshes": [
    {
      "mesh_id": 7,
      "coordinates": [0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 3.0, 0.0],
      "indices": [0, 1, 2]
    }
  ],
  "elements": [
    {
      "mesh_id": 7,
      "guid": "element-1",
      "type": "Wall",
      "vector": {"x": 4.0, "y": 5.0, "z": 6.0},
      "rotation": {"qx": 0.0, "qy": 0.0, "qz": 0.0, "qw": 1.0},
      "color": {"r": 10, "g": 20, "b": 30, "a": 255}
    }
  ],
  "info": {}
}
)json";

  const auto model =
      container::geometry::dotbim::LoadFromJson(kDotBimJson, 2.0f);

  ASSERT_EQ(model.vertices.size(), 3u);
  ASSERT_EQ(model.indices.size(), 3u);
  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);

  EXPECT_EQ(model.meshRanges[0].meshId, 7u);
  EXPECT_EQ(model.meshRanges[0].firstIndex, 0u);
  EXPECT_EQ(model.meshRanges[0].indexCount, 3u);
  EXPECT_NEAR(model.meshRanges[0].boundsCenter.x, 1.0f, 1.0e-5f);
  EXPECT_NEAR(model.meshRanges[0].boundsCenter.y, 1.5f, 1.0e-5f);

  const auto &element = model.elements[0];
  EXPECT_EQ(element.meshId, 7u);
  EXPECT_EQ(element.guid, "element-1");
  EXPECT_EQ(element.type, "Wall");
  EXPECT_NEAR(element.transform[0].x, 2.0f, 1.0e-5f);
  EXPECT_NEAR(element.transform[1].z, -2.0f, 1.0e-5f);
  EXPECT_NEAR(element.transform[2].y, 2.0f, 1.0e-5f);
  EXPECT_NEAR(element.transform[3].x, 8.0f, 1.0e-5f);
  EXPECT_NEAR(element.transform[3].y, 12.0f, 1.0e-5f);
  EXPECT_NEAR(element.transform[3].z, -10.0f, 1.0e-5f);
  EXPECT_NEAR(element.color.r, 10.0f / 255.0f, 1.0e-5f);
  EXPECT_NEAR(element.color.g, 20.0f / 255.0f, 1.0e-5f);
  EXPECT_NEAR(element.color.b, 30.0f / 255.0f, 1.0e-5f);
  EXPECT_NEAR(element.color.a, 1.0f, 1.0e-5f);

  EXPECT_NEAR(model.vertices[0].normal.z, 1.0f, 1.0e-5f);
}

TEST(DotBimLoader, RejectsElementWithMissingMeshId) {
  constexpr const char *kDotBimJson = R"json(
{
  "schema_version": "1.1.0",
  "meshes": [
    {"mesh_id": 0, "coordinates": [0, 0, 0, 1, 0, 0, 0, 1, 0], "indices": [0, 1, 2]}
  ],
  "elements": [
    {"vector": {"x": 0, "y": 0, "z": 0}}
  ],
  "info": {}
}
)json";

  EXPECT_THROW((void)container::geometry::dotbim::LoadFromJson(kDotBimJson),
               std::runtime_error);
}

} // namespace
