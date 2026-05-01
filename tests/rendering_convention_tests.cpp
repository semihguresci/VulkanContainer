#include "Container/app/AppConfig.h"
#include "Container/common/CommonMath.h"
#include "Container/renderer/FrameResourceManager.h"
#include "Container/utility/Camera.h"
#include "Container/utility/SceneData.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

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

bool sphereInsideFrustum(const FrustumPlanes& planes,
                         const glm::vec3& center,
                         float radius) {
  for (const glm::vec4& plane : planes) {
    if (glm::dot(glm::vec3(plane), center) + plane.w < -radius) {
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

bool occlusionCullMayRejectProjectedBounds(float maxExtentPixels) {
  constexpr float kLargeProjectedExtentPixels = 96.0f;
  return maxExtentPixels <= kLargeProjectedExtentPixels;
}

uint32_t resolveObjectIndex(uint32_t pushedObjectIndex,
                            uint32_t instanceID,
                            uint32_t startInstance) {
  return pushedObjectIndex == std::numeric_limits<uint32_t>::max()
             ? instanceID + startInstance
             : pushedObjectIndex;
}

float smoothRangeAttenuation(float distanceSq, float radius) {
  if (!std::isfinite(radius) || radius <= 0.0f) {
    return 1.0f;
  }

  const float safeRadius = std::max(radius, 1e-3f);
  const float invRadiusSq = 1.0f / (safeRadius * safeRadius);
  const float normalizedSq = distanceSq * invRadiusSq;
  const float smoothFactor = std::clamp(1.0f - normalizedSq, 0.0f, 1.0f);
  return smoothFactor * smoothFactor;
}

float pointLightAttenuation(float distanceSq, float radius) {
  return smoothRangeAttenuation(distanceSq, radius) / std::max(distanceSq, 0.01f);
}

float encodeGBufferMaterialMetadata(uint32_t materialIndex, bool thinSurface) {
  return static_cast<float>(materialIndex) + (thinSurface ? 0.5f : 0.0f);
}

uint32_t decodeGBufferMaterialIndex(float encodedMetadata) {
  return static_cast<uint32_t>(std::floor(std::max(encodedMetadata, 0.0f)));
}

bool decodeGBufferThinSurface(float encodedMetadata) {
  return encodedMetadata - std::floor(std::max(encodedMetadata, 0.0f)) >= 0.25f;
}

float sceneLinearLuminance(const glm::vec3& color) {
  return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

glm::vec3 applyManualExposure(const glm::vec3& color, float exposure) {
  return color * exposure;
}

float resolvePostProcessExposure(uint32_t mode,
                                 float exposure,
                                 float minExposure,
                                 float maxExposure) {
  if (mode == container::gpu::kExposureModeManual) {
    return exposure;
  }

  return std::clamp(exposure, minExposure, std::max(minExposure, maxExposure));
}

float histogramBinCenterLog2Luminance(uint32_t bin) {
  constexpr float kMinLog2Luminance = -12.0f;
  constexpr float kMaxLog2Luminance = 12.0f;
  constexpr uint32_t kBinCount = 64u;
  const float t =
      (static_cast<float>(bin) - 0.5f) / static_cast<float>(kBinCount - 1u);
  return kMinLog2Luminance + t * (kMaxLog2Luminance - kMinLog2Luminance);
}

float percentileMeteredHistogramLuminance(
    const std::array<uint32_t, 64>& bins,
    float lowPercentile,
    float highPercentile) {
  uint64_t nonBlackCount = 0;
  for (uint32_t bin = 1u; bin < bins.size(); ++bin) {
    nonBlackCount += bins[bin];
  }
  if (nonBlackCount == 0u) {
    return std::exp2(-12.0f);
  }

  const double lowerSample =
      static_cast<double>(nonBlackCount) * lowPercentile;
  const double upperSample =
      static_cast<double>(nonBlackCount) * highPercentile;
  double weighted = 0.0;
  double samples = 0.0;
  uint64_t cumulative = 0;
  for (uint32_t bin = 1u; bin < bins.size(); ++bin) {
    const uint32_t count = bins[bin];
    const double start = static_cast<double>(cumulative);
    cumulative += count;
    const double end = static_cast<double>(cumulative);
    const double included =
        std::max(0.0, std::min(end, upperSample) -
                          std::max(start, lowerSample));
    weighted += static_cast<double>(histogramBinCenterLog2Luminance(bin)) *
                included;
    samples += included;
  }
  return std::exp2(static_cast<float>(weighted / samples));
}

glm::vec3 bloomThresholdFilter(const glm::vec3& color,
                               float threshold,
                               float knee) {
  const float brightness = std::max({color.r, color.g, color.b});
  float soft = brightness - threshold + knee;
  soft = std::clamp(soft, 0.0f, 2.0f * knee);
  soft = soft * soft / (4.0f * knee + 1e-4f);
  float contribution = std::max(soft, brightness - threshold);
  contribution /= std::max(brightness, 1e-4f);
  return color * std::max(contribution, 0.0f);
}

uint32_t selectShadowCascade(float viewDepth,
                             const std::array<float, 4>& cascadeSplits) {
  uint32_t cascadeIndex = 3u;
  for (uint32_t i = 0u; i < 3u; ++i) {
    if (viewDepth < cascadeSplits[i]) {
      cascadeIndex = i;
      break;
    }
  }
  return cascadeIndex;
}

std::filesystem::path repositoryRoot() {
  std::filesystem::path candidate =
      std::filesystem::absolute(__FILE__).parent_path().parent_path();
  if (std::filesystem::exists(candidate / "shaders")) {
    return candidate;
  }

  candidate = std::filesystem::current_path();
  while (!candidate.empty()) {
    if (std::filesystem::exists(candidate / "shaders")) {
      return candidate;
    }
    candidate = candidate.parent_path();
  }

  return std::filesystem::absolute(__FILE__).parent_path().parent_path();
}

std::string readRepoTextFile(const std::filesystem::path& relativePath) {
  const std::filesystem::path path = repositoryRoot() / relativePath;
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << "Unable to open " << path.string();
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
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

TEST(RenderingConventionTests, SceneRasterFrontFaceStaysGltfCounterClockwise) {
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");

  EXPECT_TRUE(contains(pipelineBuilder,
                       "sceneRaster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE"));
  EXPECT_FALSE(contains(pipelineBuilder,
                        "sceneRaster.frontFace   = VK_FRONT_FACE_CLOCKWISE"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "makes direct lighting reject visible surfaces"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "shadowRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE"));
}

TEST(RenderingConventionTests, NormalValidationUsesSceneCullVariants) {
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/PipelineTypes.h");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string debugOverlay =
      readRepoTextFile("src/renderer/DebugOverlayRenderer.cpp");
  const std::string normalValidation =
      readRepoTextFile("shaders/normal_validation.slang");

  EXPECT_TRUE(contains(pipelineTypes, "normalValidationFrontCull"));
  EXPECT_TRUE(contains(pipelineTypes, "normalValidationNoCull"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "normal_validation_front_cull_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "normal_validation_no_cull_pipeline"));
  EXPECT_TRUE(contains(frameRecorder,
                       "p.opaqueWindingFlippedDrawCommands"));
  EXPECT_TRUE(contains(frameRecorder,
                       "normalValidationFrontCullPipeline"));
  EXPECT_TRUE(contains(frameRecorder, "normalValidationNoCullPipeline"));
  EXPECT_TRUE(contains(frameRecorder, "uint32_t faceClassificationFlags"));
  EXPECT_TRUE(contains(frameRecorder, "faceClassificationFlags,"));
  EXPECT_TRUE(contains(frameRecorder, "kNormalValidationBothSidesValid"));
  EXPECT_TRUE(contains(debugOverlay, "pc.faceClassificationFlags"));
  EXPECT_TRUE(contains(normalValidation, "SV_IsFrontFace"));
  EXPECT_TRUE(contains(normalValidation, "pc.faceClassificationFlags"));
  EXPECT_TRUE(contains(normalValidation, "kNormalValidationBothSidesValid"));
  EXPECT_FALSE(contains(normalValidation, "dot(faceNormal, viewDir)"));
}

TEST(RenderingConventionTests, PrimitiveNoCullPropagatesToShaderDoubleSidedFlag) {
  const std::string sceneController =
      readRepoTextFile("src/renderer/SceneController.cpp");
  const std::string gbuffer = readRepoTextFile("shaders/gbuffer.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");
  const std::string depthPrepass =
      readRepoTextFile("shaders/depth_prepass.slang");
  const std::string shadowDepth =
      readRepoTextFile("shaders/shadow_depth.slang");

  EXPECT_TRUE(contains(sceneController, "object.objectInfo.y"));
  EXPECT_TRUE(contains(sceneController,
                       "container::gpu::kObjectFlagDoubleSided"));
  EXPECT_TRUE(contains(gbuffer, "material.flags | obj.objectInfo.y"));
  EXPECT_TRUE(contains(transparent, "material.flags | obj.objectInfo.y"));
  EXPECT_TRUE(contains(depthPrepass, "material.flags | obj.objectInfo.y"));
  EXPECT_TRUE(contains(shadowDepth, "material.flags | obj.objectInfo.y"));
}

TEST(RenderingConventionTests, PerspectiveViewMatrixIgnoresCameraScale) {
  container::scene::PerspectiveCamera camera;
  camera.setPosition({1.0f, 2.0f, 3.0f});
  camera.setYawPitch(90.0f, -10.0f);

  const glm::mat4 viewAtUnitScale = camera.viewMatrix();
  camera.setScale({500.0f, 500.0f, 500.0f});
  const glm::mat4 viewAtLargeScale = camera.viewMatrix();

  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      EXPECT_FLOAT_EQ(viewAtUnitScale[column][row],
                      viewAtLargeScale[column][row]);
    }
  }
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

TEST(RenderingConventionTests, FrustumCullUsesDrawFirstInstanceForObjectBounds) {
  struct Bounds {
    glm::vec3 center{};
    float radius{0.0f};
  };
  struct IndirectDraw {
    uint32_t firstInstance{0};
  };

  const FrustumPlanes planes =
      extractFrustumPlanesUsingSlangRows(glm::mat4(1.0f));
  const std::array<Bounds, 2> objectBounds = {{
      {{5.0f, 0.0f, 0.5f}, 0.1f},
      {{0.0f, 0.0f, 0.5f}, 0.1f},
  }};
  const std::array<IndirectDraw, 1> compactedDrawList = {{{1u}}};

  const uint32_t drawListIndex = 0;
  const uint32_t correctObjectIndex =
      compactedDrawList[drawListIndex].firstInstance;

  EXPECT_FALSE(sphereInsideFrustum(
      planes, objectBounds[drawListIndex].center,
      objectBounds[drawListIndex].radius));
  EXPECT_TRUE(sphereInsideFrustum(
      planes, objectBounds[correctObjectIndex].center,
      objectBounds[correctObjectIndex].radius));
}

TEST(RenderingConventionTests, ShaderObjectIndexUsesPushConstantOrBaseInstance) {
  EXPECT_EQ(resolveObjectIndex(17u, 3u, 100u), 17u);
  EXPECT_EQ(resolveObjectIndex(std::numeric_limits<uint32_t>::max(), 42u, 100u),
            142u);
}

TEST(RenderingConventionTests, DeferredDirectionalLightingFetchesGpuMaterialData) {
  const container::renderer::GBufferFormats formats =
      container::renderer::GBufferFormats::defaults();
  EXPECT_EQ(formats.material, VK_FORMAT_R32G32B32A32_SFLOAT);
  EXPECT_EQ(formats.specular, VK_FORMAT_R16G16B16A16_SFLOAT);

  const std::string materialCommon =
      readRepoTextFile("shaders/material_data_common.slang");
  const std::string gbuffer = readRepoTextFile("shaders/gbuffer.slang");
  const std::string deferred =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string renderGraph =
      readRepoTextFile("src/renderer/RenderGraph.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");

  EXPECT_TRUE(contains(materialCommon, "EncodeGBufferMaterialMetadata"));
  EXPECT_TRUE(contains(materialCommon, "DecodeGBufferMaterialIndex"));
  EXPECT_TRUE(contains(materialCommon, "IsThinSurfaceGpuMaterial"));
  EXPECT_TRUE(contains(gbuffer, "EncodeGBufferMaterialMetadata(materialIndex, thinSurface)"));
  EXPECT_TRUE(contains(gbuffer, "specular : SV_Target4"));
  EXPECT_TRUE(contains(gbuffer, "output.specular = float4(dielectricF0Color, dielectricF0)"));
  EXPECT_TRUE(contains(deferred, "#include \"material_data_common.slang\""));
  EXPECT_TRUE(contains(deferred, "[[vk::binding(17, 0)]] Texture2D<float4> gSpecularTexture"));
  EXPECT_TRUE(contains(deferred,
                       "[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials"));
  EXPECT_TRUE(contains(deferred,
                       "uint materialIndex = DecodeGBufferMaterialIndex(material.w)"));
  EXPECT_TRUE(contains(deferred,
                       "GpuMaterial materialData = uMaterials[materialIndex]"));
  EXPECT_TRUE(contains(deferred,
                       "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_FALSE(contains(deferred, "bool thinSurface = material.w > 0.5"));
  EXPECT_TRUE(contains(renderGraph, "\"GBufferSpecular\""));

  const size_t lightingLayoutPos =
      pipelineBuilder.find("layouts.lighting = pipelineManager_.createPipelineLayout(");
  ASSERT_NE(lightingLayoutPos, std::string::npos);
  const size_t tiledLayoutPos =
      pipelineBuilder.find("layouts.tiledLighting", lightingLayoutPos);
  ASSERT_NE(tiledLayoutPos, std::string::npos);
  const std::string lightingLayoutBlock =
      pipelineBuilder.substr(lightingLayoutPos, tiledLayoutPos - lightingLayoutPos);
  EXPECT_TRUE(contains(lightingLayoutBlock, "descriptorLayouts.lighting"));
  EXPECT_TRUE(contains(lightingLayoutBlock, "descriptorLayouts.light"));
  EXPECT_TRUE(contains(lightingLayoutBlock, "descriptorLayouts.scene"));
  const size_t shadowLayoutPos =
      pipelineBuilder.find("layouts.shadow", tiledLayoutPos);
  ASSERT_NE(shadowLayoutPos, std::string::npos);
  const std::string tiledLayoutBlock =
      pipelineBuilder.substr(tiledLayoutPos, shadowLayoutPos - tiledLayoutPos);
  EXPECT_TRUE(contains(tiledLayoutBlock, "descriptorLayouts.lighting"));
  EXPECT_TRUE(contains(tiledLayoutBlock, "descriptorLayouts.tiled"));
  EXPECT_TRUE(contains(tiledLayoutBlock, "descriptorLayouts.scene"));

  EXPECT_TRUE(contains(frameRecorder,
                       "std::array<VkDescriptorSet, 3> directionalDescriptorSets"));
  EXPECT_TRUE(contains(frameRecorder,
                       "directionalDescriptorSets.data()"));
  EXPECT_TRUE(contains(frameRecorder,
                       "std::array<VkDescriptorSet, 3> pointLightingSets"));
  EXPECT_TRUE(contains(frameRecorder,
                       "std::array<VkDescriptorSet, 3> tiledSets"));
}

TEST(RenderingConventionTests, DeferredMaterialParityUsesSharedLayeredHelpers) {
  const std::string brdfCommon = readRepoTextFile("shaders/brdf_common.slang");
  const std::string directional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string pbrDocs = readRepoTextFile("docs/pbr-implementation.md");

  EXPECT_TRUE(contains(brdfCommon, "EvaluateDeferredPbrF0"));
  EXPECT_TRUE(contains(brdfCommon, "EvaluateDeferredPbrF0FromDielectricColor"));
  EXPECT_TRUE(contains(brdfCommon, "ApproximateDeferredIridescenceTint"));
  EXPECT_TRUE(contains(brdfCommon, "EvaluateDeferredClearcoatDirectLight"));
  EXPECT_TRUE(contains(brdfCommon, "EvaluateDeferredSheenDirectLight"));
  EXPECT_TRUE(contains(brdfCommon, "EvaluateDeferredLayeredDirectLight"));

  EXPECT_TRUE(contains(directional, "gSpecularTexture"));
  EXPECT_TRUE(contains(directional, "EvaluateDeferredPbrF0FromDielectricColor("));
  EXPECT_TRUE(contains(directional, "specularSample.rgb"));
  EXPECT_TRUE(contains(directional, "materialData.iridescenceFactor"));
  EXPECT_TRUE(contains(directional, "materialData.clearcoatFactor"));
  EXPECT_TRUE(contains(directional, "materialData.clearcoatRoughnessFactor"));
  EXPECT_TRUE(contains(directional, "materialData.sheenColorFactor.rgb"));
  EXPECT_TRUE(contains(directional, "materialData.sheenRoughnessFactor"));
  EXPECT_TRUE(contains(directional, "EvaluateDeferredLayeredDirectLight("));

  EXPECT_TRUE(contains(pointLight, "gSpecularTexture"));
  EXPECT_TRUE(contains(pointLight, "EvaluateDeferredPbrF0FromDielectricColor("));
  EXPECT_TRUE(contains(pointLight, "specularSample.rgb"));
  EXPECT_TRUE(contains(pointLight, "EvaluateDeferredLayeredDirectLight("));
  EXPECT_TRUE(contains(pointLight,
                       "[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials"));
  EXPECT_TRUE(contains(pointLight,
                       "GpuMaterial materialData = uMaterials[materialIndex]"));
  EXPECT_TRUE(contains(pointLight, "materialData.iridescenceFactor"));
  EXPECT_TRUE(contains(pointLight, "materialData.clearcoatFactor"));
  EXPECT_TRUE(contains(pointLight, "materialData.sheenColorFactor.rgb"));
  EXPECT_FALSE(contains(pointLight, "0.0, 1.0, 0.0.xxx, 1.0"));
  EXPECT_TRUE(contains(tiledLighting, "gSpecularTexture"));
  EXPECT_TRUE(contains(tiledLighting, "EvaluateDeferredPbrF0FromDielectricColor("));
  EXPECT_TRUE(contains(tiledLighting, "specularSample.rgb"));
  EXPECT_TRUE(contains(tiledLighting, "EvaluateDeferredLayeredDirectLight("));
  EXPECT_TRUE(contains(tiledLighting,
                       "[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials"));
  EXPECT_TRUE(contains(tiledLighting,
                       "GpuMaterial materialData = uMaterials[materialIndex]"));
  EXPECT_TRUE(contains(tiledLighting, "materialData.iridescenceFactor"));
  EXPECT_TRUE(contains(tiledLighting, "materialData.clearcoatFactor"));
  EXPECT_TRUE(contains(tiledLighting, "materialData.sheenColorFactor.rgb"));
  EXPECT_FALSE(contains(tiledLighting, "0.0, 1.0, 0.0.xxx, 1.0"));

  EXPECT_TRUE(contains(pbrDocs,
                       "Directional, point, and tiled deferred lighting fetch `GpuMaterial`"));
  EXPECT_FALSE(contains(pbrDocs,
                        "point and tiled lights use the compact fallback"));
}

TEST(RenderingConventionTests, PointLightAttenuationUsesSmoothedInverseSquareFalloff) {
  constexpr float kLargeRadius = 1000.0f;
  const float nearAttenuation = pointLightAttenuation(4.0f, kLargeRadius);
  const float farAttenuation = pointLightAttenuation(16.0f, kLargeRadius);

  EXPECT_TRUE(std::isfinite(pointLightAttenuation(0.0f, kLargeRadius)));
  EXPECT_GT(nearAttenuation, farAttenuation);
  EXPECT_NEAR(nearAttenuation / farAttenuation, 4.0f, 1e-3f);
  EXPECT_NEAR(pointLightAttenuation(0.25f, kLargeRadius) /
                  pointLightAttenuation(1.0f, kLargeRadius),
              4.0f, 1e-3f);
  EXPECT_NEAR(pointLightAttenuation(4.0f, container::gpu::kUnboundedPointLightRange) /
                  pointLightAttenuation(16.0f, container::gpu::kUnboundedPointLightRange),
              4.0f, 1e-3f);
  EXPECT_FLOAT_EQ(pointLightAttenuation(64.0f, 8.0f), 0.0f);
  EXPECT_FLOAT_EQ(pointLightAttenuation(65.0f, 8.0f), 0.0f);
}

TEST(RenderingConventionTests, PointLightShaderKeepsSharedAttenuationConvention) {
  const std::string brdfCommon = readRepoTextFile("shaders/brdf_common.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");

  EXPECT_NE(brdfCommon.find("float PointLightAttenuation(float distanceSq, float radius)"),
            std::string::npos);
  EXPECT_NE(brdfCommon.find("SmoothRangeAttenuation(distanceSq, radius)"),
            std::string::npos);
  EXPECT_NE(brdfCommon.find("radius <= 0.0"),
            std::string::npos);
  EXPECT_NE(brdfCommon.find("MIN_POINT_LIGHT_DISTANCE_SQ"),
            std::string::npos);
  EXPECT_NE(brdfCommon.find("smoothCutoff / max(distanceSq, MIN_POINT_LIGHT_DISTANCE_SQ)"),
            std::string::npos);
  EXPECT_NE(pointLight.find("PointLightAttenuation(distanceSq, lightRadius)"),
            std::string::npos);
  EXPECT_NE(pointLight.find("PointLightHasFiniteRange(lightRadius)"),
            std::string::npos);
  EXPECT_NE(pointLight.find("pc.colorIntensity.rgb * pc.colorIntensity.a * attenuation"),
            std::string::npos);
}

TEST(RenderingConventionTests, PointLightShadersSupportUnboundedRangeSentinel) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  const std::string tileCull =
      readRepoTextFile("shaders/tile_light_cull.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");
  const std::string stencil =
      readRepoTextFile("shaders/light_stencil.slang");

  EXPECT_TRUE(contains(sceneData, "kUnboundedPointLightRange = 0.0f"));
  EXPECT_TRUE(contains(lightingStructs,
                       "UNBOUNDED_POINT_LIGHT_RANGE = 0.0"));
  EXPECT_TRUE(contains(lightingStructs,
                       "bool PointLightHasFiniteRange(float radius)"));
  EXPECT_TRUE(contains(lightingStructs,
                       "radius > UNBOUNDED_POINT_LIGHT_RANGE"));
  EXPECT_TRUE(contains(tileCull,
                       "!PointLightHasFiniteRange(lightRange)"));
  EXPECT_TRUE(contains(tiledLighting,
                       "PointLightHasFiniteRange(lightRadius)"));
  EXPECT_TRUE(contains(transparent,
                       "PointLightHasFiniteRange(lightRadius)"));
  EXPECT_TRUE(contains(stencil,
                       "PointLightHasFiniteRange(pc.positionRadius.w)"));
}

TEST(RenderingConventionTests, SpotLightsSharePointLightDataWithConeFields) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/LightingManager.cpp");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(sceneData, "kLightTypeSpot = 1.0f"));
  EXPECT_TRUE(contains(sceneData, "directionInnerCos"));
  EXPECT_TRUE(contains(sceneData, "coneOuterCosType"));
  EXPECT_TRUE(contains(lightingStructs, "SpotLightConeAttenuation"));
  EXPECT_TRUE(contains(lightingStructs, "LIGHT_TYPE_SPOT = 1.0"));
  EXPECT_TRUE(contains(sceneManager, "gltfSpotConeCosines"));
  EXPECT_TRUE(contains(sceneManager, "lightDefinition.type == \"spot\""));
  EXPECT_TRUE(contains(sceneManager, "authoredPointLights_.push_back(spotLight)"));
  EXPECT_TRUE(contains(lightingManager, "authoredLight.coneOuterCosType.y >= 0.5f"));
  EXPECT_TRUE(contains(pointLight, "SpotLightConeAttenuation"));
  EXPECT_TRUE(contains(tiledLighting, "SpotLightConeAttenuation"));
  EXPECT_TRUE(contains(transparent, "SpotLightConeAttenuation"));
}

TEST(RenderingConventionTests, AreaLightDataLayoutMatchesShaderContract) {
  using container::gpu::AreaLightData;
  using container::gpu::LightingData;

  EXPECT_EQ(container::gpu::kMaxAreaLights, 256u);
  EXPECT_FLOAT_EQ(container::gpu::kAreaLightTypeRectangle, 0.0f);
  EXPECT_FLOAT_EQ(container::gpu::kAreaLightTypeDisk, 1.0f);

  EXPECT_EQ(sizeof(AreaLightData), 80u);
  EXPECT_EQ(alignof(AreaLightData), 16u);
  EXPECT_EQ(offsetof(AreaLightData, positionRange), 0u);
  EXPECT_EQ(offsetof(AreaLightData, colorIntensity), 16u);
  EXPECT_EQ(offsetof(AreaLightData, directionType), 32u);
  EXPECT_EQ(offsetof(AreaLightData, tangentHalfSize), 48u);
  EXPECT_EQ(offsetof(AreaLightData, bitangentHalfSize), 64u);

  EXPECT_EQ(sizeof(LightingData), 64u);
  EXPECT_EQ(offsetof(LightingData, areaLightCount), 56u);

  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  EXPECT_TRUE(contains(lightingStructs, "MAX_AREA_LIGHTS = 256u"));
  EXPECT_TRUE(contains(lightingStructs, "struct AreaLightData"));
  EXPECT_TRUE(contains(lightingStructs, "float4 positionRange"));
  EXPECT_TRUE(contains(lightingStructs, "float4 colorIntensity"));
  EXPECT_TRUE(contains(lightingStructs, "float4 directionType"));
  EXPECT_TRUE(contains(lightingStructs, "float4 tangentHalfSize"));
  EXPECT_TRUE(contains(lightingStructs, "float4 bitangentHalfSize"));
  EXPECT_TRUE(contains(lightingStructs, "uint areaLightCount"));
}

TEST(RenderingConventionTests, RectangularAndDiskAreaLightsImportToCpuBuffers) {
  const std::string sceneManagerHeader =
      readRepoTextFile("include/Container/utility/SceneManager.h");
  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");
  const std::string lightingManagerHeader =
      readRepoTextFile("include/Container/renderer/LightingManager.h");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/LightingManager.cpp");

  EXPECT_TRUE(contains(sceneManagerHeader, "authoredAreaLights() const"));
  EXPECT_TRUE(contains(sceneManagerHeader,
                       "std::vector<container::gpu::AreaLightData> authoredAreaLights_{}"));
  EXPECT_TRUE(contains(sceneManager, "gltfAreaLightSpec"));
  EXPECT_TRUE(contains(sceneManager, "\"KHR_lights_area\""));
  EXPECT_TRUE(contains(sceneManager, "\"EXT_lights_area\""));
  EXPECT_TRUE(contains(sceneManager, "areaLightTypeFromString"));
  EXPECT_TRUE(contains(sceneManager, "type == \"rect\""));
  EXPECT_TRUE(contains(sceneManager, "type == \"disk\""));
  EXPECT_TRUE(contains(sceneManager, "authoredAreaLights_.push_back(*areaLight)"));
  EXPECT_TRUE(contains(sceneManager, "gltfAreaLightTangent"));
  EXPECT_TRUE(contains(sceneManager, "gltfAreaLightBitangent"));

  EXPECT_TRUE(contains(lightingManagerHeader, "areaLightsSsbo() const"));
  EXPECT_TRUE(contains(lightingManagerHeader, "appendAuthoredAreaLights"));
  EXPECT_TRUE(contains(lightingManagerHeader, "publishAreaLights"));
  EXPECT_TRUE(contains(lightingManagerHeader, "uploadAreaLightSsbo"));
  EXPECT_TRUE(contains(lightingManager,
                       "sceneManager_->authoredAreaLights()"));
  EXPECT_TRUE(contains(lightingManager, "lightingData.areaLightCount"));
  EXPECT_TRUE(contains(lightingManager, "areaLightSsbo_"));
  EXPECT_TRUE(contains(lightingManager, "sizeof(AreaLightData) * kMaxAreaLights"));
  EXPECT_TRUE(contains(lightingManager, "writeLightDescriptorStorageBuffers"));
  EXPECT_TRUE(contains(lightingManager, "dstBinding = 2"));
  EXPECT_TRUE(contains(lightingManager, "uploadAreaLightSsbo()"));
}

TEST(RenderingConventionTests, AreaLightShadersUseSampledIntegration) {
  const std::string shaderCmake = readRepoTextFile("cmake/Shaders.cmake");
  const std::string areaLightCommon =
      readRepoTextFile("shaders/area_light_common.slang");
  const std::string directional =
      readRepoTextFile("shaders/deferred_directional.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(shaderCmake, "area_light_common.slang"));
  EXPECT_TRUE(contains(areaLightCommon,
                       "AREA_LIGHT_INTEGRATION_SAMPLE_COUNT = 4u"));
  EXPECT_TRUE(contains(areaLightCommon, "struct AreaLightFrame"));
  EXPECT_TRUE(contains(areaLightCommon, "BuildAreaLightFrame"));
  EXPECT_TRUE(contains(areaLightCommon, "AreaLightRectSampleOffset"));
  EXPECT_TRUE(contains(areaLightCommon, "AreaLightDiskSampleOffset"));
  EXPECT_TRUE(contains(areaLightCommon, "EvaluateAreaLightSampleRadiance"));
  EXPECT_TRUE(contains(areaLightCommon,
                       "frame.area / (float(AREA_LIGHT_INTEGRATION_SAMPLE_COUNT)"));

  EXPECT_TRUE(contains(directional, "#include \"area_light_common.slang\""));
  EXPECT_TRUE(contains(directional,
                       "sampleIndex < AREA_LIGHT_INTEGRATION_SAMPLE_COUNT"));
  EXPECT_TRUE(contains(directional, "EvaluateAreaLightSampleRadiance"));
  EXPECT_FALSE(contains(directional, "ClosestPointOnAreaLight"));

  EXPECT_TRUE(contains(transparent, "#include \"area_light_common.slang\""));
  EXPECT_TRUE(contains(transparent,
                       "sampleIndex < AREA_LIGHT_INTEGRATION_SAMPLE_COUNT"));
  EXPECT_TRUE(contains(transparent, "EvaluateAreaLightSampleRadiance"));
  EXPECT_FALSE(contains(transparent, "ClosestPointOnAreaLight"));
}

TEST(RenderingConventionTests, DeferredPointLightsUseMaterialFlagsForThinSurfaces) {
  const std::string brdfCommon = readRepoTextFile("shaders/brdf_common.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string directional =
      readRepoTextFile("shaders/deferred_directional.slang");

  EXPECT_TRUE(contains(brdfCommon, "EvaluateThinSurfaceTransmission"));
  EXPECT_TRUE(contains(pointLight, "#include \"material_data_common.slang\""));
  EXPECT_TRUE(contains(pointLight, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_TRUE(contains(pointLight, "EvaluateThinSurfaceTransmission("));
  EXPECT_TRUE(contains(tiledLighting, "#include \"material_data_common.slang\""));
  EXPECT_TRUE(contains(tiledLighting, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_TRUE(contains(tiledLighting, "EvaluateThinSurfaceTransmission("));
  EXPECT_TRUE(contains(directional, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_TRUE(contains(directional, "EvaluateThinSurfaceTransmission("));
}

TEST(RenderingConventionTests, MaterialOverviewLoadsMetadataWithoutFiltering) {
  const std::string postProcess = readRepoTextFile("shaders/post_process.slang");

  EXPECT_TRUE(contains(postProcess, "float2 panelUV = RectUV(screenUV, materialInner);"));
  EXPECT_TRUE(contains(postProcess, "uint2 panelPixel"));
  EXPECT_TRUE(contains(postProcess,
                       "gMaterialTexture.Load(int3(panelPixel, 0))"));
  EXPECT_EQ(postProcess.find("gMaterialTexture.SampleLevel(gBufferSampler, panelUV, 0.0)"),
            std::string::npos);
}

TEST(RenderingConventionTests, GBufferMaterialMetadataRoundTripsWithinCapacity) {
  constexpr uint32_t kLastMaterialIndex =
      container::gpu::kMaterialTextureDescriptorCapacity - 1u;
  static_assert(kLastMaterialIndex <=
                container::gpu::kMaxExactGBufferMaterialMetadataIndex);

  const float opaqueEncoded =
      encodeGBufferMaterialMetadata(kLastMaterialIndex, false);
  const float thinEncoded =
      encodeGBufferMaterialMetadata(kLastMaterialIndex, true);

  EXPECT_EQ(decodeGBufferMaterialIndex(opaqueEncoded), kLastMaterialIndex);
  EXPECT_FALSE(decodeGBufferThinSurface(opaqueEncoded));
  EXPECT_EQ(decodeGBufferMaterialIndex(thinEncoded), kLastMaterialIndex);
  EXPECT_TRUE(decodeGBufferThinSurface(thinEncoded));

  const std::string materialCommon =
      readRepoTextFile("shaders/material_data_common.slang");
  EXPECT_TRUE(contains(materialCommon,
                       "MAX_EXACT_GBUFFER_MATERIAL_INDEX = 8388607u"));
}

TEST(RenderingConventionTests, MaterialTextureSamplersUsePerTextureMetadata) {
  using container::gpu::GpuTextureMetadata;

  EXPECT_EQ(container::gpu::kMaterialSamplerWrapModeCount, 3u);
  EXPECT_EQ(container::gpu::kMaterialSamplerDescriptorCapacity, 9u);
  EXPECT_EQ(container::gpu::kMaterialSamplerWrapRepeat, 0u);
  EXPECT_EQ(container::gpu::kMaterialSamplerWrapClampToEdge, 1u);
  EXPECT_EQ(container::gpu::kMaterialSamplerWrapMirroredRepeat, 2u);
  EXPECT_EQ(sizeof(GpuTextureMetadata), 16u);
  EXPECT_EQ(offsetof(GpuTextureMetadata, samplerIndex), 0u);

  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");
  const std::string materialX =
      readRepoTextFile("src/utility/MaterialXIntegration.cpp");
  const std::string materialCommon =
      readRepoTextFile("shaders/material_data_common.slang");
  const std::string pbrCommon =
      readRepoTextFile("shaders/pbr_material_common.slang");
  const std::string gbuffer = readRepoTextFile("shaders/gbuffer.slang");

  EXPECT_TRUE(contains(materialX, "gltfTextureSamplerIndex"));
  EXPECT_TRUE(contains(materialX, "TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE"));
  EXPECT_TRUE(contains(materialX, "TINYGLTF_TEXTURE_WRAP_MIRRORED_REPEAT"));
  EXPECT_TRUE(contains(materialX, "textureCacheKey"));
  EXPECT_TRUE(contains(materialX, "resource.samplerIndex = samplerIndex"));

  EXPECT_TRUE(contains(sceneManager, "materialSamplerAddressMode"));
  EXPECT_TRUE(contains(sceneManager,
                       "VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE"));
  EXPECT_TRUE(contains(sceneManager,
                       "VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT"));
  EXPECT_TRUE(contains(sceneManager, "uploadTextureMetadataBuffer"));
  EXPECT_TRUE(contains(sceneManager, "dstBinding = 4"));
  EXPECT_TRUE(contains(sceneManager, "dstBinding = 5"));

  EXPECT_TRUE(contains(materialCommon,
                       "MATERIAL_SAMPLER_DESCRIPTOR_COUNT = 9u"));
  EXPECT_TRUE(contains(materialCommon, "struct GpuTextureMetadata"));
  EXPECT_TRUE(contains(pbrCommon, "uTextureMetadata[texIndex].samplerIndex"));
  EXPECT_TRUE(contains(pbrCommon, "materialSamplers[samplerIndex]"));
  EXPECT_TRUE(contains(gbuffer,
                       "SamplerState materialSamplers[MATERIAL_SAMPLER_DESCRIPTOR_COUNT]"));
  EXPECT_TRUE(contains(gbuffer,
                       "StructuredBuffer<GpuTextureMetadata> uTextureMetadata"));
  EXPECT_TRUE(contains(gbuffer,
                       "[[vk::binding(5, 0)]] Texture2D materialTextures"));
  EXPECT_FALSE(contains(gbuffer, "SamplerState baseSampler"));
}

TEST(RenderingConventionTests, IndirectDrawObjectIndexUsesBaseInstance) {
  const std::string objectIndexCommon =
      readRepoTextFile("shaders/object_index_common.slang");
  EXPECT_TRUE(contains(
      objectIndexCommon,
      "ResolveObjectIndex(uint pushedObjectIndex, uint instanceID, uint startInstance)"));
  EXPECT_TRUE(contains(objectIndexCommon, "? instanceID + startInstance"));

  constexpr std::array<std::string_view, 10> kObjectIndexedVertexShaders = {{
      "shaders/depth_prepass.slang",
      "shaders/forward_transparent.slang",
      "shaders/gbuffer.slang",
      "shaders/geometry_debug.slang",
      "shaders/normal_validation.slang",
      "shaders/object_normals.slang",
      "shaders/shadow_depth.slang",
      "shaders/surface_normals.slang",
      "shaders/wireframe_debug.slang",
      "shaders/wireframe_fallback.slang",
  }};

  for (const std::string_view shaderPath : kObjectIndexedVertexShaders) {
    const std::string shader = readRepoTextFile(std::filesystem::path(shaderPath));
    EXPECT_TRUE(contains(shader, "uint startInstance : SV_StartInstanceLocation"))
        << shaderPath;
    EXPECT_TRUE(contains(
        shader, "ResolveObjectIndex(pc.objectIndex, instanceID, startInstance)"))
        << shaderPath;
  }
}

TEST(RenderingConventionTests, GltfPunctualPointLightsImportAsAuthoredLights) {
  const std::string sceneManagerHeader =
      readRepoTextFile("include/Container/utility/SceneManager.h");
  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");
  const std::string lightingManagerHeader =
      readRepoTextFile("include/Container/renderer/LightingManager.h");
  const std::string lightingManager =
      readRepoTextFile("src/renderer/LightingManager.cpp");

  EXPECT_TRUE(contains(sceneManagerHeader, "authoredPointLights() const"));
  EXPECT_TRUE(contains(sceneManagerHeader,
                       "std::vector<container::gpu::PointLightData> authoredPointLights_{}"));
  EXPECT_TRUE(contains(sceneManagerHeader, "struct AuthoredDirectionalLight"));
  EXPECT_TRUE(contains(sceneManagerHeader, "authoredDirectionalLights() const"));
  EXPECT_TRUE(contains(sceneManagerHeader,
                       "std::vector<AuthoredDirectionalLight> authoredDirectionalLights_{}"));
  EXPECT_TRUE(contains(sceneManager, "collectAuthoredPunctualLights()"));
  EXPECT_TRUE(contains(sceneManager, "activeSceneRootNodes(gltfModel_)"));
  EXPECT_TRUE(contains(sceneManager, "node.light"));
  EXPECT_TRUE(contains(sceneManager, "lightDefinition.type == \"point\""));
  EXPECT_TRUE(contains(sceneManager, "lightDefinition.type == \"directional\""));
  EXPECT_TRUE(contains(sceneManager, "gltfDirectionalLightDirection"));
  EXPECT_TRUE(contains(sceneManager,
                       "glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)"));
  EXPECT_TRUE(contains(sceneManager, "lightDefinition.type == \"spot\""));
  EXPECT_TRUE(contains(sceneManager, "gltfSpotConeCosines"));
  EXPECT_TRUE(contains(sceneManager, "gltfPointLightRange"));
  EXPECT_TRUE(contains(sceneManager, "pointLightColorOrDefault"));
  EXPECT_TRUE(contains(sceneManager, "pointLightIntensityOrDefault"));
  EXPECT_TRUE(contains(sceneManager, "static_cast<float>(light.range) *"));
  EXPECT_TRUE(contains(sceneManager, "sanitizeImportScale(importScale)"));
  EXPECT_TRUE(contains(sceneManager,
                       "return container::gpu::kUnboundedPointLightRange"));
  EXPECT_EQ(sceneManager.find("transformDistanceScale(sceneLocalTransform)"),
            std::string::npos);

  EXPECT_TRUE(contains(lightingManagerHeader, "appendAuthoredPointLights"));
  EXPECT_TRUE(contains(lightingManagerHeader, "applyAuthoredDirectionalLight"));
  EXPECT_TRUE(contains(lightingManager, "sceneManager_->authoredPointLights()"));
  EXPECT_TRUE(contains(lightingManager, "sceneManager_->authoredDirectionalLights()"));
  EXPECT_TRUE(contains(lightingManager, "authoredDirectionalLights().front()"));
  EXPECT_TRUE(contains(lightingManager,
                       "glm::vec4(glm::vec3(authoredLight.direction), 0.0f)"));
  EXPECT_TRUE(contains(lightingManager,
                       "lightingData_.directionalDirection = glm::vec4(worldDirection, 0.0f)"));
  EXPECT_TRUE(contains(lightingManager,
                       "lightingData_.directionalColorIntensity = authoredLight.colorIntensity"));
  EXPECT_TRUE(contains(lightingManager, "applyAuthoredDirectionalLight(anchor)"));
  EXPECT_TRUE(contains(lightingManager, "appendAuthoredPointLights(anchor)"));
  EXPECT_TRUE(contains(lightingManager,
                       "!sceneManager_->authoredPointLights().empty()"));
  EXPECT_TRUE(contains(lightingManager, "world_.replacePointLights(pointLightsSsbo_)"));
}

TEST(RenderingConventionTests, PostProcessPushConstantsExposeExposureAndCascadeMetadata) {
  using container::gpu::ExposureHistogramPushConstants;
  using container::gpu::ExposureSettings;
  using container::gpu::ExposureStateData;
  using container::gpu::PostProcessPushConstants;

  EXPECT_EQ(sizeof(ExposureSettings), 32u);
  EXPECT_EQ(offsetof(ExposureSettings, mode), 0u);
  EXPECT_EQ(offsetof(ExposureSettings, manualExposure), 4u);
  EXPECT_EQ(offsetof(ExposureSettings, targetLuminance), 8u);
  EXPECT_EQ(offsetof(ExposureSettings, minExposure), 12u);
  EXPECT_EQ(offsetof(ExposureSettings, maxExposure), 16u);
  EXPECT_EQ(offsetof(ExposureSettings, adaptationRate), 20u);
  EXPECT_EQ(offsetof(ExposureSettings, meteringLowPercentile), 24u);
  EXPECT_EQ(offsetof(ExposureSettings, meteringHighPercentile), 28u);

  EXPECT_EQ(sizeof(PostProcessPushConstants), 76u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, outputMode), 0u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, bloomEnabled), 4u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, bloomIntensity), 8u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, exposure), 12u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, cameraNear), 16u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, cameraFar), 20u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, cascadeSplits), 24u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, tileCountX), 40u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, totalLights), 44u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, depthSliceCount), 48u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, oitEnabled), 52u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, exposureMode), 56u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, targetLuminance), 60u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, minExposure), 64u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, maxExposure), 68u);
  EXPECT_EQ(offsetof(PostProcessPushConstants, adaptationRate), 72u);

  const ExposureSettings exposureSettings{};
  EXPECT_EQ(exposureSettings.mode, container::gpu::kExposureModeManual);
  EXPECT_FLOAT_EQ(exposureSettings.manualExposure, 0.25f);
  EXPECT_FLOAT_EQ(exposureSettings.targetLuminance, 0.18f);
  EXPECT_FLOAT_EQ(exposureSettings.minExposure, 0.03125f);
  EXPECT_FLOAT_EQ(exposureSettings.maxExposure, 8.0f);
  EXPECT_FLOAT_EQ(exposureSettings.adaptationRate, 1.5f);
  EXPECT_FLOAT_EQ(exposureSettings.meteringLowPercentile, 0.50f);
  EXPECT_FLOAT_EQ(exposureSettings.meteringHighPercentile, 0.95f);

  const PostProcessPushConstants pc{};
  EXPECT_FLOAT_EQ(pc.exposure, 0.25f);
  EXPECT_FLOAT_EQ(pc.cameraNear, 0.1f);
  EXPECT_FLOAT_EQ(pc.cameraFar, 100.0f);
  EXPECT_EQ(pc.depthSliceCount, container::gpu::kClusterDepthSlices);
  EXPECT_EQ(std::size(pc.cascadeSplits), container::gpu::kShadowCascadeCount);
  EXPECT_EQ(pc.exposureMode, container::gpu::kExposureModeManual);
  EXPECT_FLOAT_EQ(pc.targetLuminance, 0.18f);
  EXPECT_FLOAT_EQ(pc.minExposure, 0.03125f);
  EXPECT_FLOAT_EQ(pc.maxExposure, 8.0f);
  EXPECT_FLOAT_EQ(pc.adaptationRate, 1.5f);

  EXPECT_EQ(sizeof(ExposureHistogramPushConstants), 32u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, width), 0u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, height), 4u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, binCount), 8u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, minLogLuminance), 12u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, maxLogLuminance), 16u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, pad0), 20u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, pad1), 24u);
  EXPECT_EQ(offsetof(ExposureHistogramPushConstants, pad2), 28u);

  EXPECT_EQ(sizeof(ExposureStateData), 16u);
  EXPECT_EQ(offsetof(ExposureStateData, exposure), 0u);
  EXPECT_EQ(offsetof(ExposureStateData, averageLuminance), 4u);
  EXPECT_EQ(offsetof(ExposureStateData, targetExposure), 8u);
  EXPECT_EQ(offsetof(ExposureStateData, initialized), 12u);

  const ExposureStateData exposureState{};
  EXPECT_FLOAT_EQ(exposureState.exposure, 0.25f);
  EXPECT_FLOAT_EQ(exposureState.averageLuminance, 0.18f);
  EXPECT_FLOAT_EQ(exposureState.targetExposure, 0.25f);
  EXPECT_FLOAT_EQ(exposureState.initialized, 0.0f);
}

