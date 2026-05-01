#include "Container/geometry/Model.h"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

void appendFloat(std::vector<char>& bytes, float value) {
  const auto* raw = reinterpret_cast<const char*>(&value);
  bytes.insert(bytes.end(), raw, raw + sizeof(float));
}

void appendUint16(std::vector<char>& bytes, uint16_t value) {
  const auto* raw = reinterpret_cast<const char*>(&value);
  bytes.insert(bytes.end(), raw, raw + sizeof(uint16_t));
}

std::filesystem::path writeTriangleGltf(
    std::string_view name,
    const std::array<uint16_t, 3>& triangleIndices) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_gltf_loader_tests";
  std::filesystem::create_directories(dir);

  const std::filesystem::path gltfPath = dir / (std::string(name) + ".gltf");
  const std::filesystem::path binPath = dir / (std::string(name) + ".bin");

  std::vector<char> bytes;
  bytes.reserve(78);

  for (const float value : {
           0.0f, 0.0f, 0.0f,
           0.0f, 1.0f, 0.0f,
           1.0f, 0.0f, 0.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const float value : {
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const uint16_t index : triangleIndices) {
    appendUint16(bytes, index);
  }

  {
    std::ofstream bin(binPath, std::ios::binary);
    bin.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const std::string binName = binPath.filename().generic_string();
  std::ofstream gltf(gltfPath);
  gltf
      << "{\n"
      << "  \"asset\": {\"version\": \"2.0\"},\n"
      << "  \"buffers\": [{\"uri\": \"" << binName
      << "\", \"byteLength\": " << bytes.size() << "}],\n"
      << "  \"bufferViews\": [\n"
      << "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 36},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 72, \"byteLength\": 6}\n"
      << "  ],\n"
      << "  \"accessors\": [\n"
      << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": [1, 1, 0]},\n"
      << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC3\"},\n"
      << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": 3, "
         "\"type\": \"SCALAR\"}\n"
      << "  ],\n"
      << "  \"materials\": [{}],\n"
      << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
         "\"NORMAL\": 1}, \"indices\": 2, \"mode\": 4, \"material\": 0}]}],\n"
      << "  \"nodes\": [{\"mesh\": 0}],\n"
      << "  \"scenes\": [{\"nodes\": [0]}],\n"
      << "  \"scene\": 0\n"
      << "}\n";

  return gltfPath;
}

std::filesystem::path writeMixedWindingQuadGltf(std::string_view name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_gltf_loader_tests";
  std::filesystem::create_directories(dir);

  const std::filesystem::path gltfPath = dir / (std::string(name) + ".gltf");
  const std::filesystem::path binPath = dir / (std::string(name) + ".bin");

  std::vector<char> bytes;
  bytes.reserve(108);

  for (const float value : {
           0.0f, 0.0f, 0.0f,
           0.0f, 1.0f, 0.0f,
           1.0f, 0.0f, 0.0f,
           1.0f, 1.0f, 0.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const float value : {
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const uint16_t index : {0, 1, 2, 0, 2, 3}) {
    appendUint16(bytes, index);
  }

  {
    std::ofstream bin(binPath, std::ios::binary);
    bin.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const std::string binName = binPath.filename().generic_string();
  std::ofstream gltf(gltfPath);
  gltf
      << "{\n"
      << "  \"asset\": {\"version\": \"2.0\"},\n"
      << "  \"buffers\": [{\"uri\": \"" << binName
      << "\", \"byteLength\": " << bytes.size() << "}],\n"
      << "  \"bufferViews\": [\n"
      << "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 48},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 48, \"byteLength\": 48},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 12}\n"
      << "  ],\n"
      << "  \"accessors\": [\n"
      << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 4, "
         "\"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": [1, 1, 0]},\n"
      << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 4, "
         "\"type\": \"VEC3\"},\n"
      << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": 6, "
         "\"type\": \"SCALAR\"}\n"
      << "  ],\n"
      << "  \"materials\": [{}],\n"
      << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
         "\"NORMAL\": 1}, \"indices\": 2, \"mode\": 4, \"material\": 0}]}],\n"
      << "  \"nodes\": [{\"mesh\": 0}],\n"
      << "  \"scenes\": [{\"nodes\": [0]}],\n"
      << "  \"scene\": 0\n"
      << "}\n";

  return gltfPath;
}

std::filesystem::path writeMostlyConsistentWindingGltf(std::string_view name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_gltf_loader_tests";
  std::filesystem::create_directories(dir);

  const std::filesystem::path gltfPath = dir / (std::string(name) + ".gltf");
  const std::filesystem::path binPath = dir / (std::string(name) + ".bin");

  std::vector<char> bytes;
  bytes.reserve(450);

  for (int triangle = 0; triangle < 5; ++triangle) {
    const float x = static_cast<float>(triangle) * 2.0f;
    for (const float value : {
             x, 0.0f, 0.0f,
             x + 1.0f, 0.0f, 0.0f,
             x, 1.0f, 0.0f,
         }) {
      appendFloat(bytes, value);
    }
  }
  for (int vertex = 0; vertex < 15; ++vertex) {
    for (const float value : {0.0f, 0.0f, 1.0f}) {
      appendFloat(bytes, value);
    }
  }
  for (const uint16_t index : {
           0, 1, 2,
           3, 4, 5,
           6, 7, 8,
           9, 10, 11,
           12, 14, 13,
       }) {
    appendUint16(bytes, index);
  }

  {
    std::ofstream bin(binPath, std::ios::binary);
    bin.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const std::string binName = binPath.filename().generic_string();
  std::ofstream gltf(gltfPath);
  gltf
      << "{\n"
      << "  \"asset\": {\"version\": \"2.0\"},\n"
      << "  \"buffers\": [{\"uri\": \"" << binName
      << "\", \"byteLength\": " << bytes.size() << "}],\n"
      << "  \"bufferViews\": [\n"
      << "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 180},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 180, \"byteLength\": 180},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 360, \"byteLength\": 30}\n"
      << "  ],\n"
      << "  \"accessors\": [\n"
      << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 15, "
         "\"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": [9, 1, 0]},\n"
      << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 15, "
         "\"type\": \"VEC3\"},\n"
      << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": 15, "
         "\"type\": \"SCALAR\"}\n"
      << "  ],\n"
      << "  \"materials\": [{}],\n"
      << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
         "\"NORMAL\": 1}, \"indices\": 2, \"mode\": 4, \"material\": 0}]}],\n"
      << "  \"nodes\": [{\"mesh\": 0}],\n"
      << "  \"scenes\": [{\"nodes\": [0]}],\n"
      << "  \"scene\": 0\n"
      << "}\n";

  return gltfPath;
}

std::filesystem::path writeDenseOpenReliefGltf(std::string_view name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_gltf_loader_tests";
  std::filesystem::create_directories(dir);

  const std::filesystem::path gltfPath = dir / (std::string(name) + ".gltf");
  const std::filesystem::path binPath = dir / (std::string(name) + ".bin");

  constexpr uint16_t kColumns = 64;
  constexpr uint16_t kRows = 8;
  constexpr uint32_t kVertexCount =
      static_cast<uint32_t>(kColumns + 1u) * static_cast<uint32_t>(kRows + 1u);
  constexpr uint32_t kIndexCount =
      static_cast<uint32_t>(kColumns) * static_cast<uint32_t>(kRows) * 6u;
  constexpr size_t kPositionBytes = kVertexCount * 3u * sizeof(float);
  constexpr size_t kNormalBytes = kVertexCount * 3u * sizeof(float);
  constexpr size_t kIndexBytes = kIndexCount * sizeof(uint16_t);

  std::vector<char> bytes;
  bytes.reserve(kPositionBytes + kNormalBytes + kIndexBytes);

  for (uint16_t y = 0; y <= kRows; ++y) {
    for (uint16_t x = 0; x <= kColumns; ++x) {
      appendFloat(bytes, static_cast<float>(x));
      appendFloat(bytes, static_cast<float>(y));
      appendFloat(bytes, 0.0f);
    }
  }
  for (uint32_t vertex = 0; vertex < kVertexCount; ++vertex) {
    appendFloat(bytes, 0.0f);
    appendFloat(bytes, 0.0f);
    appendFloat(bytes, 1.0f);
  }
  for (uint16_t y = 0; y < kRows; ++y) {
    for (uint16_t x = 0; x < kColumns; ++x) {
      const uint16_t row0 = static_cast<uint16_t>(y * (kColumns + 1u));
      const uint16_t row1 = static_cast<uint16_t>((y + 1u) * (kColumns + 1u));
      const uint16_t v00 = static_cast<uint16_t>(row0 + x);
      const uint16_t v10 = static_cast<uint16_t>(row0 + x + 1u);
      const uint16_t v01 = static_cast<uint16_t>(row1 + x);
      const uint16_t v11 = static_cast<uint16_t>(row1 + x + 1u);
      appendUint16(bytes, v00);
      appendUint16(bytes, v10);
      appendUint16(bytes, v11);
      appendUint16(bytes, v00);
      appendUint16(bytes, v11);
      appendUint16(bytes, v01);
    }
  }

  {
    std::ofstream bin(binPath, std::ios::binary);
    bin.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const std::string binName = binPath.filename().generic_string();
  std::ofstream gltf(gltfPath);
  gltf
      << "{\n"
      << "  \"asset\": {\"version\": \"2.0\"},\n"
      << "  \"buffers\": [{\"uri\": \"" << binName
      << "\", \"byteLength\": " << bytes.size() << "}],\n"
      << "  \"bufferViews\": [\n"
      << "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": "
      << kPositionBytes << "},\n"
      << "    {\"buffer\": 0, \"byteOffset\": " << kPositionBytes
      << ", \"byteLength\": " << kNormalBytes << "},\n"
      << "    {\"buffer\": 0, \"byteOffset\": " << (kPositionBytes + kNormalBytes)
      << ", \"byteLength\": " << kIndexBytes << "}\n"
      << "  ],\n"
      << "  \"accessors\": [\n"
      << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": "
      << kVertexCount
      << ", \"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": ["
      << kColumns << ", " << kRows << ", 0]},\n"
      << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": "
      << kVertexCount << ", \"type\": \"VEC3\"},\n"
      << "    {\"bufferView\": 2, \"componentType\": 5123, \"count\": "
      << kIndexCount << ", \"type\": \"SCALAR\"}\n"
      << "  ],\n"
      << "  \"materials\": [{}],\n"
      << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
         "\"NORMAL\": 1}, \"indices\": 2, \"mode\": 4, \"material\": 0}]}],\n"
      << "  \"nodes\": [{\"mesh\": 0}],\n"
      << "  \"scenes\": [{\"nodes\": [0]}],\n"
      << "  \"scene\": 0\n"
      << "}\n";

  return gltfPath;
}

std::filesystem::path writeTangentTriangleGltf(
    std::string_view name,
    const std::array<glm::vec4, 3>& tangents) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "container_gltf_loader_tests";
  std::filesystem::create_directories(dir);

  const std::filesystem::path gltfPath = dir / (std::string(name) + ".gltf");
  const std::filesystem::path binPath = dir / (std::string(name) + ".bin");

  std::vector<char> bytes;
  bytes.reserve(150);

  for (const float value : {
           0.0f, 0.0f, 0.0f,
           1.0f, 0.0f, 0.0f,
           0.0f, 1.0f, 0.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const float value : {
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
           0.0f, 0.0f, 1.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const float value : {
           0.0f, 0.0f,
           0.0f, 1.0f,
           1.0f, 0.0f,
       }) {
    appendFloat(bytes, value);
  }
  for (const glm::vec4& tangent : tangents) {
    appendFloat(bytes, tangent.x);
    appendFloat(bytes, tangent.y);
    appendFloat(bytes, tangent.z);
    appendFloat(bytes, tangent.w);
  }
  for (const uint16_t index : {0, 1, 2}) {
    appendUint16(bytes, index);
  }

  {
    std::ofstream bin(binPath, std::ios::binary);
    bin.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  const std::string binName = binPath.filename().generic_string();
  std::ofstream gltf(gltfPath);
  gltf
      << "{\n"
      << "  \"asset\": {\"version\": \"2.0\"},\n"
      << "  \"buffers\": [{\"uri\": \"" << binName
      << "\", \"byteLength\": " << bytes.size() << "}],\n"
      << "  \"bufferViews\": [\n"
      << "    {\"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 36},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 72, \"byteLength\": 24},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 96, \"byteLength\": 48},\n"
      << "    {\"buffer\": 0, \"byteOffset\": 144, \"byteLength\": 6}\n"
      << "  ],\n"
      << "  \"accessors\": [\n"
      << "    {\"bufferView\": 0, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC3\", \"min\": [0, 0, 0], \"max\": [1, 1, 0]},\n"
      << "    {\"bufferView\": 1, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC3\"},\n"
      << "    {\"bufferView\": 2, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC2\"},\n"
      << "    {\"bufferView\": 3, \"componentType\": 5126, \"count\": 3, "
         "\"type\": \"VEC4\"},\n"
      << "    {\"bufferView\": 4, \"componentType\": 5123, \"count\": 3, "
         "\"type\": \"SCALAR\"}\n"
      << "  ],\n"
      << "  \"materials\": [{}],\n"
      << "  \"meshes\": [{\"primitives\": [{\"attributes\": {\"POSITION\": 0, "
         "\"NORMAL\": 1, \"TEXCOORD_0\": 2, \"TANGENT\": 3}, "
         "\"indices\": 4, \"mode\": 4, \"material\": 0}]}],\n"
      << "  \"nodes\": [{\"mesh\": 0}],\n"
      << "  \"scenes\": [{\"nodes\": [0]}],\n"
      << "  \"scene\": 0\n"
      << "}\n";

  return gltfPath;
}

glm::vec3 firstTriangleFaceNormal(const container::geometry::Model& model) {
  const auto& vertices = model.vertices();
  const auto& indices = model.indices();
  const glm::vec3& p0 = vertices[indices[0]].position;
  const glm::vec3& p1 = vertices[indices[1]].position;
  const glm::vec3& p2 = vertices[indices[2]].position;
  return glm::normalize(glm::cross(p1 - p0, p2 - p0));
}

void expectTangentNear(const glm::vec4& actual, const glm::vec4& expected) {
  EXPECT_NEAR(actual.x, expected.x, 1e-5f);
  EXPECT_NEAR(actual.y, expected.y, 1e-5f);
  EXPECT_NEAR(actual.z, expected.z, 1e-5f);
  EXPECT_NEAR(actual.w, expected.w, 1e-5f);
}

void expectFiniteOrthonormalTangent(
    const container::geometry::Vertex& vertex) {
  EXPECT_TRUE(std::isfinite(vertex.tangent.x));
  EXPECT_TRUE(std::isfinite(vertex.tangent.y));
  EXPECT_TRUE(std::isfinite(vertex.tangent.z));
  EXPECT_TRUE(std::isfinite(vertex.tangent.w));
  EXPECT_NEAR(glm::length(glm::vec3(vertex.tangent)), 1.0f, 1e-5f);
  EXPECT_NEAR(glm::dot(glm::vec3(vertex.tangent), vertex.normal), 0.0f,
              1e-5f);
  EXPECT_TRUE(vertex.tangent.w == 1.0f || vertex.tangent.w == -1.0f);
}

std::filesystem::path findSampleAsset(std::string_view relativePath) {
  const std::filesystem::path relative{std::string(relativePath)};
  if (std::filesystem::exists(relative)) {
    return std::filesystem::absolute(relative);
  }

  std::vector<std::filesystem::path> roots;
  auto addRoot = [&](std::filesystem::path root) {
    if (!root.empty()) {
      roots.push_back(std::filesystem::absolute(std::move(root)));
    }
  };

  addRoot(std::filesystem::current_path());
  addRoot(std::filesystem::absolute(__FILE__).parent_path().parent_path());

  for (std::filesystem::path cursor = std::filesystem::current_path();
       !cursor.empty(); cursor = cursor.parent_path()) {
    addRoot(cursor);
    if (cursor == cursor.parent_path()) {
      break;
    }
  }

  for (const auto& root : roots) {
    const std::array candidates = {
        root / relative,
        root / "out" / "build" / "windows-debug" / relative,
        root / "out" / "build" / "windows-release" / relative,
    };
    for (const auto& candidate : candidates) {
      if (std::filesystem::exists(candidate)) {
        return std::filesystem::absolute(candidate);
      }
    }
  }

  return {};
}

}  // namespace

TEST(GltfModelLoader, RepairsTriangleWindingOppositeImportedNormals) {
  const auto gltfPath = writeTriangleGltf("bad_winding", {0, 1, 2});

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.indices().size(), 3u);
  ASSERT_EQ(model.primitiveRanges().size(), 1u);
  EXPECT_EQ(model.indices()[0], 0u);
  EXPECT_EQ(model.indices()[1], 2u);
  EXPECT_EQ(model.indices()[2], 1u);
  EXPECT_FALSE(model.primitiveRanges()[0].disableBackfaceCulling);
  EXPECT_GT(firstTriangleFaceNormal(model).z, 0.99f);
}

TEST(GltfModelLoader, KeepsTriangleWindingAlignedWithImportedNormals) {
  const auto gltfPath = writeTriangleGltf("good_winding", {0, 2, 1});

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.indices().size(), 3u);
  ASSERT_EQ(model.primitiveRanges().size(), 1u);
  EXPECT_EQ(model.indices()[0], 0u);
  EXPECT_EQ(model.indices()[1], 2u);
  EXPECT_EQ(model.indices()[2], 1u);
  EXPECT_FALSE(model.primitiveRanges()[0].disableBackfaceCulling);
  EXPECT_GT(firstTriangleFaceNormal(model).z, 0.99f);
}

TEST(GltfModelLoader, DoesNotLocallyFlipMixedEvidencePrimitive) {
  const auto gltfPath = writeMixedWindingQuadGltf("mixed_winding");

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.indices().size(), 6u);
  ASSERT_EQ(model.primitiveRanges().size(), 1u);
  EXPECT_EQ(model.indices()[0], 0u);
  EXPECT_EQ(model.indices()[1], 1u);
  EXPECT_EQ(model.indices()[2], 2u);
  EXPECT_EQ(model.indices()[3], 0u);
  EXPECT_EQ(model.indices()[4], 2u);
  EXPECT_EQ(model.indices()[5], 3u);
  EXPECT_TRUE(model.primitiveRanges()[0].disableBackfaceCulling);
}

TEST(GltfModelLoader, KeepsCullDisabledForIsolatedWindingOutliers) {
  const auto gltfPath =
      writeMostlyConsistentWindingGltf("mostly_consistent_winding");

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.indices().size(), 15u);
  ASSERT_EQ(model.primitiveRanges().size(), 1u);
  EXPECT_EQ(model.indices()[12], 12u);
  EXPECT_EQ(model.indices()[13], 14u);
  EXPECT_EQ(model.indices()[14], 13u);
  EXPECT_TRUE(model.primitiveRanges()[0].disableBackfaceCulling);
}

TEST(GltfModelLoader, DisablesCullForDenseOpenReliefTopology) {
  const auto gltfPath = writeDenseOpenReliefGltf("dense_open_relief");

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.primitiveRanges().size(), 1u);
  EXPECT_TRUE(model.primitiveRanges()[0].disableBackfaceCulling);
}

TEST(GltfModelLoader, SampleTextureSettingsSingleSidedPrimitiveKeepsCulling) {
  const auto gltfPath = findSampleAsset(
      "models/glTF-Sample-Models/2.0/TextureSettingsTest/glTF/"
      "TextureSettingsTest.gltf");
  if (gltfPath.empty()) {
    GTEST_SKIP() << "TextureSettingsTest sample asset is not available";
  }

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_GT(model.primitiveRanges().size(), 8u);
  const auto singleSided = std::ranges::find_if(
      model.primitiveRanges(), [](const container::geometry::PrimitiveRange& p) {
        return p.materialIndex == 8;
      });
  ASSERT_NE(singleSided, model.primitiveRanges().end())
      << "SingleSidedMaterial primitive was not found";
  EXPECT_FALSE(singleSided->disableBackfaceCulling)
      << "TextureSettings single-sided is an open glTF plane, but it must keep "
         "normal back-face culling unless the material is doubleSided.";
}

TEST(GltfModelLoader, SampleSponzaLionReliefPrimitivesUseNoCullException) {
  const auto gltfPath = findSampleAsset(
      "models/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf");
  if (gltfPath.empty()) {
    GTEST_SKIP() << "Sponza sample asset is not available";
  }

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  size_t lionReliefCount = 0;
  for (const auto& primitive : model.primitiveRanges()) {
    if (primitive.materialIndex != 23 || primitive.indexCount < 1024u * 3u) {
      continue;
    }
    ++lionReliefCount;
    EXPECT_TRUE(primitive.disableBackfaceCulling)
        << "Sponza lion relief material 23 is a dense open embossed surface; "
           "it must stay in the no-cull exception bucket.";
  }
  EXPECT_GE(lionReliefCount, 2u)
      << "Expected the two large Sponza lion relief primitives to be present.";
}

TEST(GltfModelLoader, PreservesValidImportedTangents) {
  const glm::vec4 expectedTangent(0.0f, 1.0f, 0.0f, -1.0f);
  const auto gltfPath = writeTangentTriangleGltf(
      "valid_tangents",
      {expectedTangent, expectedTangent, expectedTangent});

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.vertices().size(), 3u);
  for (const auto& vertex : model.vertices()) {
    expectFiniteOrthonormalTangent(vertex);
    expectTangentNear(vertex.tangent, expectedTangent);
  }
}

TEST(GltfModelLoader, RepairsInvalidImportedTangentsFromGeometry) {
  const glm::vec4 expectedTangent(0.0f, 1.0f, 0.0f, -1.0f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const auto gltfPath = writeTangentTriangleGltf(
      "invalid_tangents",
      {expectedTangent, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
       glm::vec4(nan, 0.0f, 0.0f, 1.0f)});

  const auto model =
      container::geometry::Model::LoadFromGltf(gltfPath.string());

  ASSERT_EQ(model.vertices().size(), 3u);
  for (const auto& vertex : model.vertices()) {
    expectFiniteOrthonormalTangent(vertex);
    expectTangentNear(vertex.tangent, expectedTangent);
  }
}
