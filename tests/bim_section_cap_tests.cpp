#include "Container/renderer/BimManager.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

namespace {

std::vector<container::renderer::BimSectionCapTriangle> cubeTriangles(
    uint32_t objectIndex) {
  const std::array<glm::vec3, 8> v{{
      {-1.0f, -1.0f, -1.0f},
      {1.0f, -1.0f, -1.0f},
      {1.0f, 1.0f, -1.0f},
      {-1.0f, 1.0f, -1.0f},
      {-1.0f, -1.0f, 1.0f},
      {1.0f, -1.0f, 1.0f},
      {1.0f, 1.0f, 1.0f},
      {-1.0f, 1.0f, 1.0f},
  }};
  const std::array<std::array<uint32_t, 3>, 12> faces{{
      {{0u, 1u, 2u}},
      {{0u, 2u, 3u}},
      {{4u, 6u, 5u}},
      {{4u, 7u, 6u}},
      {{0u, 4u, 5u}},
      {{0u, 5u, 1u}},
      {{1u, 5u, 6u}},
      {{1u, 6u, 2u}},
      {{2u, 6u, 7u}},
      {{2u, 7u, 3u}},
      {{3u, 7u, 4u}},
      {{3u, 4u, 0u}},
  }};

  std::vector<container::renderer::BimSectionCapTriangle> triangles;
  triangles.reserve(faces.size());
  for (const auto& face : faces) {
    triangles.push_back({
        .objectIndex = objectIndex,
        .p0 = v[face[0]],
        .p1 = v[face[1]],
        .p2 = v[face[2]],
    });
  }
  return triangles;
}

std::vector<container::renderer::BimSectionCapTriangle> translatedCubeTriangles(
    uint32_t objectIndex, const glm::vec3& translation) {
  std::vector<container::renderer::BimSectionCapTriangle> triangles =
      cubeTriangles(objectIndex);
  for (container::renderer::BimSectionCapTriangle& triangle : triangles) {
    triangle.p0 += translation;
    triangle.p1 += translation;
    triangle.p2 += translation;
  }
  return triangles;
}

float planeDistance(const glm::vec4& plane, const glm::vec3& point) {
  return glm::dot(glm::vec3{plane}, point) + plane.w;
}

bool normalMatchesPlane(const glm::vec3& normal, const glm::vec4& plane) {
  const glm::vec4 normalizedPlane =
      container::renderer::normalizedSectionCapPlane(plane);
  return glm::dot(normal, glm::vec3{normalizedPlane}) > 0.999f;
}

}  // namespace

TEST(BimSectionCap, GeneratesFillAndHatchDrawListsForClippedMesh) {
  constexpr uint32_t kObjectIndex = 7u;
  const std::vector<container::renderer::BimSectionCapTriangle> triangles =
      cubeTriangles(kObjectIndex);

  container::renderer::BimSectionCapBuildOptions options{};
  options.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  options.hatchSpacing = 0.5f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;

  const container::renderer::BimSectionCapGeneratedMesh cap =
      container::renderer::BuildBimSectionCapMesh(triangles, options);

  ASSERT_TRUE(cap.valid());
  ASSERT_EQ(cap.fillDrawCommands.size(), 1u);
  ASSERT_EQ(cap.hatchDrawCommands.size(), 1u);
  EXPECT_EQ(cap.fillDrawCommands.front().objectIndex, kObjectIndex);
  EXPECT_EQ(cap.hatchDrawCommands.front().objectIndex, kObjectIndex);
  EXPECT_EQ(cap.fillDrawCommands.front().firstIndex, 0u);
  EXPECT_EQ(cap.fillDrawCommands.front().indexCount % 3u, 0u);
  EXPECT_EQ(cap.hatchDrawCommands.front().indexCount % 2u, 0u);
  EXPECT_GT(cap.hatchDrawCommands.front().firstIndex,
            cap.fillDrawCommands.front().firstIndex);

  for (const container::geometry::Vertex& vertex : cap.vertices) {
    EXPECT_NEAR(vertex.position.y, 0.0f, 1.0e-5f);
    EXPECT_NEAR(vertex.normal.y, 1.0f, 1.0e-5f);
  }
}

TEST(BimSectionCap, DisconnectedLoopsDoNotFanAcrossGap) {
  constexpr uint32_t kObjectIndex = 29u;
  std::vector<container::renderer::BimSectionCapTriangle> triangles =
      translatedCubeTriangles(kObjectIndex, {-3.0f, 0.0f, 0.0f});
  std::vector<container::renderer::BimSectionCapTriangle> rightCube =
      translatedCubeTriangles(kObjectIndex, {3.0f, 0.0f, 0.0f});
  triangles.insert(triangles.end(), rightCube.begin(), rightCube.end());

  container::renderer::BimSectionCapBuildOptions options{};
  options.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  options.hatchSpacing = 0.5f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;

  const container::renderer::BimSectionCapGeneratedMesh cap =
      container::renderer::BuildBimSectionCapMesh(triangles, options);

  ASSERT_TRUE(cap.valid());
  ASSERT_EQ(cap.fillDrawCommands.size(), 1u);
  for (uint32_t indexOffset = 0u;
       indexOffset < cap.fillDrawCommands.front().indexCount; ++indexOffset) {
    const uint32_t vertexIndex =
        cap.indices[cap.fillDrawCommands.front().firstIndex + indexOffset];
    ASSERT_LT(vertexIndex, cap.vertices.size());
    const float x = cap.vertices[vertexIndex].position.x;
    EXPECT_TRUE(x <= -2.0f || x >= 2.0f)
        << "Cap fill vertex bridged the gap at x=" << x;
  }
}

