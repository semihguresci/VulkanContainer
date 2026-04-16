#include "Container/app/AppConfig.h"
#include "Container/common/CommonMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <glm/glm.hpp>

namespace {

glm::vec3 clipToNdc(const glm::vec4& clip) {
  return glm::vec3(clip) / clip.w;
}

glm::vec2 sceneUvToNdc(const glm::vec2& uv) {
  return {uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f};
}

glm::vec2 sceneNdcToUv(const glm::vec2& ndc) {
  return {ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f};
}

glm::vec2 shadowNdcToUv(const glm::vec2& ndc) {
  return ndc * 0.5f + 0.5f;
}

glm::vec2 sceneNdcToFramebuffer(const glm::vec2& ndc) {
  return {(ndc.x + 1.0f) * 0.5f, (1.0f - ndc.y) * 0.5f};
}

glm::vec2 shadowNdcToFramebuffer(const glm::vec2& ndc) {
  return (ndc + 1.0f) * 0.5f;
}

float linearizeReverseZPerspectiveDepth(float depth, float nearPlane, float farPlane) {
  return (nearPlane * farPlane) / (depth * (farPlane - nearPlane) + nearPlane);
}

glm::vec3 applyGlTfNormalScale(glm::vec3 decodedNormal, float scale) {
  decodedNormal.x *= scale;
  decodedNormal.y *= scale;
  return glm::normalize(decodedNormal);
}

float applyOcclusionStrength(float sampledOcclusion, float strength) {
  return 1.0f + (sampledOcclusion - 1.0f) * strength;
}

float signedArea(const std::array<glm::vec2, 3>& triangle) {
  const glm::vec2 ab = triangle[1] - triangle[0];
  const glm::vec2 ac = triangle[2] - triangle[0];
  return ab.x * ac.y - ab.y * ac.x;
}

float slangMatrixAt(const glm::mat4& matrix, int row, int column) {
  return matrix[column][row];
}

glm::vec4 slangMatrixRow(const glm::mat4& matrix, int row) {
  return {
      slangMatrixAt(matrix, row, 0),
      slangMatrixAt(matrix, row, 1),
      slangMatrixAt(matrix, row, 2),
      slangMatrixAt(matrix, row, 3),
  };
}

glm::vec4 normalizePlane(glm::vec4 plane) {
  const float lengthSq = glm::dot(glm::vec3(plane), glm::vec3(plane));
  if (lengthSq <= 1e-8f) {
    return plane;
  }
  return plane / std::sqrt(lengthSq);
}

using FrustumPlanes = std::array<glm::vec4, 6>;

FrustumPlanes extractFrustumPlanesUsingSlangRows(const glm::mat4& matrix) {
  const glm::vec4 row0 = slangMatrixRow(matrix, 0);
  const glm::vec4 row1 = slangMatrixRow(matrix, 1);
  const glm::vec4 row2 = slangMatrixRow(matrix, 2);
  const glm::vec4 row3 = slangMatrixRow(matrix, 3);

  return {{
      normalizePlane(row3 + row0),
      normalizePlane(row3 - row0),
      normalizePlane(row3 + row1),
      normalizePlane(row3 - row1),
      normalizePlane(row2),
      normalizePlane(row3 - row2),
  }};
}

FrustumPlanes extractFrustumPlanesUsingWrongColumnAccess(const glm::mat4& matrix) {
  return {{
      normalizePlane({slangMatrixAt(matrix, 0, 3) + slangMatrixAt(matrix, 0, 0),
                      slangMatrixAt(matrix, 1, 3) + slangMatrixAt(matrix, 1, 0),
                      slangMatrixAt(matrix, 2, 3) + slangMatrixAt(matrix, 2, 0),
                      slangMatrixAt(matrix, 3, 3) + slangMatrixAt(matrix, 3, 0)}),
      normalizePlane({slangMatrixAt(matrix, 0, 3) - slangMatrixAt(matrix, 0, 0),
                      slangMatrixAt(matrix, 1, 3) - slangMatrixAt(matrix, 1, 0),
                      slangMatrixAt(matrix, 2, 3) - slangMatrixAt(matrix, 2, 0),
                      slangMatrixAt(matrix, 3, 3) - slangMatrixAt(matrix, 3, 0)}),
      normalizePlane({slangMatrixAt(matrix, 0, 3) + slangMatrixAt(matrix, 0, 1),
                      slangMatrixAt(matrix, 1, 3) + slangMatrixAt(matrix, 1, 1),
                      slangMatrixAt(matrix, 2, 3) + slangMatrixAt(matrix, 2, 1),
                      slangMatrixAt(matrix, 3, 3) + slangMatrixAt(matrix, 3, 1)}),
      normalizePlane({slangMatrixAt(matrix, 0, 3) - slangMatrixAt(matrix, 0, 1),
                      slangMatrixAt(matrix, 1, 3) - slangMatrixAt(matrix, 1, 1),
                      slangMatrixAt(matrix, 2, 3) - slangMatrixAt(matrix, 2, 1),
                      slangMatrixAt(matrix, 3, 3) - slangMatrixAt(matrix, 3, 1)}),
      normalizePlane({slangMatrixAt(matrix, 0, 2),
                      slangMatrixAt(matrix, 1, 2),
                      slangMatrixAt(matrix, 2, 2),
                      slangMatrixAt(matrix, 3, 2)}),
      normalizePlane({slangMatrixAt(matrix, 0, 3) - slangMatrixAt(matrix, 0, 2),
                      slangMatrixAt(matrix, 1, 3) - slangMatrixAt(matrix, 1, 2),
                      slangMatrixAt(matrix, 2, 3) - slangMatrixAt(matrix, 2, 2),
                      slangMatrixAt(matrix, 3, 3) - slangMatrixAt(matrix, 3, 2)}),
  }};
}

bool pointInsideFrustum(const FrustumPlanes& planes, const glm::vec3& point) {
  for (const glm::vec4& plane : planes) {
    if (glm::dot(glm::vec3(plane), point) + plane.w < 0.0f) {
      return false;
    }
  }
  return true;
}

bool pointInsideClipVolume(const glm::mat4& viewProj, const glm::vec3& point) {
  const glm::vec4 clip = viewProj * glm::vec4(point, 1.0f);
  if (clip.w <= 0.0f) {
    return false;
  }

  return clip.x >= -clip.w && clip.x <= clip.w &&
         clip.y >= -clip.w && clip.y <= clip.w &&
         clip.z >= 0.0f && clip.z <= clip.w;
}

float rowXyzLength(const glm::mat4& matrix, int row) {
  const glm::vec4 r = slangMatrixRow(matrix, row);
  return glm::length(glm::vec3(r));
}

}  // namespace

