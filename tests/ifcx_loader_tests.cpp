#include "Container/geometry/IfcxLoader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

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

TEST(IfcxLoader, AppliesInheritedMaterialPresentation) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "mat-gray",
      "attributes": {
        "bsi::ifc::presentation::diffuseColor": [0.35, 0.45, 0.55],
        "bsi::ifc::presentation::opacity": 0.6
      }
    },
    {
      "path": "mesh",
      "inherits": { "material": "mat-gray" },
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

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(model.elements[0].color.r, 0.35f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.45f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.55f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.6f, 1.0e-6f);
}

TEST(IfcxLoader, InstancesInheritedTypeGeometry) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "window-type",
      "children": { "Body": "shared-mesh" }
    },
    {
      "path": "shared-mesh",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        }
      }
    },
    {
      "path": "window-a",
      "inherits": { "windowType": "window-type" },
      "attributes": {
        "usd::xformop": {
          "transform": [
            [1, 0, 0, 0],
            [0, 1, 0, 0],
            [0, 0, 1, 0],
            [1, 0, 0, 1]
          ]
        }
      }
    },
    {
      "path": "window-b",
      "inherits": { "windowType": "window-type" },
      "attributes": {
        "usd::xformop": {
          "transform": [
            [1, 0, 0, 0],
            [0, 1, 0, 0],
            [0, 0, 1, 0],
            [3, 0, 0, 1]
          ]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 2u);
  std::vector<float> xTranslations{
      model.elements[0].transform[3].x,
      model.elements[1].transform[3].x,
  };
  std::ranges::sort(xTranslations);
  EXPECT_NEAR(xTranslations[0], 1.0f, 1.0e-6f);
  EXPECT_NEAR(xTranslations[1], 3.0f, 1.0e-6f);
}

TEST(IfcxLoader, InstancePresentationOverridesInheritedTypeDefaults) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "door-type",
      "attributes": {
        "bsi::ifc::presentation::diffuseColor": [1.0, 0.0, 0.0]
      },
      "children": { "Body": "shared-mesh" }
    },
    {
      "path": "shared-mesh",
      "attributes": {
        "usd::usdgeom::mesh": {
          "faceVertexIndices": [0, 1, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        }
      }
    },
    {
      "path": "door-instance",
      "inherits": { "doorType": "door-type" },
      "attributes": {
        "bsi::ifc::presentation::diffuseColor": [0.0, 0.0, 1.0]
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(model.elements[0].color.r, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 1.0f, 1.0e-6f);
}

TEST(IfcxLoader, MapsInheritedGltfMaterial) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "mat-pbr",
      "attributes": {
        "gltf::material": {
          "pbrMetallicRoughness": {
            "baseColorFactor": [0.1, 0.2, 0.3, 0.4],
            "metallicFactor": 0.7,
            "roughnessFactor": 0.25,
            "baseColorTexture": "textures/base.png"
          },
          "normalTexture": {
            "texture": "textures/normal.png",
            "scale": 0.5
          },
          "occlusionTexture": {
            "texture": "textures/ao.png",
            "strength": 0.6
          },
          "emissiveTexture": "textures/emissive.png",
          "emissiveFactor": [0.8, 0.7, 0.6],
          "alphaMode": "BLEND",
          "alphaCutoff": 0.35,
          "doubleSided": true
        }
      }
    },
    {
      "path": "mesh",
      "inherits": { "material": "mat-pbr" },
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

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_EQ(model.materials.size(), 1u);
  EXPECT_EQ(model.elements[0].materialIndex, 0u);
  EXPECT_NEAR(model.elements[0].color.r, 0.1f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.2f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.3f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.4f, 1.0e-6f);
  EXPECT_TRUE(model.elements[0].doubleSided);
  const auto &material = model.materials[0].pbr;
  EXPECT_NEAR(material.metallicFactor, 0.7f, 1.0e-6f);
  EXPECT_NEAR(material.roughnessFactor, 0.25f, 1.0e-6f);
  EXPECT_NEAR(material.normalTextureScale, 0.5f, 1.0e-6f);
  EXPECT_NEAR(material.occlusionStrength, 0.6f, 1.0e-6f);
  EXPECT_NEAR(material.emissiveColor.r, 0.8f, 1.0e-6f);
  EXPECT_NEAR(material.alphaCutoff, 0.35f, 1.0e-6f);
  EXPECT_EQ(material.alphaMode, container::material::AlphaMode::Blend);
  EXPECT_FALSE(model.materials[0].texturePaths.baseColor.empty());
  EXPECT_FALSE(model.materials[0].texturePaths.normal.empty());
  EXPECT_FALSE(model.materials[0].texturePaths.occlusion.empty());
  EXPECT_FALSE(model.materials[0].texturePaths.emissive.empty());
}

TEST(IfcxLoader, LoadsBasisCurvesAndPointArrays) {
  constexpr const char *kIfcJson = R"json(
{
  "data": [
    {
      "path": "root",
      "children": {
        "Axis": "curve",
        "Survey": "points"
      }
    },
    {
      "path": "curve",
      "attributes": {
        "usd::usdgeom::basiscurves": {
          "curveVertexCounts": [2, 2],
          "points": [[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 1, 1]]
        }
      }
    },
    {
      "path": "points",
      "attributes": {
        "points::array": {
          "positions": [[0, 0, 0], [1, 1, 1]],
          "colors": [[1, 0, 0], [0, 1, 0]]
        }
      }
    }
  ]
}
)json";

  const auto model = container::geometry::ifcx::LoadFromJson(kIfcJson);

  EXPECT_EQ(model.meshRanges.size(), 2u);
  EXPECT_EQ(model.elements.size(), 2u);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
}