TEST(RenderingConventionTests, PostProcessShaderUsesExposurePushConstant) {
  const std::string pushConstants =
      readRepoTextFile("shaders/push_constants_common.slang");
  const std::string postProcess = readRepoTextFile("shaders/post_process.slang");

  EXPECT_NE(pushConstants.find("struct PostProcessPushConstants"),
            std::string::npos);
  EXPECT_NE(pushConstants.find("float exposure;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float cascadeSplits[4];"), std::string::npos);
  EXPECT_NE(pushConstants.find("uint exposureMode;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float targetLuminance;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float minExposure;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float maxExposure;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float adaptationRate;"), std::string::npos);
  EXPECT_NE(pushConstants.find("struct ExposureHistogramPushConstants"),
            std::string::npos);
  EXPECT_NE(pushConstants.find("float minLogLuminance;"),
            std::string::npos);
  EXPECT_NE(pushConstants.find("float maxLogLuminance;"),
            std::string::npos);
  EXPECT_NE(postProcess.find("ConstantBuffer<PostProcessPushConstants> pc"),
            std::string::npos);
  EXPECT_NE(postProcess.find("ResolveExposureForToneMapping"),
            std::string::npos);
  EXPECT_NE(postProcess.find("pc.exposureMode"), std::string::npos);
  EXPECT_NE(postProcess.find("pc.minExposure"), std::string::npos);
  EXPECT_NE(postProcess.find("pc.maxExposure"), std::string::npos);
  EXPECT_NE(postProcess.find("finalHdr * resolvedExposure"),
            std::string::npos);
  EXPECT_NE(postProcess.find("litColor *= resolvedExposure"),
            std::string::npos);
  EXPECT_NE(postProcess.find("overviewCompositedColor * resolvedExposure"),
            std::string::npos);
  EXPECT_EQ(postProcess.find("finalHdr * 0.25"), std::string::npos);
  EXPECT_EQ(postProcess.find("litColor *= 0.25"), std::string::npos);
}