TEST(RenderingConventionTests, PerspectiveReverseZMapsNearAndFarToOneAndZero) {
  constexpr float kNear = 0.1f;
  constexpr float kFar = 100.0f;

  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(60.0f), 16.0f / 9.0f, kNear, kFar);

  const glm::vec3 nearNdc = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kNear, 1.0f));
  const glm::vec3 farNdc = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kFar, 1.0f));

  EXPECT_NEAR(nearNdc.z, 1.0f, 1e-5f);
  EXPECT_NEAR(farNdc.z, 0.0f, 1e-5f);
}

TEST(RenderingConventionTests, OrthoReverseZMapsNearAndFarToOneAndZero) {
  constexpr float kNear = 2.0f;
  constexpr float kFar = 10.0f;

  const glm::mat4 proj = container::math::orthoRH_ReverseZ(
      -4.0f, 4.0f, -3.0f, 3.0f, kNear, kFar);

  const glm::vec3 nearNdc = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kNear, 1.0f));
  const glm::vec3 farNdc = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kFar, 1.0f));

  EXPECT_NEAR(nearNdc.z, 1.0f, 1e-5f);
  EXPECT_NEAR(farNdc.z, 0.0f, 1e-5f);
}

TEST(RenderingConventionTests, ReverseZPerspectiveLinearizationRecoversViewDistance) {
  constexpr float kNear = 0.1f;
  constexpr float kFar = 100.0f;
  constexpr float kMid = 12.5f;

  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(60.0f), 16.0f / 9.0f, kNear, kFar);

  const float nearDepth = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kNear, 1.0f)).z;
  const float midDepth = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kMid, 1.0f)).z;
  const float farDepth = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kFar, 1.0f)).z;

  EXPECT_NEAR(linearizeReverseZPerspectiveDepth(nearDepth, kNear, kFar), kNear, 1e-5f);
  EXPECT_NEAR(linearizeReverseZPerspectiveDepth(midDepth, kNear, kFar), kMid, 1e-4f);
  EXPECT_NEAR(linearizeReverseZPerspectiveDepth(farDepth, kNear, kFar), kFar, 1e-3f);
}

