#include "Container/renderer/bim/BimSectionCapBuilder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using container::renderer::BimSectionCapBuildOptions;
using container::renderer::BimSectionCapDrawStyle;
using container::renderer::BimSectionCapGeneratedMesh;
using container::renderer::BimSectionCapMaterialStyle;
using container::renderer::BimSectionCapTriangle;
using container::renderer::appendBimSectionCapClipPlane;
using container::renderer::BuildBimSectionCapMesh;

std::vector<BimSectionCapTriangle> cubeTriangles(
    uint32_t objectIndex, uint32_t materialIndex,
    const glm::vec3& translation = glm::vec3{0.0f}) {
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

  std::vector<BimSectionCapTriangle> triangles;
  triangles.reserve(faces.size());
  for (const auto& face : faces) {
    triangles.push_back({
        .objectIndex = objectIndex,
        .materialIndex = materialIndex,
        .p0 = v[face[0]] + translation,
        .p1 = v[face[1]] + translation,
        .p2 = v[face[2]] + translation,
    });
  }
  return triangles;
}

uint32_t hatchIndexCountForObject(const BimSectionCapGeneratedMesh& cap,
                                  uint32_t objectIndex) {
  uint32_t count = 0u;
  for (const auto& command : cap.hatchDrawCommands) {
    if (command.objectIndex == objectIndex) {
      count += command.indexCount;
    }
  }
  return count;
}

const BimSectionCapDrawStyle* hatchStyleForMaterial(
    const BimSectionCapGeneratedMesh& cap, uint32_t materialIndex) {
  for (const BimSectionCapDrawStyle& style : cap.hatchDrawStyles) {
    if (style.materialIndex == materialIndex) {
      return &style;
    }
  }
  return nullptr;
}

glm::vec3 firstHatchDirectionAbs(const BimSectionCapGeneratedMesh& cap) {
  if (cap.hatchDrawCommands.empty() ||
      cap.hatchDrawCommands.front().indexCount < 2u) {
    return {0.0f, 0.0f, 0.0f};
  }
  const auto& command = cap.hatchDrawCommands.front();
  const uint32_t i0 = cap.indices[command.firstIndex];
  const uint32_t i1 = cap.indices[command.firstIndex + 1u];
  const glm::vec3 delta = cap.vertices[i1].position - cap.vertices[i0].position;
  const float length = glm::length(delta);
  if (length <= 1.0e-6f) {
    return {0.0f, 0.0f, 0.0f};
  }
  const glm::vec3 direction = delta / length;
  return {std::abs(direction.x), std::abs(direction.y),
          std::abs(direction.z)};
}

