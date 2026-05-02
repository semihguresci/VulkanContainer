#include "Container/geometry/IfcxLoader.h"

#include <gtest/gtest.h>

#include <filesystem>

#include <glm/geometric.hpp>

#ifndef CONTAINER_BINARY_DIR
#define CONTAINER_BINARY_DIR "."
#endif

namespace {

TEST(IfcxLoader, ParsesUsdMeshWithInheritedMetadataAndTransform) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "product",
      "children": { "Body": "mesh" },
      "attributes": {
        "usd::xformop": {
          "transform": [
            [1, 0, 0, 0],
            [0, 1, 0, 0],
            [0, 0, 1, 0],
            [2, 3, 4, 1]
          ]
        },
        "bsi::ifc::class": { "code": "IfcWall" },
        "bsi::ifc::presentation::diffuseColor": [0.2, 0.4, 0.6],
        "bsi::ifc::presentation::opacity": 0.5
      }
    },
    {
      "path": "mesh",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson, 2.0f);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.vertices.size(), 3u);
  EXPECT_EQ(model.indices.size(), 3u);
  EXPECT_NEAR(model.elements[0].transform[3].x, 4.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].y, 8.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].z, -6.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[0].x, 2.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[1].z, -2.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[2].y, 2.0f, 1.0e-6f);
  EXPECT_EQ(model.elements[0].type, "IfcWall");
  EXPECT_NEAR(model.elements[0].color.r, 0.2f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.4f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.6f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.5f, 1.0e-6f);
  EXPECT_NEAR(glm::length(model.vertices[0].normal), 1.0f, 1.0e-6f);
}

TEST(IfcxLoader, TriangulatesPolygonCountsAndSkipsInvisibleMeshes) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "quad",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexCounts": [4],
          "faceVertexIndices": [0, 1, 2, 3],
          "points": [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]]
        }
      }
    },
    {
      "path": "hidden",
      "attributes": {
        "usd::usdgeom::visibility": { "visibility": "invisible" },
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [0, 1, 0], [0, 0, 1]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.indices.size(), 6u);
  EXPECT_EQ(model.vertices.size(), 6u);
}

TEST(IfcxLoader, LoadsDownloadedBuildingSmartIfc5Sample) {
  const std::filesystem::path sample =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "models" /
      "buildingSMART-IFC5-development" / "examples" / "Hello Wall" /
      "hello-wall.ifcx";
  if (!std::filesystem::exists(sample)) {
    GTEST_SKIP() << "buildingSMART IFC5-development files are not available";
  }

  const auto model = container::geometry::ifcx::LoadFromFile(sample);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_FALSE(model.meshRanges.empty());
  EXPECT_FALSE(model.elements.empty());
}

} // namespace
