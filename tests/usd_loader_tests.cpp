#include "Container/geometry/UsdLoader.h"
#include "Container/utility/SceneData.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>

#ifndef CONTAINER_BINARY_DIR
#define CONTAINER_BINARY_DIR "."
#endif

namespace {

constexpr const char *kTriangleUsd = R"usd(
#usda 1.0

def Xform "Root"
{
    matrix4d xformOp:transform = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (2, 3, 4, 1))
    uniform token[] xformOpOrder = ["xformOp:transform"]
    color3f[] primvars:displayColor = [(0.25, 0.5, 0.75)]
    float[] primvars:displayOpacity = [0.5]

    def Mesh "Quad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    }
}
)usd";

constexpr const char *kZUpTriangleUsd = R"usd(
#usda 1.0
(
    upAxis = "Z"
)

def Xform "Root"
{
    matrix4d xformOp:transform = ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (2, 3, 4, 1))
    uniform token[] xformOpOrder = ["xformOp:transform"]

    def Mesh "Tri"
    {
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 0, 1)]
    }
}
)usd";

constexpr const char *kTinyUsdDisplayOpacityUsd = R"usd(
#usda 1.0

def Mesh "Tri"
{
    color3f[] primvars:displayColor = [(0.1, 0.2, 0.3)]
    float[] primvars:displayOpacity = [0.35]
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}
)usd";

void appendLe16(std::vector<uint8_t> &bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void appendLe32(std::vector<uint8_t> &bytes, uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

std::filesystem::path writeStoredUsdz(std::string_view name,
                                      std::string_view layerText) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_usd_loader_tests";
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / (std::string(name) + ".usdz");

  constexpr std::string_view kEntryName = "root.usda";
  std::vector<uint8_t> bytes;
  const uint32_t localHeaderOffset = 0u;
  appendLe32(bytes, 0x04034b50u);
  appendLe16(bytes, 20u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe32(bytes, 0u);
  appendLe32(bytes, static_cast<uint32_t>(layerText.size()));
  appendLe32(bytes, static_cast<uint32_t>(layerText.size()));
  appendLe16(bytes, static_cast<uint16_t>(kEntryName.size()));
  appendLe16(bytes, 0u);
  bytes.insert(bytes.end(), kEntryName.begin(), kEntryName.end());
  bytes.insert(bytes.end(), layerText.begin(), layerText.end());

  const uint32_t centralDirectoryOffset = static_cast<uint32_t>(bytes.size());
  appendLe32(bytes, 0x02014b50u);
  appendLe16(bytes, 20u);
  appendLe16(bytes, 20u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe32(bytes, 0u);
  appendLe32(bytes, static_cast<uint32_t>(layerText.size()));
  appendLe32(bytes, static_cast<uint32_t>(layerText.size()));
  appendLe16(bytes, static_cast<uint16_t>(kEntryName.size()));
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe32(bytes, 0u);
  appendLe32(bytes, localHeaderOffset);
  bytes.insert(bytes.end(), kEntryName.begin(), kEntryName.end());

  const uint32_t centralDirectorySize =
      static_cast<uint32_t>(bytes.size()) - centralDirectoryOffset;
  appendLe32(bytes, 0x06054b50u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 0u);
  appendLe16(bytes, 1u);
  appendLe16(bytes, 1u);
  appendLe32(bytes, centralDirectorySize);
  appendLe32(bytes, centralDirectoryOffset);
  appendLe16(bytes, 0u);

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return path;
}

std::filesystem::path writeUsdText(std::string_view name,
                                   std::string_view layerText) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_usd_loader_tests";
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / (std::string(name) + ".usda");
  std::ofstream out(path, std::ios::binary);
  out.write(layerText.data(), static_cast<std::streamsize>(layerText.size()));
  return path;
}

std::filesystem::path openUsdSampleRoot() {
  return std::filesystem::path(CONTAINER_BINARY_DIR) / "models" /
         "OpenUSD-Sample-Assets";
}

void expectRenderableUsdSample(const std::filesystem::path &path) {
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "OpenUSD sample is not available: " << path.string();
  }

  const auto model = container::geometry::usd::LoadFromFile(path);
  EXPECT_FALSE(model.vertices.empty());
  EXPECT_FALSE(model.indices.empty());
  EXPECT_FALSE(model.meshRanges.empty());
  EXPECT_FALSE(model.elements.empty());
}

} // namespace