TEST(RenderingConventionTests, ExposureSettingsFlowFromUiToPostProcessPushConstants) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager =
      readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string exposureHeader =
      readRepoTextFile("include/Container/renderer/ExposureManager.h");
  const std::string exposureManager =
      readRepoTextFile("src/renderer/ExposureManager.cpp");
  const std::string exposureHistogram =
      readRepoTextFile("shaders/exposure_histogram.slang");
  const std::string frameResourceManager =
      readRepoTextFile("src/renderer/FrameResourceManager.cpp");
  const std::string renderGraph =
      readRepoTextFile("src/renderer/RenderGraph.cpp");

  EXPECT_TRUE(contains(guiHeader, "const container::gpu::ExposureSettings& exposureSettings()"));
  EXPECT_TRUE(contains(guiHeader, "container::gpu::ExposureSettings exposureSettings_{}"));
  EXPECT_TRUE(contains(guiManager, "\"Exposure Mode\""));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.manualExposure"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.targetLuminance"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.minExposure"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.maxExposure"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.adaptationRate"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.meteringLowPercentile"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.meteringHighPercentile"));
  EXPECT_TRUE(contains(frameRecorder, "sanitizeExposureSettings"));
  EXPECT_TRUE(contains(frameRecorder, "resolvePostProcessExposure"));
  EXPECT_TRUE(contains(frameRecorder, "RenderPassId::ExposureAdaptation"));
  EXPECT_TRUE(contains(frameRecorder, "exposureManager_->dispatch"));
  EXPECT_TRUE(contains(frameRecorder, "exposureManager_->resolvedExposure"));
  EXPECT_TRUE(contains(exposureHeader, "kHistogramBinCount = 64"));
  EXPECT_TRUE(contains(exposureManager, "collectReadback"));
  EXPECT_TRUE(contains(exposureManager, "settings.targetLuminance"));
  EXPECT_TRUE(contains(exposureManager, "settings.meteringLowPercentile"));
  EXPECT_TRUE(contains(exposureManager, "settings.meteringHighPercentile"));
  EXPECT_TRUE(contains(exposureHistogram, "RWStructuredBuffer<uint> luminanceHistogram"));
  EXPECT_TRUE(contains(exposureHistogram, "InterlockedAdd(luminanceHistogram[bin]"));
  EXPECT_TRUE(contains(frameResourceManager, "fallbackExposure.exposure = 0.25f"));
  EXPECT_TRUE(contains(frameResourceManager, "fallbackExposure.initialized = 0.0f"));
  EXPECT_TRUE(contains(renderGraph, "\"ExposureAdaptation\""));
  EXPECT_TRUE(contains(renderGraph, "\"ExposureState\""));
  EXPECT_TRUE(contains(frameRecorder, "ppPc.exposureMode = exposureSettings.mode"));
  EXPECT_TRUE(contains(frameRecorder, "ppPc.targetLuminance = exposureSettings.targetLuminance"));
  EXPECT_TRUE(contains(frameRecorder, "ppPc.minExposure = exposureSettings.minExposure"));
  EXPECT_TRUE(contains(frameRecorder, "ppPc.maxExposure = exposureSettings.maxExposure"));
  EXPECT_TRUE(contains(frameRecorder, "ppPc.adaptationRate = exposureSettings.adaptationRate"));
}