TEST(IfcxLoader, ComposesLocalIfcImports) {
  const std::filesystem::path tempDir =
      std::filesystem::temp_directory_path() / "container_ifcx_import_test";
  std::filesystem::remove_all(tempDir);
  std::filesystem::create_directories(tempDir);

  const std::filesystem::path base = tempDir / "base.ifcx";
  const std::filesystem::path overlay = tempDir / "overlay.ifcx";
  {
    std::ofstream file(base, std::ios::binary);
    file << R"json(
{
  "data": [
    {
      "path": "product",
      "children": { "Body": "mesh" }
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
  }
  {
    std::ofstream file(overlay, std::ios::binary);
    file << R"json(
{
  "imports": [
    { "uri": "base.ifcx" }
  ],
  "data": [
    {
      "path": "product",
      "attributes": {
        "bsi::ifc::presentation::diffuseColor": [0.9, 0.1, 0.2]
      }
    }
  ]
}
)json";
  }

  const auto model = container::geometry::ifcx::LoadFromFile(overlay);

  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_NEAR(model.elements[0].color.r, 0.9f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.1f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.2f, 1.0e-6f);

  std::filesystem::remove_all(tempDir);
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

TEST(IfcxLoader, LoadsDownloadedBuildingSmartIfc5PbrLayer) {
  const std::filesystem::path sample =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "models" /
      "buildingSMART-IFC5-development" / "examples" / "Domestic Hot Water" /
      "domestic-hot-water-add-pbr.ifcx";
  if (!std::filesystem::exists(sample)) {
    GTEST_SKIP() << "buildingSMART IFC5-development files are not available";
  }

  const auto model = container::geometry::ifcx::LoadFromFile(sample);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_FALSE(model.meshRanges.empty());
  EXPECT_FALSE(model.elements.empty());
  EXPECT_GE(model.materials.size(), 1u);
}

TEST(IfcxLoader, LoadsDownloadedBuildingSmartIfc5PointCloudSample) {
  const std::filesystem::path sample =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "models" /
      "buildingSMART-IFC5-development" / "examples" / "Point Cloud" /
      "point-cloud.ifcx";
  if (!std::filesystem::exists(sample)) {
    GTEST_SKIP() << "buildingSMART IFC5-development files are not available";
  }

  const auto model = container::geometry::ifcx::LoadFromFile(sample);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_FALSE(model.meshRanges.empty());
  EXPECT_FALSE(model.elements.empty());
  EXPECT_GE(model.elements.size(), 5u);
}

} // namespace