TEST(RenderingConventionTests, GlTfNormalScaleOnlyScalesTangentSpaceXY) {
  const glm::vec3 decodedNormal = glm::normalize(glm::vec3(0.4f, -0.3f, 1.0f));

  const glm::vec3 flattened = applyGlTfNormalScale(decodedNormal, 0.0f);
  const glm::vec3 boosted = applyGlTfNormalScale(decodedNormal, 2.0f);

  EXPECT_NEAR(flattened.x, 0.0f, 1e-6f);
  EXPECT_NEAR(flattened.y, 0.0f, 1e-6f);
  EXPECT_NEAR(flattened.z, 1.0f, 1e-6f);
  EXPECT_GT(std::abs(boosted.x), std::abs(decodedNormal.x));
  EXPECT_GT(std::abs(boosted.y), std::abs(decodedNormal.y));
  EXPECT_LT(boosted.z, decodedNormal.z);
}

TEST(RenderingConventionTests, GlTfOcclusionStrengthInterpolatesFromOneToTexture) {
  constexpr float kSampledOcclusion = 0.35f;

  EXPECT_NEAR(applyOcclusionStrength(kSampledOcclusion, 0.0f), 1.0f, 1e-6f);
  EXPECT_NEAR(applyOcclusionStrength(kSampledOcclusion, 1.0f), kSampledOcclusion, 1e-6f);
  EXPECT_NEAR(applyOcclusionStrength(kSampledOcclusion, 0.25f), 0.8375f, 1e-6f);
}

TEST(RenderingConventionTests, SceneViewportUvAndNdcRoundTripFlipsY) {
  const glm::vec2 topLeftNdc = sceneUvToNdc({0.0f, 0.0f});
  const glm::vec2 bottomLeftNdc = sceneUvToNdc({0.0f, 1.0f});

  EXPECT_NEAR(topLeftNdc.x, -1.0f, 1e-6f);
  EXPECT_NEAR(topLeftNdc.y, 1.0f, 1e-6f);
  EXPECT_NEAR(bottomLeftNdc.x, -1.0f, 1e-6f);
  EXPECT_NEAR(bottomLeftNdc.y, -1.0f, 1e-6f);

  const glm::vec2 uv = {0.27f, 0.81f};
  const glm::vec2 roundTrip = sceneNdcToUv(sceneUvToNdc(uv));

  EXPECT_NEAR(roundTrip.x, uv.x, 1e-6f);
  EXPECT_NEAR(roundTrip.y, uv.y, 1e-6f);
}

TEST(RenderingConventionTests, ShadowViewportUsesPositiveHeightMapping) {
  const glm::vec2 topLeftUv = shadowNdcToUv({-1.0f, -1.0f});
  const glm::vec2 bottomRightUv = shadowNdcToUv({1.0f, 1.0f});

  EXPECT_NEAR(topLeftUv.x, 0.0f, 1e-6f);
  EXPECT_NEAR(topLeftUv.y, 0.0f, 1e-6f);
  EXPECT_NEAR(bottomRightUv.x, 1.0f, 1e-6f);
  EXPECT_NEAR(bottomRightUv.y, 1.0f, 1e-6f);

  const glm::vec2 shadowUv = shadowNdcToUv({0.25f, 0.75f});
  const glm::vec2 sceneUv = sceneNdcToUv({0.25f, 0.75f});
  EXPECT_GT(shadowUv.y, sceneUv.y);
}

TEST(RenderingConventionTests, NegativeSceneViewportFlipsFramebufferWinding) {
  const std::array<glm::vec2, 3> ccwNdcTriangle = {{
      {-0.5f, -0.5f},
      {0.5f, -0.5f},
      {0.0f, 0.5f},
  }};

  const float ndcArea = signedArea(ccwNdcTriangle);
  const float sceneFramebufferArea = signedArea({{
      sceneNdcToFramebuffer(ccwNdcTriangle[0]),
      sceneNdcToFramebuffer(ccwNdcTriangle[1]),
      sceneNdcToFramebuffer(ccwNdcTriangle[2]),
  }});
  const float shadowFramebufferArea = signedArea({{
      shadowNdcToFramebuffer(ccwNdcTriangle[0]),
      shadowNdcToFramebuffer(ccwNdcTriangle[1]),
      shadowNdcToFramebuffer(ccwNdcTriangle[2]),
  }});

  EXPECT_GT(ndcArea, 0.0f);
  EXPECT_LT(sceneFramebufferArea, 0.0f);
  EXPECT_GT(shadowFramebufferArea, 0.0f);
}