TEST(RenderingConventionTests, AutoExposurePercentileMeteringRejectsOutliers) {
  std::array<uint32_t, 64> bins{};
  bins[20] = 900;
  bins[50] = 100;
  bins[63] = 1;

  const float fullRange = percentileMeteredHistogramLuminance(bins, 0.0f, 1.0f);
  const float percentileWindow =
      percentileMeteredHistogramLuminance(bins, 0.0f, 0.899f);

  EXPECT_LT(percentileWindow, fullRange);
  EXPECT_NEAR(percentileWindow, std::exp2(histogramBinCenterLog2Luminance(20)),
              1e-4f);

  const std::string exposureManager =
      readRepoTextFile("src/renderer/ExposureManager.cpp");
  EXPECT_TRUE(contains(exposureManager, "lowerSample"));
  EXPECT_TRUE(contains(exposureManager, "upperSample"));
  EXPECT_TRUE(contains(exposureManager, "includedSamples"));
}

TEST(RenderingConventionTests, ManualExposureScalesSceneLinearLuminance) {
  const glm::vec3 hdrColor{6.0f, 2.0f, 0.5f};
  const glm::vec3 halfExposure = applyManualExposure(hdrColor, 0.5f);
  const glm::vec3 doubleExposure = applyManualExposure(hdrColor, 2.0f);

  EXPECT_NEAR(sceneLinearLuminance(halfExposure),
              sceneLinearLuminance(hdrColor) * 0.5f, 1e-6f);
  EXPECT_NEAR(sceneLinearLuminance(doubleExposure),
              sceneLinearLuminance(hdrColor) * 2.0f, 1e-6f);
  EXPECT_GT(sceneLinearLuminance(doubleExposure),
            sceneLinearLuminance(halfExposure));

  EXPECT_FLOAT_EQ(resolvePostProcessExposure(
                      container::gpu::kExposureModeManual, 0.25f, 1.0f, 2.0f),
                  0.25f);
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(
                      container::gpu::kExposureModeAuto, 0.25f, 1.0f, 2.0f),
                  1.0f);
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(
                      container::gpu::kExposureModeAuto, 3.0f, 1.0f, 2.0f),
                  2.0f);
}

