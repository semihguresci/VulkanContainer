#include "Container/geometry/Model.h"

#include <gtest/gtest.h>

#include <glm/geometric.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

glm::vec3 firstTriangleFaceNormal(const container::geometry::Model& model) {
  const auto& vertices = model.vertices();
  const auto& indices = model.indices();
  const glm::vec3& p0 = vertices[indices[0]].position;
  const glm::vec3& p1 = vertices[indices[1]].position;
  const glm::vec3& p2 = vertices[indices[2]].position;
  return glm::normalize(glm::cross(p1 - p0, p2 - p0));
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
  EXPECT_TRUE(model.primitiveRanges()[0].disableBackfaceCulling);
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
  EXPECT_TRUE(model.primitiveRanges()[0].disableBackfaceCulling);
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