TEST(BimSectionCap, CrossHatchingAddsAdditionalLineIndices) {
  constexpr uint32_t kObjectIndex = 11u;
  const std::vector<container::renderer::BimSectionCapTriangle> triangles =
      cubeTriangles(kObjectIndex);

  container::renderer::BimSectionCapBuildOptions diagonal{};
  diagonal.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  diagonal.hatchSpacing = 0.5f;
  diagonal.hatchAngleRadians = 0.0f;
  diagonal.capOffset = 0.0f;

  container::renderer::BimSectionCapBuildOptions cross = diagonal;
  cross.crossHatch = true;

  const container::renderer::BimSectionCapGeneratedMesh diagonalCap =
      container::renderer::BuildBimSectionCapMesh(triangles, diagonal);
  const container::renderer::BimSectionCapGeneratedMesh crossCap =
      container::renderer::BuildBimSectionCapMesh(triangles, cross);

  ASSERT_TRUE(diagonalCap.valid());
  ASSERT_TRUE(crossCap.valid());
  ASSERT_EQ(diagonalCap.hatchDrawCommands.size(), 1u);
  ASSERT_EQ(crossCap.hatchDrawCommands.size(), 1u);
  EXPECT_EQ(crossCap.hatchDrawCommands.front().objectIndex, kObjectIndex);
  EXPECT_GT(crossCap.hatchDrawCommands.front().indexCount,
            diagonalCap.hatchDrawCommands.front().indexCount);
}

TEST(BimSectionCap, MultiPlaneClipBoundsGeneratedVertices) {
  constexpr uint32_t kObjectIndex = 17u;
  const std::vector<container::renderer::BimSectionCapTriangle> triangles =
      cubeTriangles(kObjectIndex);

  container::renderer::BimSectionCapBuildOptions options{};
  options.clipPlaneCount = 2u;
  options.clipPlanes[0] = {0.0f, 1.0f, 0.0f, 0.0f};
  options.clipPlanes[1] = {1.0f, 0.0f, 0.0f, 0.0f};
  options.hatchSpacing = 0.5f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;

  const container::renderer::BimSectionCapGeneratedMesh cap =
      container::renderer::BuildBimSectionCapMesh(triangles, options);

  ASSERT_TRUE(cap.valid());
  ASSERT_EQ(cap.fillDrawCommands.size(), 2u);
  ASSERT_EQ(cap.hatchDrawCommands.size(), 2u);
  for (const container::renderer::DrawCommand& command : cap.fillDrawCommands) {
    EXPECT_EQ(command.objectIndex, kObjectIndex);
  }
  for (const container::renderer::DrawCommand& command :
       cap.hatchDrawCommands) {
    EXPECT_EQ(command.objectIndex, kObjectIndex);
  }

  bool sawHorizontalCap = false;
  bool sawVerticalCap = false;
  for (const container::geometry::Vertex& vertex : cap.vertices) {
    EXPECT_GE(planeDistance(options.clipPlanes[0], vertex.position),
              -1.0e-5f);
    EXPECT_GE(planeDistance(options.clipPlanes[1], vertex.position),
              -1.0e-5f);
    sawHorizontalCap = sawHorizontalCap ||
                       normalMatchesPlane(vertex.normal, options.clipPlanes[0]);
    sawVerticalCap = sawVerticalCap ||
                     normalMatchesPlane(vertex.normal, options.clipPlanes[1]);
  }
  EXPECT_TRUE(sawHorizontalCap);
  EXPECT_TRUE(sawVerticalCap);
}

TEST(BimSectionCap, BoxClipGeneratesCapsForAllPlanes) {
  constexpr uint32_t kObjectIndex = 23u;
  const std::vector<container::renderer::BimSectionCapTriangle> triangles =
      cubeTriangles(kObjectIndex);

  container::renderer::BimSectionCapBuildOptions options{};
  options.clipPlaneCount = 6u;
  options.clipPlanes[0] = {1.0f, 0.0f, 0.0f, 0.5f};
  options.clipPlanes[1] = {-1.0f, 0.0f, 0.0f, 0.5f};
  options.clipPlanes[2] = {0.0f, 1.0f, 0.0f, 0.5f};
  options.clipPlanes[3] = {0.0f, -1.0f, 0.0f, 0.5f};
  options.clipPlanes[4] = {0.0f, 0.0f, 1.0f, 0.5f};
  options.clipPlanes[5] = {0.0f, 0.0f, -1.0f, 0.5f};
  options.hatchSpacing = 0.5f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;

  const container::renderer::BimSectionCapGeneratedMesh cap =
      container::renderer::BuildBimSectionCapMesh(triangles, options);

  ASSERT_TRUE(cap.valid());
  EXPECT_EQ(cap.fillDrawCommands.size(), 6u);
  EXPECT_EQ(cap.hatchDrawCommands.size(), 6u);
  for (const container::renderer::DrawCommand& command : cap.fillDrawCommands) {
    EXPECT_EQ(command.objectIndex, kObjectIndex);
  }
  for (const container::renderer::DrawCommand& command :
       cap.hatchDrawCommands) {
    EXPECT_EQ(command.objectIndex, kObjectIndex);
  }

  std::array<bool, 6> sawCapNormal{};
  for (const container::geometry::Vertex& vertex : cap.vertices) {
    for (uint32_t planeIndex = 0; planeIndex < options.clipPlaneCount;
         ++planeIndex) {
      EXPECT_GE(planeDistance(options.clipPlanes[planeIndex], vertex.position),
                -1.0e-5f);
      if (normalMatchesPlane(vertex.normal, options.clipPlanes[planeIndex])) {
        sawCapNormal[planeIndex] = true;
      }
    }
  }
  for (bool sawNormal : sawCapNormal) {
    EXPECT_TRUE(sawNormal);
  }
}