TEST(RenderingConventionTests, BloomThresholdFilterIsSceneLinear) {
  const glm::vec3 hdrColor{5.0f, 1.25f, 0.5f};
  constexpr float kThreshold = 2.0f;
  constexpr float kKnee = 0.5f;
  constexpr float kScale = 3.0f;

  const glm::vec3 filtered =
      bloomThresholdFilter(hdrColor, kThreshold, kKnee);
  const glm::vec3 scaledFiltered =
      bloomThresholdFilter(hdrColor * kScale, kThreshold * kScale,
                           kKnee * kScale);

  EXPECT_NEAR(scaledFiltered.r, filtered.r * kScale, 1e-4f);
  EXPECT_NEAR(scaledFiltered.g, filtered.g * kScale, 1e-4f);
  EXPECT_NEAR(scaledFiltered.b, filtered.b * kScale, 1e-4f);

  const std::string bloomDownsample =
      readRepoTextFile("shaders/bloom_downsample.slang");
  EXPECT_TRUE(contains(bloomDownsample,
                       "color = ThresholdFilter(color, pc.threshold, pc.knee)"));
  EXPECT_FALSE(contains(bloomDownsample, "pc.exposure"));
}

TEST(RenderingConventionTests, ShadowCascadeMetadataLayoutMatchesShaderContract) {
  using container::gpu::ShadowCascadeCullData;
  using container::gpu::ShadowCascadeData;
  using container::gpu::ShadowCullData;
  using container::gpu::ShadowData;

  EXPECT_EQ(container::gpu::kShadowCascadeCount, 4u);
  EXPECT_EQ(sizeof(ShadowCascadeData), 80u);
  EXPECT_EQ(alignof(ShadowCascadeData), 16u);
  EXPECT_EQ(offsetof(ShadowCascadeData, viewProj), 0u);
  EXPECT_EQ(offsetof(ShadowCascadeData, splitDepth), 64u);
  EXPECT_EQ(offsetof(ShadowCascadeData, texelSize), 68u);
  EXPECT_EQ(offsetof(ShadowCascadeData, worldRadius), 72u);
  EXPECT_EQ(offsetof(ShadowCascadeData, depthRange), 76u);
  EXPECT_EQ(sizeof(ShadowData),
            sizeof(ShadowCascadeData) * container::gpu::kShadowCascadeCount + 32u);
  EXPECT_EQ(offsetof(ShadowData, cascades), 0u);
  EXPECT_EQ(offsetof(ShadowData, biasSettings), 320u);
  EXPECT_EQ(offsetof(ShadowData, filterSettings), 336u);

  EXPECT_EQ(sizeof(ShadowCascadeCullData), 192u);
  EXPECT_EQ(alignof(ShadowCascadeCullData), 16u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, viewProj), 0u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, lightView), 64u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, receiverMinBounds), 128u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, receiverMaxBounds), 144u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, casterMinBounds), 160u);
  EXPECT_EQ(offsetof(ShadowCascadeCullData, casterMaxBounds), 176u);
  EXPECT_EQ(sizeof(ShadowCullData),
            sizeof(ShadowCascadeCullData) * container::gpu::kShadowCascadeCount);
}