bool outsideBounds(const glm::vec3& point, const glm::vec3& min,
                   const glm::vec3& max) {
  return point.x < min.x || point.x > max.x || point.y < min.y ||
         point.y > max.y || point.z < min.z || point.z > max.z;
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

TEST(BimSectionCapBuilderTests,
     MaterialStylesUseDenserConcreteHatchingThanGlass) {
  constexpr uint32_t kConcreteObject = 10u;
  constexpr uint32_t kGlassObject = 11u;
  constexpr uint32_t kConcreteMaterial = 3u;
  constexpr uint32_t kGlassMaterial = 4u;

  std::vector<BimSectionCapTriangle> triangles =
      cubeTriangles(kConcreteObject, kConcreteMaterial, {-3.0f, 0.0f, 0.0f});
  std::vector<BimSectionCapTriangle> glass =
      cubeTriangles(kGlassObject, kGlassMaterial, {3.0f, 0.0f, 0.0f});
  triangles.insert(triangles.end(), glass.begin(), glass.end());

  BimSectionCapBuildOptions options{};
  options.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  options.hatchSpacing = 1.0f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;
  options.materialStyles = {
      BimSectionCapMaterialStyle{.materialIndex = kConcreteMaterial,
                                 .fillColor = {0.18f, 0.18f, 0.16f},
                                 .fillOpacity = 0.9f,
                                 .hatchSpacing = 0.2f,
                                 .hatchAngleRadians = 0.0f,
                                 .hatchColor = {0.02f, 0.02f, 0.02f}},
      BimSectionCapMaterialStyle{.materialIndex = kGlassMaterial,
                                 .fillColor = {0.4f, 0.7f, 0.9f},
                                 .fillOpacity = 0.35f,
                                 .hatchSpacing = 0.8f,
                                 .hatchAngleRadians = 0.0f,
                                 .hatchColor = {0.15f, 0.35f, 0.5f}},
  };

  const BimSectionCapGeneratedMesh cap =
      BuildBimSectionCapMesh(triangles, options);

  ASSERT_TRUE(cap.valid());
  ASSERT_EQ(cap.hatchDrawStyles.size(), cap.hatchDrawCommands.size());
  const BimSectionCapDrawStyle* concrete =
      hatchStyleForMaterial(cap, kConcreteMaterial);
  const BimSectionCapDrawStyle* glassStyle =
      hatchStyleForMaterial(cap, kGlassMaterial);
  ASSERT_NE(concrete, nullptr);
  ASSERT_NE(glassStyle, nullptr);
  EXPECT_FLOAT_EQ(concrete->hatchSpacing, 0.2f);
  EXPECT_FLOAT_EQ(glassStyle->hatchSpacing, 0.8f);
  EXPECT_GT(hatchIndexCountForObject(cap, kConcreteObject),
            hatchIndexCountForObject(cap, kGlassObject));
}

TEST(BimSectionCapBuilderTests,
     UnstyledCapsInheritGlobalFillAndHatchDefaults) {
  constexpr uint32_t kObjectIndex = 12u;
  const glm::vec3 kFillColor{0.21f, 0.31f, 0.41f};
  const glm::vec3 kHatchColor{0.72f, 0.62f, 0.52f};
  const std::vector<BimSectionCapTriangle> triangles = cubeTriangles(
      kObjectIndex, std::numeric_limits<uint32_t>::max());

  BimSectionCapBuildOptions options{};
  options.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  options.fillColor = kFillColor;
  options.fillOpacity = 0.47f;
  options.hatchColor = kHatchColor;
  options.hatchSpacing = 0.5f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;

  const BimSectionCapGeneratedMesh cap =
      BuildBimSectionCapMesh(triangles, options);

  ASSERT_TRUE(cap.valid());
  ASSERT_FALSE(cap.fillDrawStyles.empty());
  ASSERT_FALSE(cap.hatchDrawStyles.empty());
  EXPECT_EQ(cap.fillDrawStyles.front().materialIndex,
            std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(cap.fillDrawStyles.front().fillColor, kFillColor);
  EXPECT_FLOAT_EQ(cap.fillDrawStyles.front().fillOpacity, 0.47f);
  EXPECT_EQ(cap.hatchDrawStyles.front().hatchColor, kHatchColor);
}

TEST(BimSectionCapBuilderTests,
     HatchDirectionAndCountAreStableForElevationPlaneSigns) {
  auto buildForPlane = [](glm::vec4 plane, bool reverseTriangles) {
    std::vector<BimSectionCapTriangle> triangles = cubeTriangles(
        22u, std::numeric_limits<uint32_t>::max(), {0.0f, 0.0f, 0.0f});
    if (reverseTriangles) {
      std::ranges::reverse(triangles);
    }

    BimSectionCapBuildOptions options{};
    options.sectionPlane = plane;
    options.hatchSpacing = 0.35f;
    options.hatchAngleRadians = 0.25f;
    options.capOffset = 0.0f;
    return BuildBimSectionCapMesh(triangles, options);
  };

  const BimSectionCapGeneratedMesh front =
      buildForPlane({0.0f, 0.0f, 1.0f, 0.0f}, false);
  const BimSectionCapGeneratedMesh back =
      buildForPlane({0.0f, 0.0f, -1.0f, 0.0f}, true);
  const BimSectionCapGeneratedMesh right =
      buildForPlane({1.0f, 0.0f, 0.0f, 0.0f}, false);
  const BimSectionCapGeneratedMesh left =
      buildForPlane({-1.0f, 0.0f, 0.0f, 0.0f}, true);

  ASSERT_TRUE(front.valid());
  ASSERT_TRUE(back.valid());
  ASSERT_TRUE(right.valid());
  ASSERT_TRUE(left.valid());
  EXPECT_EQ(front.hatchDrawCommands.front().indexCount,
            back.hatchDrawCommands.front().indexCount);
  EXPECT_EQ(right.hatchDrawCommands.front().indexCount,
            left.hatchDrawCommands.front().indexCount);

  const glm::vec3 frontDirection = firstHatchDirectionAbs(front);
  const glm::vec3 backDirection = firstHatchDirectionAbs(back);
  const glm::vec3 rightDirection = firstHatchDirectionAbs(right);
  const glm::vec3 leftDirection = firstHatchDirectionAbs(left);
  EXPECT_NEAR(frontDirection.x, backDirection.x, 1.0e-5f);
  EXPECT_NEAR(frontDirection.y, backDirection.y, 1.0e-5f);
  EXPECT_NEAR(frontDirection.z, backDirection.z, 1.0e-5f);
  EXPECT_NEAR(rightDirection.x, leftDirection.x, 1.0e-5f);
  EXPECT_NEAR(rightDirection.y, leftDirection.y, 1.0e-5f);
  EXPECT_NEAR(rightDirection.z, leftDirection.z, 1.0e-5f);
}

TEST(BimSectionCapBuilderTests, SectionMarkerArrowsAreOutsideModelBounds) {
  const std::vector<BimSectionCapTriangle> triangles =
      cubeTriangles(33u, std::numeric_limits<uint32_t>::max());

  BimSectionCapBuildOptions options{};
  options.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  options.hatchSpacing = 0.5f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;

  const BimSectionCapGeneratedMesh cap =
      BuildBimSectionCapMesh(triangles, options);

  ASSERT_TRUE(cap.valid());
  ASSERT_FALSE(cap.sectionMarkerLines.empty());
  const auto arrowLine = std::ranges::find_if(cap.sectionMarkerLines,
                                              [](const auto& line) {
                                                return line.startArrow ||
                                                       line.endArrow;
                                              });
  ASSERT_NE(arrowLine, cap.sectionMarkerLines.end());
  EXPECT_GT(arrowLine->lineWidth, 0.0f);
  if (arrowLine->startArrow) {
    EXPECT_TRUE(outsideBounds(arrowLine->a, {-1.0f, -1.0f, -1.0f},
                              {1.0f, 1.0f, 1.0f}));
  }
  if (arrowLine->endArrow) {
    EXPECT_TRUE(outsideBounds(arrowLine->b, {-1.0f, -1.0f, -1.0f},
                              {1.0f, 1.0f, 1.0f}));
  }
}

TEST(BimSectionCapBuilderTests,
     ComposedSectionAndBoxClipPlanesGenerateAndClipAllCaps) {
  constexpr uint32_t kObjectIndex = 44u;
  const std::vector<BimSectionCapTriangle> triangles =
      cubeTriangles(kObjectIndex, std::numeric_limits<uint32_t>::max());

  BimSectionCapBuildOptions options{};
  options.hatchSpacing = 0.5f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;
  appendBimSectionCapClipPlane(options, {0.0f, 1.0f, 0.0f, 0.0f});
  appendBimSectionCapClipPlane(options, {1.0f, 0.0f, 0.0f, 0.5f});
  appendBimSectionCapClipPlane(options, {-1.0f, 0.0f, 0.0f, 0.5f});

  const BimSectionCapGeneratedMesh cap =
      BuildBimSectionCapMesh(triangles, options);

  ASSERT_TRUE(cap.valid());
  ASSERT_EQ(options.clipPlaneCount, 3u);
  EXPECT_EQ(cap.fillDrawCommands.size(), 3u);

  std::array<bool, 3> sawCapNormal{};
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

TEST(BimSectionCapBuilderTests,
     ComposedSectionAndFullBoxClipRetainsAllSevenPlanes) {
  constexpr uint32_t kObjectIndex = 45u;
  const std::vector<BimSectionCapTriangle> triangles =
      cubeTriangles(kObjectIndex, std::numeric_limits<uint32_t>::max());

  BimSectionCapBuildOptions options{};
  options.hatchSpacing = 0.5f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;
  const std::array<glm::vec4, 7> planes{{
      {0.0f, 1.0f, 0.0f, 0.0f},
      {1.0f, 0.0f, 0.0f, 0.5f},
      {-1.0f, 0.0f, 0.0f, 0.5f},
      {0.0f, 1.0f, 0.0f, 0.5f},
      {0.0f, -1.0f, 0.0f, 0.5f},
      {0.0f, 0.0f, 1.0f, 0.5f},
      {0.0f, 0.0f, -1.0f, 0.5f},
  }};
  for (const glm::vec4& plane : planes) {
    EXPECT_TRUE(appendBimSectionCapClipPlane(options, plane));
  }

  const BimSectionCapGeneratedMesh cap =
      BuildBimSectionCapMesh(triangles, options);

  ASSERT_EQ(options.clipPlaneCount, planes.size());
  ASSERT_TRUE(cap.valid());
  bool sawSectionCap = false;
  for (const container::geometry::Vertex& vertex : cap.vertices) {
    for (const glm::vec4& plane : planes) {
      EXPECT_GE(planeDistance(plane, vertex.position), -1.0e-5f);
    }
    sawSectionCap =
        sawSectionCap || normalMatchesPlane(vertex.normal, planes.front());
  }
  EXPECT_TRUE(sawSectionCap);
}

TEST(BimSectionCapBuilderTests,
     InvertedBoxClipOptionsFailClosedWithoutRenderingCaps) {
  const std::vector<BimSectionCapTriangle> triangles =
      cubeTriangles(46u, std::numeric_limits<uint32_t>::max());

  BimSectionCapBuildOptions options{};
  options.invertedBoxClip = true;
  options.hatchSpacing = 0.5f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;
  appendBimSectionCapClipPlane(options, {0.0f, 1.0f, 0.0f, 0.0f});
  appendBimSectionCapClipPlane(options, {1.0f, 0.0f, 0.0f, 0.5f});
  appendBimSectionCapClipPlane(options, {-1.0f, 0.0f, 0.0f, 0.5f});

  const BimSectionCapGeneratedMesh cap =
      BuildBimSectionCapMesh(triangles, options);

  EXPECT_FALSE(cap.valid());
  EXPECT_TRUE(cap.fillDrawCommands.empty());
  EXPECT_TRUE(cap.hatchDrawCommands.empty());
  EXPECT_TRUE(cap.sectionMarkerLines.empty());
}

TEST(BimSectionCapBuilderTests,
     MarkersAreNotProducedWhenClipPlanesRemoveFinalCapGeometry) {
  const std::vector<BimSectionCapTriangle> triangles =
      cubeTriangles(55u, std::numeric_limits<uint32_t>::max());

  BimSectionCapBuildOptions options{};
  options.hatchSpacing = 0.5f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;
  appendBimSectionCapClipPlane(options, {0.0f, 1.0f, 0.0f, 0.0f});
  appendBimSectionCapClipPlane(options, {1.0f, 0.0f, 0.0f, -2.0f});

  const BimSectionCapGeneratedMesh cap =
      BuildBimSectionCapMesh(triangles, options);

  EXPECT_TRUE(cap.fillDrawCommands.empty());
  EXPECT_TRUE(cap.hatchDrawCommands.empty());
  EXPECT_TRUE(cap.sectionMarkerLines.empty());
}