TEST(RenderingConventionTests, FrustumPlaneExtractionUsesSlangRowIndexing) {
  const glm::mat4 view = container::math::lookAt(
      glm::vec3(3.0f, 2.0f, 5.0f),
      glm::vec3(0.0f, 0.0f, 0.0f),
      glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(55.0f), 16.0f / 9.0f, 0.1f, 100.0f);
  const glm::mat4 viewProj = proj * view;

  const FrustumPlanes correctPlanes = extractFrustumPlanesUsingSlangRows(viewProj);
  const FrustumPlanes wrongPlanes =
      extractFrustumPlanesUsingWrongColumnAccess(viewProj);

  int correctMismatchCount = 0;
  int wrongMismatchCount = 0;
  for (int x = -8; x <= 8; x += 2) {
    for (int y = -8; y <= 8; y += 2) {
      for (int z = -8; z <= 8; z += 2) {
        const glm::vec3 point(
            static_cast<float>(x),
            static_cast<float>(y),
            static_cast<float>(z));
        const bool clipInside = pointInsideClipVolume(viewProj, point);
        if (pointInsideFrustum(correctPlanes, point) != clipInside) {
          ++correctMismatchCount;
        }
        if (pointInsideFrustum(wrongPlanes, point) != clipInside) {
          ++wrongMismatchCount;
        }
      }
    }
  }

  EXPECT_EQ(correctMismatchCount, 0);
  EXPECT_GT(wrongMismatchCount, 0);
}

TEST(RenderingConventionTests, OcclusionCullSphereBoundsNeedProjectionScale) {
  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);

  constexpr float kSphereRadius = 1.0f;
  constexpr float kSphereViewDepth = 5.0f;
  const glm::vec4 topPointClip =
      proj * glm::vec4(0.0f, kSphereRadius, -kSphereViewDepth, 1.0f);
  const float actualNdcYExtent = std::abs(clipToNdc(topPointClip).y);

  const float oldProjectedRadius = kSphereRadius / kSphereViewDepth;
  const float conservativeProjectedRadius =
      rowXyzLength(proj, 1) * kSphereRadius / (kSphereViewDepth - kSphereRadius);

  EXPECT_LT(oldProjectedRadius, actualNdcYExtent);
  EXPECT_GE(conservativeProjectedRadius, actualNdcYExtent);
}

TEST(RenderingConventionTests, OcclusionCullSphereDepthUsesNearestPointProjection) {
  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);

  constexpr float kSphereRadius = 2.0f;
  constexpr float kSphereCenterDepth = 10.0f;
  const glm::vec4 centerClip =
      proj * glm::vec4(0.0f, 0.0f, -kSphereCenterDepth, 1.0f);
  const float oldMixedDepth =
      clipToNdc(centerClip).z + (kSphereRadius / centerClip.w);

  const glm::vec4 nearestPointClip =
      proj * glm::vec4(0.0f, 0.0f, -(kSphereCenterDepth - kSphereRadius), 1.0f);
  const float exactClosestDepth = clipToNdc(nearestPointClip).z;

  EXPECT_GT(oldMixedDepth, exactClosestDepth);
  EXPECT_NEAR(exactClosestDepth, clipToNdc(nearestPointClip).z, 1e-6f);
}

TEST(RenderingConventionTests, DefaultSceneUsesCompositeTestSceneToken) {
  const auto config = container::app::DefaultAppConfig();

  EXPECT_EQ(config.modelPath, container::app::kDefaultSceneModelToken);
}

TEST(RenderingConventionTests, DefaultSceneModelListContainsTriangleCubeAndSphere) {
  const auto& modelPaths = container::app::kDefaultSceneModelRelativePaths;

  ASSERT_EQ(modelPaths.size(), 3u);
  EXPECT_NE(modelPaths[0].find("Triangle"), std::string_view::npos);
  EXPECT_NE(modelPaths[1].find("Cube"), std::string_view::npos);
  EXPECT_EQ(modelPaths[2], std::string_view("__procedural_uv_sphere__"));
}