TEST(RenderingConventionTests, ShadowSettingsMapToShadowBufferVectors) {
  using container::gpu::ShadowData;
  using container::gpu::ShadowSettings;

  EXPECT_EQ(sizeof(ShadowSettings), 40u);
  EXPECT_EQ(offsetof(ShadowSettings, normalBiasMinTexels), 0u);
  EXPECT_EQ(offsetof(ShadowSettings, normalBiasMaxTexels), 4u);
  EXPECT_EQ(offsetof(ShadowSettings, slopeBiasScale), 8u);
  EXPECT_EQ(offsetof(ShadowSettings, receiverPlaneBiasScale), 12u);
  EXPECT_EQ(offsetof(ShadowSettings, filterRadiusTexels), 16u);
  EXPECT_EQ(offsetof(ShadowSettings, cascadeBlendFraction), 20u);
  EXPECT_EQ(offsetof(ShadowSettings, constantDepthBias), 24u);
  EXPECT_EQ(offsetof(ShadowSettings, maxDepthBias), 28u);
  EXPECT_EQ(offsetof(ShadowSettings, rasterConstantBias), 32u);
  EXPECT_EQ(offsetof(ShadowSettings, rasterSlopeBias), 36u);

  const ShadowSettings settings{};
  const ShadowData shadowData{};
  EXPECT_FLOAT_EQ(shadowData.biasSettings.x, settings.normalBiasMinTexels);
  EXPECT_FLOAT_EQ(shadowData.biasSettings.y, settings.normalBiasMaxTexels);
  EXPECT_FLOAT_EQ(shadowData.biasSettings.z, settings.slopeBiasScale);
  EXPECT_FLOAT_EQ(shadowData.biasSettings.w, settings.receiverPlaneBiasScale);
  EXPECT_FLOAT_EQ(shadowData.filterSettings.x, settings.filterRadiusTexels);
  EXPECT_FLOAT_EQ(shadowData.filterSettings.y, settings.cascadeBlendFraction);
  EXPECT_FLOAT_EQ(shadowData.filterSettings.z, settings.constantDepthBias);
  EXPECT_FLOAT_EQ(shadowData.filterSettings.w, settings.maxDepthBias);
  EXPECT_FLOAT_EQ(settings.rasterConstantBias, -4.0f);
  EXPECT_FLOAT_EQ(settings.rasterSlopeBias, -1.5f);
}