TEST(UsdLoader, ParsesUsdMeshTransformColorAndPolygonTriangulation) {
  const auto model = container::geometry::usd::LoadFromText(kTriangleUsd, 2.0f);

  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.indices.size(), 6u);
  EXPECT_EQ(model.vertices.size(), 6u);
  EXPECT_NEAR(model.elements[0].transform[3].x, 4.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].y, 6.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].transform[3].z, 8.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.r, 0.25f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.5f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.75f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.5f, 1.0e-6f);
  EXPECT_NEAR(glm::length(model.vertices[0].normal), 1.0f, 1.0e-6f);
}

TEST(UsdLoader, ConvertsZUpStageToRendererAxes) {
  const auto model =
      container::geometry::usd::LoadFromText(kZUpTriangleUsd, 2.0f);

  ASSERT_EQ(model.elements.size(), 1u);
  const glm::mat4 &transform = model.elements[0].transform;
  EXPECT_NEAR(transform[0].x, 2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[1].z, -2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[2].y, 2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].x, 4.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].y, 8.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].z, -6.0f, 1.0e-6f);
}

TEST(UsdLoader, SkipsInvisibleMeshes) {
  constexpr const char *kUsd = R"usd(
#usda 1.0
def Xform "Hidden"
{
    token visibility = "invisible"
    def Mesh "Tri"
    {
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
}
)usd";

  const auto model = container::geometry::usd::LoadFromText(kUsd);
  EXPECT_TRUE(model.vertices.empty());
  EXPECT_TRUE(model.indices.empty());
  EXPECT_TRUE(model.elements.empty());
}

TEST(UsdLoader, ParsesDoubleSidedFlag) {
  constexpr const char *kUsd = R"usd(
#usda 1.0
def Mesh "Tri"
{
    uniform bool doubleSided = false
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
}
)usd";

  const auto model = container::geometry::usd::LoadFromText(kUsd);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_FALSE(model.elements[0].doubleSided);
}

TEST(UsdLoader, LoadsStoredUsdzRootLayer) {
  const std::filesystem::path path =
      writeStoredUsdz("stored_triangle", kTriangleUsd);

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.indices.size(), 6u);
}

TEST(UsdLoader, ParsesAuthoredVertexAttributes) {
  constexpr const char *kUsd = R"usd(
#usda 1.0
def Mesh "Tri"
{
    int[] faceVertexIndices = [0, 1, 2]
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1)]
    texCoord2f[] primvars:st = [(0.125, 0.25), (0.875, 0.25), (0.125, 0.75)]
    texCoord2f[] primvars:st1 = [(0.2, 0.3), (0.4, 0.5), (0.6, 0.7)]
    color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1)]
}
)usd";

  const auto model = container::geometry::usd::LoadFromText(kUsd);

  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_GE(model.vertices.size(), 3u);
  EXPECT_NEAR(model.vertices[0].normal.x, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].normal.y, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].normal.z, 1.0f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].texCoord.x, 0.125f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].texCoord.y, 0.25f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].texCoord1.x, 0.2f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].texCoord1.y, 0.3f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].color.r, 1.0f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].color.g, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.vertices[0].color.b, 0.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.r, 1.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 1.0f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 1.0f, 1.0e-6f);
}