TEST(RenderingConventionTests, ShadowSettingsFlowFromUiThroughShadowUpload) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string shadowManager =
      readRepoTextFile("src/renderer/ShadowManager.cpp");

  EXPECT_TRUE(contains(guiHeader, "const container::gpu::ShadowSettings& shadowSettings() const"));
  EXPECT_TRUE(contains(guiHeader, "container::gpu::ShadowSettings shadowSettings_{}"));
  EXPECT_TRUE(contains(rendererFrontend, "subs_.guiManager->shadowSettings()"));
  EXPECT_TRUE(contains(shadowManager, "const container::gpu::ShadowSettings& shadowSettings"));

  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.normalBiasMinTexels"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.normalBiasMaxTexels"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.slopeBiasScale"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.receiverPlaneBiasScale"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.filterRadiusTexels"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.cascadeBlendFraction"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.constantDepthBias"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.maxDepthBias"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.rasterConstantBias"));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.rasterSlopeBias"));

  EXPECT_TRUE(contains(shadowManager, "shadowData_.biasSettings = glm::vec4("));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.normalBiasMinTexels"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.normalBiasMaxTexels"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.slopeBiasScale"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.receiverPlaneBiasScale"));
  EXPECT_TRUE(contains(shadowManager, "shadowData_.filterSettings = glm::vec4("));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.filterRadiusTexels"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.cascadeBlendFraction"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.constantDepthBias"));
  EXPECT_TRUE(contains(shadowManager, "shadowSettings.maxDepthBias"));
}

TEST(RenderingConventionTests, ShadowRasterDepthBiasIsDynamicFrameSetting) {
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(sceneData, "float rasterConstantBias{-4.0f}"));
  EXPECT_TRUE(contains(sceneData, "float rasterSlopeBias{-1.5f}"));
  EXPECT_TRUE(contains(frameRecorderHeader,
                       "container::gpu::ShadowSettings"));
  EXPECT_TRUE(contains(rendererFrontend, "p.shadowSettings = subs_.guiManager"));
  EXPECT_TRUE(contains(rendererFrontend,
                       ": container::gpu::ShadowSettings{}"));

  EXPECT_TRUE(contains(pipelineBuilder, "VK_DYNAMIC_STATE_DEPTH_BIAS"));
  EXPECT_TRUE(contains(pipelineBuilder, "shadowDynState"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "shadowRaster.depthBiasConstantFactor = 0.0f"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "shadowRaster.depthBiasSlopeFactor    = 0.0f"));
  EXPECT_TRUE(contains(pipelineBuilder, "sdPCI.pDynamicState       = &shadowDynState"));

  EXPECT_TRUE(contains(frameRecorder, "vkCmdSetDepthBias(cmd"));
  EXPECT_TRUE(contains(frameRecorder, "p.shadowSettings.rasterConstantBias"));
  EXPECT_TRUE(contains(frameRecorder, "p.shadowSettings.rasterSlopeBias"));

  EXPECT_TRUE(contains(guiManager, "\"Raster Constant Bias\""));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.rasterConstantBias"));
  EXPECT_TRUE(contains(guiManager, "\"Raster Slope Bias\""));
  EXPECT_TRUE(contains(guiManager, "&shadowSettings_.rasterSlopeBias"));
}

TEST(RenderingConventionTests, ShadowUploadSanitizesBiasAndFilterSettings) {
  const std::string shadowManager =
      readRepoTextFile("src/renderer/ShadowManager.cpp");

  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.normalBiasMinTexels, 0.0f)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.normalBiasMaxTexels, normalBiasMinTexels)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.slopeBiasScale, 0.0f)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.receiverPlaneBiasScale, 0.0f)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.filterRadiusTexels, 0.25f)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::clamp(shadowSettings.cascadeBlendFraction, 0.0f, 0.45f)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.constantDepthBias, 0.0f)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.maxDepthBias, 0.0f)"));
}

TEST(RenderingConventionTests, ShadowCascadeSelectionUsesOrderedSplitDepths) {
  const std::array<float, 4> splits = {5.0f, 15.0f, 45.0f, 100.0f};

  EXPECT_LT(splits[0], splits[1]);
  EXPECT_LT(splits[1], splits[2]);
  EXPECT_LT(splits[2], splits[3]);

  EXPECT_EQ(selectShadowCascade(0.1f, splits), 0u);
  EXPECT_EQ(selectShadowCascade(4.999f, splits), 0u);
  EXPECT_EQ(selectShadowCascade(5.0f, splits), 1u);
  EXPECT_EQ(selectShadowCascade(44.999f, splits), 2u);
  EXPECT_EQ(selectShadowCascade(45.0f, splits), 3u);
  EXPECT_EQ(selectShadowCascade(120.0f, splits), 3u);
}

TEST(RenderingConventionTests, ShadowShaderStructsExposeCascadeSplitMetadata) {
  const std::string lightingStructs =
      readRepoTextFile("shaders/lighting_structs.slang");
  const std::string postProcess = readRepoTextFile("shaders/post_process.slang");
  const std::string shadowCommon = readRepoTextFile("shaders/shadow_common.slang");

  EXPECT_NE(lightingStructs.find("static const uint SHADOW_CASCADE_COUNT = 4u"),
            std::string::npos);
  EXPECT_NE(lightingStructs.find("struct ShadowCascadeData"),
            std::string::npos);
  EXPECT_NE(lightingStructs.find("float splitDepth;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float texelSize;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float worldRadius;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float depthRange;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("ShadowCascadeData cascades[SHADOW_CASCADE_COUNT]"),
            std::string::npos);
  EXPECT_NE(lightingStructs.find("float4 biasSettings;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float4 filterSettings;"), std::string::npos);
  EXPECT_NE(postProcess.find("pc.cascadeSplits[i]"), std::string::npos);
  EXPECT_NE(postProcess.find("pc.cascadeSplits[cascadeIndex]"),
            std::string::npos);
  EXPECT_NE(shadowCommon.find("shadowData.biasSettings"), std::string::npos);
  EXPECT_NE(shadowCommon.find("shadowData.filterSettings"), std::string::npos);
  EXPECT_NE(shadowCommon.find("shadowData.cascades[cascadeIndex].texelSize"),
            std::string::npos);
  EXPECT_NE(postProcess.find("uShadow.filterSettings.y"), std::string::npos);
  EXPECT_NE(postProcess.find("uShadow.cascades[cascadeIndex].texelSize"),
            std::string::npos);
}

TEST(RenderingConventionTests, ShadowDebugViewsUseDataDrivenCascadeMetadata) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string postProcess = readRepoTextFile("shaders/post_process.slang");

  EXPECT_TRUE(contains(guiHeader, "ShadowCascades = 11"));
  EXPECT_TRUE(contains(guiHeader, "ShadowTexelDensity = 13"));
  EXPECT_TRUE(contains(guiManager, "\"Shadow Cascades\""));
  EXPECT_TRUE(contains(guiManager, "\"Shadow Texel Density\""));

  EXPECT_TRUE(contains(postProcess, "if (outputMode == 11u)"));
  EXPECT_TRUE(contains(postProcess, "pc.cascadeSplits[cascadeIndex]"));
  EXPECT_TRUE(contains(postProcess,
                       "float blendFraction = clamp(uShadow.filterSettings.y, 0.0, 0.45)"));
  EXPECT_TRUE(contains(postProcess, "lerp(color, cascadeColors[cascadeIndex + 1], blendIndicator)"));
  EXPECT_FALSE(contains(postProcess, "float blendFraction = 0.1"));
  EXPECT_FALSE(contains(postProcess, "cascadeRange * 0.1"));
  EXPECT_FALSE(contains(postProcess, "cascadeRange * 0.10"));

  EXPECT_TRUE(contains(postProcess, "if (outputMode == 13u)"));
  EXPECT_TRUE(contains(postProcess,
                       "float texelSize = max(uShadow.cascades[cascadeIndex].texelSize"));
  EXPECT_TRUE(contains(postProcess, "float texelsPerMeter = 1.0 / texelSize"));
  EXPECT_TRUE(contains(postProcess, "float density = saturate(log2(max(texelsPerMeter"));
  EXPECT_FALSE(contains(postProcess, "float texelSize = 1.0"));
  EXPECT_FALSE(contains(postProcess, "float density = 1.0"));
  EXPECT_FALSE(contains(postProcess, "float density = 0.0"));
}

TEST(RenderingConventionTests, ShadowShadersUseDataDrivenDepthBiasSettings) {
  const std::string shadowCommon = readRepoTextFile("shaders/shadow_common.slang");
  const std::string shadowDepth = readRepoTextFile("shaders/shadow_depth.slang");

  EXPECT_TRUE(contains(shadowCommon, "float ComputeReceiverPlaneDepthBias"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.biasSettings.w"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.filterSettings.w"));
  EXPECT_TRUE(contains(shadowCommon, "float ComputeSlopeScaledBias"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.filterSettings.z"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.biasSettings.z"));
  EXPECT_TRUE(contains(shadowCommon, "float bias = ComputeSlopeScaledBias"));
  EXPECT_TRUE(contains(shadowCommon, "ComputeReceiverPlaneDepthBias(shadowNDC, shadowData)"));
  EXPECT_TRUE(contains(shadowCommon, "float compareDepth = shadowNDC.z + bias"));

  EXPECT_TRUE(contains(shadowDepth, "ConstantBuffer<ShadowBuffer> uShadow"));
  EXPECT_TRUE(contains(shadowDepth, "uShadow.cascades[pc.cascadeIndex].viewProj"));
  EXPECT_TRUE(contains(shadowDepth, "[[vk::location(5)]] float2 inTexCoord1"));
  EXPECT_TRUE(contains(shadowDepth, "material.baseColorTextureTransform"));
  EXPECT_TRUE(contains(shadowDepth, "ApplyGpuTextureTransform"));
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

TEST(RenderingConventionTests, OcclusionCullKeepsLargeProjectedBoundsVisible) {
  EXPECT_TRUE(occlusionCullMayRejectProjectedBounds(32.0f));
  EXPECT_TRUE(occlusionCullMayRejectProjectedBounds(96.0f));
  EXPECT_FALSE(occlusionCullMayRejectProjectedBounds(96.01f));
  EXPECT_FALSE(occlusionCullMayRejectProjectedBounds(256.0f));
}

TEST(RenderingConventionTests, DefaultConfigUsesRuntimeSponzaScene) {
  const auto config = container::app::DefaultAppConfig();

  EXPECT_EQ(config.modelPath, container::app::kDefaultModelRelativePath);
  EXPECT_NE(config.modelPath, container::app::kDefaultSceneModelToken);
}

TEST(RenderingConventionTests, DefaultSceneModelListContainsTriangleCubeAndSphere) {
  const auto& modelPaths = container::app::kDefaultSceneModelRelativePaths;

  ASSERT_EQ(modelPaths.size(), 3u);
  EXPECT_NE(modelPaths[0].find("Triangle"), std::string_view::npos);
  EXPECT_NE(modelPaths[1].find("Cube"), std::string_view::npos);
  EXPECT_EQ(modelPaths[2], std::string_view("__procedural_uv_sphere__"));
}