TEST(UsdLoader, LoadsDisplayOpacityWhenTinyUsdIsAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      writeUsdText("display_opacity", kTinyUsdDisplayOpacityUsd);

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_NEAR(model.elements[0].color.r, 0.1f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.g, 0.2f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.b, 0.3f, 1.0e-6f);
  EXPECT_NEAR(model.elements[0].color.a, 0.35f, 1.0e-6f);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, TinyUsdConvertsZUpStageToRendererAxesWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      writeUsdText("z_up_stage_axes", kZUpTriangleUsd);

  const auto model = container::geometry::usd::LoadFromFile(path, 2.0f);
  ASSERT_EQ(model.elements.size(), 1u);
  const glm::mat4 &transform = model.elements[0].transform;
  EXPECT_NEAR(transform[0].x, 2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[1].z, -2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[2].y, 2.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].x, 4.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].y, 8.0f, 1.0e-6f);
  EXPECT_NEAR(transform[3].z, -6.0f, 1.0e-6f);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, LoadsBoundPreviewSurfaceMaterialsWhenTinyUsdIsAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "_deps" / "tinyusdz-src" /
      "models" / "cube-previewsurface.usda";
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "TinyUSDZ PreviewSurface sample is not available";
  }

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_EQ(model.elements.size(), 1u);
  ASSERT_FALSE(model.materials.empty());
  ASSERT_NE(model.elements[0].materialIndex,
            std::numeric_limits<uint32_t>::max());
  ASSERT_LT(model.elements[0].materialIndex, model.materials.size());
  const auto &material = model.materials[model.elements[0].materialIndex].pbr;
  EXPECT_NEAR(material.baseColor.r, 0.12825416f, 1.0e-6f);
  EXPECT_NEAR(material.baseColor.g, 0.8000001f, 1.0e-6f);
  EXPECT_NEAR(material.baseColor.b, 0.21895278f, 1.0e-6f);
  EXPECT_NEAR(material.metallicFactor, 0.0f, 1.0e-6f);
  EXPECT_NEAR(material.roughnessFactor, 0.4f, 1.0e-6f);
  EXPECT_NEAR(material.opacityFactor, 1.0f, 1.0e-6f);
  EXPECT_EQ(material.alphaMode, container::material::AlphaMode::Opaque);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, LoadsTinyUsdTextureAssetsWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  const std::filesystem::path path =
      std::filesystem::path(CONTAINER_BINARY_DIR) / "_deps" / "tinyusdz-src" /
      "models" / "texture-cat-plane.usdz";
  if (!std::filesystem::exists(path)) {
    GTEST_SKIP() << "TinyUSDZ textured USDZ sample is not available";
  }

  const auto model = container::geometry::usd::LoadFromFile(path);
  ASSERT_FALSE(model.materials.empty());

  bool foundBaseColorTexture = false;
  for (const auto &material : model.materials) {
    const auto &texture = material.texturePaths.baseColor;
    if (texture.empty()) {
      continue;
    }

    foundBaseColorTexture = true;
    EXPECT_FALSE(texture.name.empty());
    EXPECT_LE(texture.samplerIndex,
              container::gpu::kMaterialSamplerDescriptorCapacity - 1u);
    EXPECT_FALSE(texture.encodedBytes.empty());
  }
  EXPECT_TRUE(foundBaseColorTexture);
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, RejectsBinaryUsdcPayloads) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_usd_loader_tests";
  std::filesystem::create_directories(dir);
  const std::filesystem::path path = dir / "binary.usd";
  {
    std::ofstream out(path, std::ios::binary);
    out.write("PXR-USDC", 8);
  }

  EXPECT_THROW((void)container::geometry::usd::LoadFromFile(path),
               std::runtime_error);
}

TEST(UsdLoader, LoadsOpenUsdKitchenSetSampleWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  expectRenderableUsdSample(openUsdSampleRoot() / "Kitchen_set" /
                            "Kitchen_set.usd");
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, LoadsOpenUsdKitchenSetInstancedSampleWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  expectRenderableUsdSample(openUsdSampleRoot() / "Kitchen_set" /
                            "Kitchen_set_instanced.usd");
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}

TEST(UsdLoader, LoadsOpenUsdPointInstancedMedCitySampleWhenAvailable) {
#if defined(CONTAINER_HAS_TINYUSDZ)
  expectRenderableUsdSample(openUsdSampleRoot() / "PointInstancedMedCity" /
                            "PointInstancedMedCity.usd");
#else
  GTEST_SKIP() << "TinyUSDZ importer is disabled";
#endif
}
