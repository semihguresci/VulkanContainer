#include "Container/app/AppConfig.h"
#include "Container/common/CommonMath.h"
#include "Container/renderer/BimManager.h"
#include "Container/renderer/DebugOverlayRenderer.h"
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

glm::vec3 clipToNdc(const glm::vec4 &clip) { return glm::vec3(clip) / clip.w; }

glm::vec2 sceneUvToNdc(const glm::vec2 &uv) {
  return {uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f};
}

glm::vec2 sceneNdcToUv(const glm::vec2 &ndc) {
  return {ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f};
}

glm::vec2 shadowNdcToUv(const glm::vec2 &ndc) { return ndc * 0.5f + 0.5f; }

glm::vec2 sceneNdcToFramebuffer(const glm::vec2 &ndc) {
  return {(ndc.x + 1.0f) * 0.5f, (1.0f - ndc.y) * 0.5f};
}

glm::vec2 shadowNdcToFramebuffer(const glm::vec2 &ndc) {
  return (ndc + 1.0f) * 0.5f;
}

float linearizeReverseZPerspectiveDepth(float depth, float nearPlane,
                                        float farPlane) {
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

float signedArea(const std::array<glm::vec2, 3> &triangle) {
  const glm::vec2 ab = triangle[1] - triangle[0];
  const glm::vec2 ac = triangle[2] - triangle[0];
  return ab.x * ac.y - ab.y * ac.x;
}

float slangMatrixAt(const glm::mat4 &matrix, int row, int column) {
  return matrix[column][row];
}

glm::vec4 slangMatrixRow(const glm::mat4 &matrix, int row) {
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

FrustumPlanes extractFrustumPlanesUsingSlangRows(const glm::mat4 &matrix) {
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

FrustumPlanes
extractFrustumPlanesUsingWrongColumnAccess(const glm::mat4 &matrix) {
  return {{
      normalizePlane(
          {slangMatrixAt(matrix, 0, 3) + slangMatrixAt(matrix, 0, 0),
           slangMatrixAt(matrix, 1, 3) + slangMatrixAt(matrix, 1, 0),
           slangMatrixAt(matrix, 2, 3) + slangMatrixAt(matrix, 2, 0),
           slangMatrixAt(matrix, 3, 3) + slangMatrixAt(matrix, 3, 0)}),
      normalizePlane(
          {slangMatrixAt(matrix, 0, 3) - slangMatrixAt(matrix, 0, 0),
           slangMatrixAt(matrix, 1, 3) - slangMatrixAt(matrix, 1, 0),
           slangMatrixAt(matrix, 2, 3) - slangMatrixAt(matrix, 2, 0),
           slangMatrixAt(matrix, 3, 3) - slangMatrixAt(matrix, 3, 0)}),
      normalizePlane(
          {slangMatrixAt(matrix, 0, 3) + slangMatrixAt(matrix, 0, 1),
           slangMatrixAt(matrix, 1, 3) + slangMatrixAt(matrix, 1, 1),
           slangMatrixAt(matrix, 2, 3) + slangMatrixAt(matrix, 2, 1),
           slangMatrixAt(matrix, 3, 3) + slangMatrixAt(matrix, 3, 1)}),
      normalizePlane(
          {slangMatrixAt(matrix, 0, 3) - slangMatrixAt(matrix, 0, 1),
           slangMatrixAt(matrix, 1, 3) - slangMatrixAt(matrix, 1, 1),
           slangMatrixAt(matrix, 2, 3) - slangMatrixAt(matrix, 2, 1),
           slangMatrixAt(matrix, 3, 3) - slangMatrixAt(matrix, 3, 1)}),
      normalizePlane({slangMatrixAt(matrix, 0, 2), slangMatrixAt(matrix, 1, 2),
                      slangMatrixAt(matrix, 2, 2),
                      slangMatrixAt(matrix, 3, 2)}),
      normalizePlane(
          {slangMatrixAt(matrix, 0, 3) - slangMatrixAt(matrix, 0, 2),
           slangMatrixAt(matrix, 1, 3) - slangMatrixAt(matrix, 1, 2),
           slangMatrixAt(matrix, 2, 3) - slangMatrixAt(matrix, 2, 2),
           slangMatrixAt(matrix, 3, 3) - slangMatrixAt(matrix, 3, 2)}),
  }};
}

bool pointInsideFrustum(const FrustumPlanes &planes, const glm::vec3 &point) {
  for (const glm::vec4 &plane : planes) {
    if (glm::dot(glm::vec3(plane), point) + plane.w < 0.0f) {
      return false;
    }
  }
  return true;
}

bool sphereInsideFrustum(const FrustumPlanes &planes, const glm::vec3 &center,
                         float radius) {
  for (const glm::vec4 &plane : planes) {
    if (glm::dot(glm::vec3(plane), center) + plane.w < -radius) {
      return false;
    }
  }
  return true;
}

bool pointInsideClipVolume(const glm::mat4 &viewProj, const glm::vec3 &point) {
  const glm::vec4 clip = viewProj * glm::vec4(point, 1.0f);
  if (clip.w <= 0.0f) {
    return false;
  }

  return clip.x >= -clip.w && clip.x <= clip.w && clip.y >= -clip.w &&
         clip.y <= clip.w && clip.z >= 0.0f && clip.z <= clip.w;
}

float rowXyzLength(const glm::mat4 &matrix, int row) {
  const glm::vec4 r = slangMatrixRow(matrix, row);
  return glm::length(glm::vec3(r));
}

bool occlusionCullMayRejectProjectedBounds(float maxExtentPixels) {
  constexpr float kLargeProjectedExtentPixels = 96.0f;
  return maxExtentPixels <= kLargeProjectedExtentPixels;
}

uint32_t resolveObjectIndex(uint32_t pushedObjectIndex, uint32_t instanceID,
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
  return smoothRangeAttenuation(distanceSq, radius) /
         std::max(distanceSq, 0.01f);
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

float sceneLinearLuminance(const glm::vec3 &color) {
  return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

glm::vec3 applyManualExposure(const glm::vec3 &color, float exposure) {
  return color * exposure;
}

float resolvePostProcessExposure(uint32_t mode, float exposure,
                                 float minExposure, float maxExposure) {
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

float percentileMeteredHistogramLuminance(const std::array<uint32_t, 64> &bins,
                                          float lowPercentile,
                                          float highPercentile) {
  uint64_t nonBlackCount = 0;
  for (uint32_t bin = 1u; bin < bins.size(); ++bin) {
    nonBlackCount += bins[bin];
  }
  if (nonBlackCount == 0u) {
    return std::exp2(-12.0f);
  }

  const double lowerSample = static_cast<double>(nonBlackCount) * lowPercentile;
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
    const double included = std::max(0.0, std::min(end, upperSample) -
                                              std::max(start, lowerSample));
    weighted +=
        static_cast<double>(histogramBinCenterLog2Luminance(bin)) * included;
    samples += included;
  }
  return std::exp2(static_cast<float>(weighted / samples));
}

glm::vec3 bloomThresholdFilter(const glm::vec3 &color, float threshold,
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
                             const std::array<float, 4> &cascadeSplits) {
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

std::string readRepoTextFile(const std::filesystem::path &relativePath) {
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

} // namespace

TEST(RenderingConventionTests, PerspectiveReverseZMapsNearAndFarToOneAndZero) {
  constexpr float kNear = 0.1f;
  constexpr float kFar = 100.0f;

  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(60.0f), 16.0f / 9.0f, kNear, kFar);

  const glm::vec3 nearNdc =
      clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kNear, 1.0f));
  const glm::vec3 farNdc = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kFar, 1.0f));

  EXPECT_NEAR(nearNdc.z, 1.0f, 1e-5f);
  EXPECT_NEAR(farNdc.z, 0.0f, 1e-5f);
}

TEST(RenderingConventionTests, OrthoReverseZMapsNearAndFarToOneAndZero) {
  constexpr float kNear = 2.0f;
  constexpr float kFar = 10.0f;

  const glm::mat4 proj =
      container::math::orthoRH_ReverseZ(-4.0f, 4.0f, -3.0f, 3.0f, kNear, kFar);

  const glm::vec3 nearNdc =
      clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kNear, 1.0f));
  const glm::vec3 farNdc = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kFar, 1.0f));

  EXPECT_NEAR(nearNdc.z, 1.0f, 1e-5f);
  EXPECT_NEAR(farNdc.z, 0.0f, 1e-5f);
}

TEST(RenderingConventionTests,
     ReverseZPerspectiveLinearizationRecoversViewDistance) {
  constexpr float kNear = 0.1f;
  constexpr float kFar = 100.0f;
  constexpr float kMid = 12.5f;

  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(60.0f), 16.0f / 9.0f, kNear, kFar);

  const float nearDepth =
      clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kNear, 1.0f)).z;
  const float midDepth = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kMid, 1.0f)).z;
  const float farDepth = clipToNdc(proj * glm::vec4(0.0f, 0.0f, -kFar, 1.0f)).z;

  EXPECT_NEAR(linearizeReverseZPerspectiveDepth(nearDepth, kNear, kFar), kNear,
              1e-5f);
  EXPECT_NEAR(linearizeReverseZPerspectiveDepth(midDepth, kNear, kFar), kMid,
              1e-4f);
  EXPECT_NEAR(linearizeReverseZPerspectiveDepth(farDepth, kNear, kFar), kFar,
              1e-3f);
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

TEST(RenderingConventionTests,
     GlTfOcclusionStrengthInterpolatesFromOneToTexture) {
  constexpr float kSampledOcclusion = 0.35f;

  EXPECT_NEAR(applyOcclusionStrength(kSampledOcclusion, 0.0f), 1.0f, 1e-6f);
  EXPECT_NEAR(applyOcclusionStrength(kSampledOcclusion, 1.0f),
              kSampledOcclusion, 1e-6f);
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

TEST(RenderingConventionTests, SceneViewportRecordingUsesSharedHelper) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string sceneViewport =
      readRepoTextFile("src/renderer/SceneViewport.cpp");
  const std::string deferredPostProcess =
      readRepoTextFile("src/renderer/DeferredRasterPostProcess.cpp");
  const std::string deferredTransformGizmo =
      readRepoTextFile("src/renderer/DeferredRasterTransformGizmo.cpp");

  EXPECT_FALSE(contains(frameRecorderHeader, "setViewportAndScissor"));
  EXPECT_FALSE(
      contains(frameRecorder, "void FrameRecorder::setViewportAndScissor"));
  EXPECT_TRUE(contains(frameRecorder, "recordSceneViewportAndScissor"));
  EXPECT_TRUE(contains(deferredPostProcess, "recordSceneViewportAndScissor"));
  EXPECT_TRUE(
      contains(deferredTransformGizmo, "recordSceneViewportAndScissor"));
  EXPECT_TRUE(contains(sceneViewport, "viewport.height = -static_cast<float>"));
  EXPECT_TRUE(contains(sceneViewport, "vkCmdSetViewport"));
  EXPECT_TRUE(contains(sceneViewport, "vkCmdSetScissor"));
}

TEST(RenderingConventionTests, SceneRasterFrontFaceStaysGltfCounterClockwise) {
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");

  EXPECT_TRUE(
      contains(pipelineBuilder,
               "sceneRaster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE"));
  EXPECT_FALSE(contains(pipelineBuilder,
                        "sceneRaster.frontFace   = VK_FRONT_FACE_CLOCKWISE"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "makes direct lighting reject visible surfaces"));
  EXPECT_TRUE(
      contains(pipelineBuilder,
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
  EXPECT_TRUE(
      contains(pipelineBuilder, "normal_validation_front_cull_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "normal_validation_no_cull_pipeline"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.draws.opaqueWindingFlippedDrawCommands"));
  EXPECT_TRUE(contains(frameRecorder, "normalValidationFrontCullPipeline"));
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

TEST(RenderingConventionTests,
     PrimitiveNoCullPropagatesToShaderDoubleSidedFlag) {
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
  EXPECT_TRUE(
      contains(sceneController, "container::gpu::kObjectFlagDoubleSided"));
  EXPECT_TRUE(contains(gbuffer, "material.flags | obj.objectInfo.y"));
  EXPECT_TRUE(contains(transparent, "material.flags | obj.objectInfo.y"));
  EXPECT_TRUE(contains(depthPrepass, "material.flags | obj.objectInfo.y"));
  EXPECT_TRUE(contains(shadowDepth, "material.flags | obj.objectInfo.y"));
}

TEST(RenderingConventionTests, TransparentOitWritesAreDepthTestedEarly) {
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(pipelineBuilder, "transparentDS = depthPrepassDS"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "transparentDS.depthWriteEnable = VK_FALSE"));
  EXPECT_TRUE(contains(transparent, "[earlydepthstencil]"));
  EXPECT_TRUE(contains(transparent, "InterlockedAdd(nodeCounter[0]"));
  EXPECT_TRUE(contains(transparent,
                       "InterlockedExchange(headPointerImage[pixelCoord]"));
}

TEST(RenderingConventionTests, SamplePickerDiscoversIfc5Examples) {
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string sampleManifest =
      readRepoTextFile("models/sample_model_regressions.json");

  EXPECT_TRUE(
      contains(guiManager, "models/buildingSMART-IFC5-development/examples"));
  EXPECT_TRUE(contains(guiManager, "DiscoverAuxiliarySampleAssets"));
  EXPECT_TRUE(contains(guiManager, "usd::usdgeom::mesh"));
  EXPECT_TRUE(contains(guiManager, "IFCTRIANGULATEDFACESET"));
  EXPECT_TRUE(contains(guiManager, "IFCEXTRUDEDAREASOLID"));

  EXPECT_TRUE(
      contains(sampleManifest, "ifc5_domestic_hot_water_ifcx_renderable"));
  EXPECT_TRUE(
      contains(sampleManifest, "ifc5_pcert_infra_bridge_ifcx_renderable"));
  EXPECT_TRUE(contains(sampleManifest, "ifc5_railway_simple_ifcx_renderable"));
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
  const glm::mat4 view = container::math::lookAt(glm::vec3(3.0f, 2.0f, 5.0f),
                                                 glm::vec3(0.0f, 0.0f, 0.0f),
                                                 glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::mat4 proj = container::math::perspectiveRH_ReverseZ(
      glm::radians(55.0f), 16.0f / 9.0f, 0.1f, 100.0f);
  const glm::mat4 viewProj = proj * view;

  const FrustumPlanes correctPlanes =
      extractFrustumPlanesUsingSlangRows(viewProj);
  const FrustumPlanes wrongPlanes =
      extractFrustumPlanesUsingWrongColumnAccess(viewProj);

  int correctMismatchCount = 0;
  int wrongMismatchCount = 0;
  for (int x = -8; x <= 8; x += 2) {
    for (int y = -8; y <= 8; y += 2) {
      for (int z = -8; z <= 8; z += 2) {
        const glm::vec3 point(static_cast<float>(x), static_cast<float>(y),
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

TEST(RenderingConventionTests,
     FrustumCullUsesDrawFirstInstanceForObjectBounds) {
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

  EXPECT_FALSE(sphereInsideFrustum(planes, objectBounds[drawListIndex].center,
                                   objectBounds[drawListIndex].radius));
  EXPECT_TRUE(sphereInsideFrustum(planes,
                                  objectBounds[correctObjectIndex].center,
                                  objectBounds[correctObjectIndex].radius));
}

TEST(RenderingConventionTests,
     ShaderObjectIndexUsesPushConstantOrBaseInstance) {
  EXPECT_EQ(resolveObjectIndex(17u, 3u, 100u), 17u);
  EXPECT_EQ(resolveObjectIndex(std::numeric_limits<uint32_t>::max(), 42u, 100u),
            142u);
}

TEST(RenderingConventionTests,
     DeferredDirectionalLightingFetchesGpuMaterialData) {
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
  EXPECT_TRUE(contains(
      gbuffer, "EncodeGBufferMaterialMetadata(materialIndex, thinSurface)"));
  EXPECT_TRUE(contains(gbuffer, "specular : SV_Target4"));
  EXPECT_TRUE(contains(
      gbuffer, "output.specular = float4(dielectricF0Color, dielectricF0)"));
  EXPECT_TRUE(contains(deferred, "#include \"material_data_common.slang\""));
  EXPECT_TRUE(contains(
      deferred, "[[vk::binding(17, 0)]] Texture2D<float4> gSpecularTexture"));
  EXPECT_TRUE(contains(
      deferred,
      "[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials"));
  EXPECT_TRUE(contains(
      deferred, "uint materialIndex = DecodeGBufferMaterialIndex(material.w)"));
  EXPECT_TRUE(contains(deferred,
                       "GpuMaterial materialData = uMaterials[materialIndex]"));
  EXPECT_TRUE(
      contains(deferred, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_FALSE(contains(deferred, "bool thinSurface = material.w > 0.5"));
  EXPECT_TRUE(contains(renderGraph, "\"GBufferSpecular\""));

  const size_t lightingLayoutPos = pipelineBuilder.find(
      "layouts.lighting = pipelineManager_.createPipelineLayout(");
  ASSERT_NE(lightingLayoutPos, std::string::npos);
  const size_t tiledLayoutPos =
      pipelineBuilder.find("layouts.tiledLighting", lightingLayoutPos);
  ASSERT_NE(tiledLayoutPos, std::string::npos);
  const std::string lightingLayoutBlock = pipelineBuilder.substr(
      lightingLayoutPos, tiledLayoutPos - lightingLayoutPos);
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

  EXPECT_TRUE(
      contains(frameRecorder,
               "std::array<VkDescriptorSet, 3> directionalDescriptorSets"));
  EXPECT_TRUE(contains(frameRecorder, "directionalDescriptorSets.data()"));
  EXPECT_TRUE(contains(frameRecorder,
                       "std::array<VkDescriptorSet, 3> pointLightingSets"));
  EXPECT_TRUE(
      contains(frameRecorder, "std::array<VkDescriptorSet, 3> tiledSets"));
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
  EXPECT_TRUE(
      contains(directional, "EvaluateDeferredPbrF0FromDielectricColor("));
  EXPECT_TRUE(contains(directional, "specularSample.rgb"));
  EXPECT_TRUE(contains(directional, "materialData.iridescenceFactor"));
  EXPECT_TRUE(contains(directional, "materialData.clearcoatFactor"));
  EXPECT_TRUE(contains(directional, "materialData.clearcoatRoughnessFactor"));
  EXPECT_TRUE(contains(directional, "materialData.sheenColorFactor.rgb"));
  EXPECT_TRUE(contains(directional, "materialData.sheenRoughnessFactor"));
  EXPECT_TRUE(contains(directional, "EvaluateDeferredLayeredDirectLight("));

  EXPECT_TRUE(contains(pointLight, "gSpecularTexture"));
  EXPECT_TRUE(
      contains(pointLight, "EvaluateDeferredPbrF0FromDielectricColor("));
  EXPECT_TRUE(contains(pointLight, "specularSample.rgb"));
  EXPECT_TRUE(contains(pointLight, "EvaluateDeferredLayeredDirectLight("));
  EXPECT_TRUE(contains(
      pointLight,
      "[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials"));
  EXPECT_TRUE(contains(pointLight,
                       "GpuMaterial materialData = uMaterials[materialIndex]"));
  EXPECT_TRUE(contains(pointLight, "materialData.iridescenceFactor"));
  EXPECT_TRUE(contains(pointLight, "materialData.clearcoatFactor"));
  EXPECT_TRUE(contains(pointLight, "materialData.sheenColorFactor.rgb"));
  EXPECT_FALSE(contains(pointLight, "0.0, 1.0, 0.0.xxx, 1.0"));
  EXPECT_TRUE(contains(tiledLighting, "gSpecularTexture"));
  EXPECT_TRUE(
      contains(tiledLighting, "EvaluateDeferredPbrF0FromDielectricColor("));
  EXPECT_TRUE(contains(tiledLighting, "specularSample.rgb"));
  EXPECT_TRUE(contains(tiledLighting, "EvaluateDeferredLayeredDirectLight("));
  EXPECT_TRUE(contains(
      tiledLighting,
      "[[vk::binding(3, 2)]] StructuredBuffer<GpuMaterial> uMaterials"));
  EXPECT_TRUE(contains(tiledLighting,
                       "GpuMaterial materialData = uMaterials[materialIndex]"));
  EXPECT_TRUE(contains(tiledLighting, "materialData.iridescenceFactor"));
  EXPECT_TRUE(contains(tiledLighting, "materialData.clearcoatFactor"));
  EXPECT_TRUE(contains(tiledLighting, "materialData.sheenColorFactor.rgb"));
  EXPECT_FALSE(contains(tiledLighting, "0.0, 1.0, 0.0.xxx, 1.0"));

  EXPECT_TRUE(contains(
      pbrDocs,
      "Directional, point, and tiled deferred lighting fetch `GpuMaterial`"));
  EXPECT_FALSE(
      contains(pbrDocs, "point and tiled lights use the compact fallback"));
}

TEST(RenderingConventionTests,
     PointLightAttenuationUsesSmoothedInverseSquareFalloff) {
  constexpr float kLargeRadius = 1000.0f;
  const float nearAttenuation = pointLightAttenuation(4.0f, kLargeRadius);
  const float farAttenuation = pointLightAttenuation(16.0f, kLargeRadius);

  EXPECT_TRUE(std::isfinite(pointLightAttenuation(0.0f, kLargeRadius)));
  EXPECT_GT(nearAttenuation, farAttenuation);
  EXPECT_NEAR(nearAttenuation / farAttenuation, 4.0f, 1e-3f);
  EXPECT_NEAR(pointLightAttenuation(0.25f, kLargeRadius) /
                  pointLightAttenuation(1.0f, kLargeRadius),
              4.0f, 1e-3f);
  EXPECT_NEAR(
      pointLightAttenuation(4.0f, container::gpu::kUnboundedPointLightRange) /
          pointLightAttenuation(16.0f,
                                container::gpu::kUnboundedPointLightRange),
      4.0f, 1e-3f);
  EXPECT_FLOAT_EQ(pointLightAttenuation(64.0f, 8.0f), 0.0f);
  EXPECT_FLOAT_EQ(pointLightAttenuation(65.0f, 8.0f), 0.0f);
}

TEST(RenderingConventionTests,
     PointLightShaderKeepsSharedAttenuationConvention) {
  const std::string brdfCommon = readRepoTextFile("shaders/brdf_common.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");

  EXPECT_NE(brdfCommon.find(
                "float PointLightAttenuation(float distanceSq, float radius)"),
            std::string::npos);
  EXPECT_NE(brdfCommon.find("SmoothRangeAttenuation(distanceSq, radius)"),
            std::string::npos);
  EXPECT_NE(brdfCommon.find("radius <= 0.0"), std::string::npos);
  EXPECT_NE(brdfCommon.find("MIN_POINT_LIGHT_DISTANCE_SQ"), std::string::npos);
  EXPECT_NE(brdfCommon.find(
                "smoothCutoff / max(distanceSq, MIN_POINT_LIGHT_DISTANCE_SQ)"),
            std::string::npos);
  EXPECT_NE(pointLight.find("PointLightAttenuation(distanceSq, lightRadius)"),
            std::string::npos);
  EXPECT_NE(pointLight.find("PointLightHasFiniteRange(lightRadius)"),
            std::string::npos);
  EXPECT_NE(pointLight.find(
                "pc.colorIntensity.rgb * pc.colorIntensity.a * attenuation"),
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
  const std::string stencil = readRepoTextFile("shaders/light_stencil.slang");

  EXPECT_TRUE(contains(sceneData, "kUnboundedPointLightRange = 0.0f"));
  EXPECT_TRUE(contains(lightingStructs, "UNBOUNDED_POINT_LIGHT_RANGE = 0.0"));
  EXPECT_TRUE(
      contains(lightingStructs, "bool PointLightHasFiniteRange(float radius)"));
  EXPECT_TRUE(
      contains(lightingStructs, "radius > UNBOUNDED_POINT_LIGHT_RANGE"));
  EXPECT_TRUE(contains(tileCull, "!PointLightHasFiniteRange(lightRange)"));
  EXPECT_TRUE(contains(tiledLighting, "PointLightHasFiniteRange(lightRadius)"));
  EXPECT_TRUE(contains(transparent, "PointLightHasFiniteRange(lightRadius)"));
  EXPECT_TRUE(
      contains(stencil, "PointLightHasFiniteRange(pc.positionRadius.w)"));
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
  EXPECT_TRUE(
      contains(sceneManager, "authoredPointLights_.push_back(spotLight)"));
  EXPECT_TRUE(
      contains(lightingManager, "authoredLight.coneOuterCosType.y >= 0.5f"));
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
  EXPECT_TRUE(contains(
      sceneManagerHeader,
      "std::vector<container::gpu::AreaLightData> authoredAreaLights_{}"));
  EXPECT_TRUE(contains(sceneManager, "gltfAreaLightSpec"));
  EXPECT_TRUE(contains(sceneManager, "\"KHR_lights_area\""));
  EXPECT_TRUE(contains(sceneManager, "\"EXT_lights_area\""));
  EXPECT_TRUE(contains(sceneManager, "areaLightTypeFromString"));
  EXPECT_TRUE(contains(sceneManager, "type == \"rect\""));
  EXPECT_TRUE(contains(sceneManager, "type == \"disk\""));
  EXPECT_TRUE(
      contains(sceneManager, "authoredAreaLights_.push_back(*areaLight)"));
  EXPECT_TRUE(contains(sceneManager, "gltfAreaLightTangent"));
  EXPECT_TRUE(contains(sceneManager, "gltfAreaLightBitangent"));

  EXPECT_TRUE(contains(lightingManagerHeader, "areaLightsSsbo() const"));
  EXPECT_TRUE(contains(lightingManagerHeader, "appendAuthoredAreaLights"));
  EXPECT_TRUE(contains(lightingManagerHeader, "publishAreaLights"));
  EXPECT_TRUE(contains(lightingManagerHeader, "uploadAreaLightSsbo"));
  EXPECT_TRUE(contains(lightingManager, "sceneManager_->authoredAreaLights()"));
  EXPECT_TRUE(contains(lightingManager, "lightingData.areaLightCount"));
  EXPECT_TRUE(contains(lightingManager, "areaLightSsbo_"));
  EXPECT_TRUE(
      contains(lightingManager, "sizeof(AreaLightData) * kMaxAreaLights"));
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
  EXPECT_TRUE(
      contains(areaLightCommon, "AREA_LIGHT_INTEGRATION_SAMPLE_COUNT = 4u"));
  EXPECT_TRUE(contains(areaLightCommon, "struct AreaLightFrame"));
  EXPECT_TRUE(contains(areaLightCommon, "BuildAreaLightFrame"));
  EXPECT_TRUE(contains(areaLightCommon, "AreaLightRectSampleOffset"));
  EXPECT_TRUE(contains(areaLightCommon, "AreaLightDiskSampleOffset"));
  EXPECT_TRUE(contains(areaLightCommon, "EvaluateAreaLightSampleRadiance"));
  EXPECT_TRUE(
      contains(areaLightCommon,
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

TEST(RenderingConventionTests,
     DeferredPointLightsUseMaterialFlagsForThinSurfaces) {
  const std::string brdfCommon = readRepoTextFile("shaders/brdf_common.slang");
  const std::string pointLight = readRepoTextFile("shaders/point_light.slang");
  const std::string tiledLighting =
      readRepoTextFile("shaders/tiled_lighting.slang");
  const std::string directional =
      readRepoTextFile("shaders/deferred_directional.slang");

  EXPECT_TRUE(contains(brdfCommon, "EvaluateThinSurfaceTransmission"));
  EXPECT_TRUE(contains(pointLight, "#include \"material_data_common.slang\""));
  EXPECT_TRUE(
      contains(pointLight, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_TRUE(contains(pointLight, "EvaluateThinSurfaceTransmission("));
  EXPECT_TRUE(
      contains(tiledLighting, "#include \"material_data_common.slang\""));
  EXPECT_TRUE(
      contains(tiledLighting, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_TRUE(contains(tiledLighting, "EvaluateThinSurfaceTransmission("));
  EXPECT_TRUE(
      contains(directional, "IsThinSurfaceGpuMaterial(materialData.flags)"));
  EXPECT_TRUE(contains(directional, "EvaluateThinSurfaceTransmission("));
}

TEST(RenderingConventionTests, MaterialOverviewLoadsMetadataWithoutFiltering) {
  const std::string postProcess =
      readRepoTextFile("shaders/post_process.slang");

  EXPECT_TRUE(contains(postProcess, "else if (slot == 3u) panelMode = 3u;"));
  EXPECT_TRUE(contains(
      postProcess,
      "uint2 panelPixel = TexturePixelFromUV(panelUV, width, height);"));
  EXPECT_TRUE(contains(postProcess,
                       "DebugTextureViewColor(panelMode, panelUV, panelPixel"));
  EXPECT_TRUE(
      contains(postProcess, "gMaterialTexture.Load(int3(pixelCoord, 0))"));
  EXPECT_EQ(postProcess.find(
                "gMaterialTexture.SampleLevel(gBufferSampler, panelUV, 0.0)"),
            std::string::npos);
}

TEST(RenderingConventionTests, OverviewViewIncludesAllTextureDebugPanels) {
  const std::string postProcess =
      readRepoTextFile("shaders/post_process.slang");

  EXPECT_TRUE(contains(postProcess, "static const uint OVERVIEW_COLUMNS = 3u"));
  EXPECT_TRUE(contains(postProcess, "static const uint OVERVIEW_ROWS = 4u"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 6u) panelMode = 6u;"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 7u) panelMode = 7u;"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 8u) panelMode = 11u;"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 9u) panelMode = 12u;"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 10u) panelMode = 13u;"));
  EXPECT_TRUE(contains(postProcess, "else if (slot == 11u) panelMode = 14u;"));
  EXPECT_TRUE(contains(postProcess,
                       "DebugTextureViewColor(panelMode, panelUV, panelPixel"));
}

TEST(RenderingConventionTests,
     GBufferMaterialMetadataRoundTripsWithinCapacity) {
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
  EXPECT_TRUE(
      contains(materialCommon, "MAX_EXACT_GBUFFER_MATERIAL_INDEX = 8388607u"));
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
  EXPECT_TRUE(contains(sceneManager, "VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE"));
  EXPECT_TRUE(
      contains(sceneManager, "VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT"));
  EXPECT_TRUE(contains(sceneManager, "uploadTextureMetadataBuffer"));
  EXPECT_TRUE(contains(sceneManager, "dstBinding = 4"));
  EXPECT_TRUE(contains(sceneManager, "dstBinding = 5"));

  EXPECT_TRUE(
      contains(materialCommon, "MATERIAL_SAMPLER_DESCRIPTOR_COUNT = 9u"));
  EXPECT_TRUE(contains(materialCommon, "struct GpuTextureMetadata"));
  EXPECT_TRUE(contains(pbrCommon, "uTextureMetadata[texIndex].samplerIndex"));
  EXPECT_TRUE(contains(pbrCommon, "materialSamplers[samplerIndex]"));
  EXPECT_TRUE(contains(
      gbuffer,
      "SamplerState materialSamplers[MATERIAL_SAMPLER_DESCRIPTOR_COUNT]"));
  EXPECT_TRUE(contains(
      gbuffer, "StructuredBuffer<GpuTextureMetadata> uTextureMetadata"));
  EXPECT_TRUE(
      contains(gbuffer, "[[vk::binding(5, 0)]] Texture2D materialTextures"));
  EXPECT_FALSE(contains(gbuffer, "SamplerState baseSampler"));
}

TEST(RenderingConventionTests, IndirectDrawObjectIndexUsesBaseInstance) {
  const std::string objectIndexCommon =
      readRepoTextFile("shaders/object_index_common.slang");
  EXPECT_TRUE(contains(objectIndexCommon,
                       "ResolveObjectIndex(uint pushedObjectIndex, "
                       "uint instanceID, uint startInstance)"));
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
    const std::string shader =
        readRepoTextFile(std::filesystem::path(shaderPath));
    EXPECT_TRUE(
        contains(shader, "uint startInstance : SV_StartInstanceLocation"))
        << shaderPath;
    EXPECT_TRUE(contains(
        shader,
        "ResolveObjectIndex(pc.objectIndex, instanceID, startInstance)"))
        << shaderPath;
  }
}

TEST(RenderingConventionTests, SectionPlanePushConstantsMatchShaderContracts) {
  using container::gpu::BindlessPushConstants;
  using container::gpu::ShadowPushConstants;
  using container::renderer::NormalValidationPushConstants;
  using container::renderer::SurfaceNormalPushConstants;
  using container::renderer::WireframePushConstants;

  EXPECT_EQ(sizeof(BindlessPushConstants), 32u);
  EXPECT_EQ(offsetof(BindlessPushConstants, objectIndex), 0u);
  EXPECT_EQ(offsetof(BindlessPushConstants, sectionPlaneEnabled), 4u);
  EXPECT_EQ(offsetof(BindlessPushConstants, sectionPlane), 16u);

  EXPECT_EQ(sizeof(ShadowPushConstants), 32u);
  EXPECT_EQ(offsetof(ShadowPushConstants, objectIndex), 0u);
  EXPECT_EQ(offsetof(ShadowPushConstants, cascadeIndex), 4u);
  EXPECT_EQ(offsetof(ShadowPushConstants, sectionPlaneEnabled), 8u);
  EXPECT_EQ(offsetof(ShadowPushConstants, sectionPlane), 16u);

  EXPECT_EQ(sizeof(WireframePushConstants), 64u);
  EXPECT_EQ(offsetof(WireframePushConstants, objectIndex), 0u);
  EXPECT_EQ(offsetof(WireframePushConstants, sectionPlaneEnabled), 4u);
  EXPECT_EQ(offsetof(WireframePushConstants, sectionPlane), 16u);
  EXPECT_EQ(offsetof(WireframePushConstants, colorIntensity), 32u);
  EXPECT_EQ(offsetof(WireframePushConstants, lineWidth), 48u);

  EXPECT_EQ(sizeof(NormalValidationPushConstants), 48u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, objectIndex), 0u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, showFaceFill), 4u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, faceAlpha), 8u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, faceClassificationFlags),
            12u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, sectionPlaneEnabled), 16u);
  EXPECT_EQ(offsetof(NormalValidationPushConstants, sectionPlane), 32u);

  EXPECT_EQ(sizeof(SurfaceNormalPushConstants), 32u);
  EXPECT_EQ(offsetof(SurfaceNormalPushConstants, objectIndex), 0u);
  EXPECT_EQ(offsetof(SurfaceNormalPushConstants, lineLength), 4u);
  EXPECT_EQ(offsetof(SurfaceNormalPushConstants, lineOffset), 8u);
  EXPECT_EQ(offsetof(SurfaceNormalPushConstants, sectionPlaneEnabled), 12u);
  EXPECT_EQ(offsetof(SurfaceNormalPushConstants, sectionPlane), 16u);

  const std::string pushConstants =
      readRepoTextFile("shaders/push_constants_common.slang");
  EXPECT_TRUE(contains(pushConstants, "struct BindlessPushConstants"));
  EXPECT_TRUE(contains(pushConstants, "struct ShadowPushConstants"));
  EXPECT_TRUE(contains(pushConstants, "struct WireframePushConstants"));
  EXPECT_TRUE(contains(pushConstants, "struct NormalValidationPushConstants"));
  EXPECT_TRUE(contains(pushConstants, "struct SurfaceNormalPushConstants"));
  EXPECT_TRUE(contains(pushConstants, "uint sectionPlaneEnabled;"));
  EXPECT_TRUE(contains(pushConstants, "float4 sectionPlane;"));
  EXPECT_TRUE(contains(pushConstants, "bool SectionPlaneClips"));

  constexpr std::array<std::string_view, 11> kSectionClippedShaders = {{
      "shaders/depth_prepass.slang",
      "shaders/forward_transparent.slang",
      "shaders/gbuffer.slang",
      "shaders/geometry_debug.slang",
      "shaders/normal_validation.slang",
      "shaders/object_normals.slang",
      "shaders/shadow_depth.slang",
      "shaders/surface_normals.slang",
      "shaders/transparent_pick.slang",
      "shaders/wireframe_debug.slang",
      "shaders/wireframe_fallback.slang",
  }};

  for (const std::string_view shaderPath : kSectionClippedShaders) {
    const std::string shader =
        readRepoTextFile(std::filesystem::path(shaderPath));
    EXPECT_TRUE(contains(shader, "scene_clip_common.slang")) << shaderPath;
    EXPECT_TRUE(contains(shader, "SceneClipPlanesClip")) << shaderPath;
  }

  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string bimLightingOverlayHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayPlanner.h");
  const std::string bimLightingOverlayPlanner =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayPlanner.cpp");
  const std::string primitivePlanner =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassPlanner.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string sceneController =
      readRepoTextFile("src/renderer/SceneController.cpp");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");
  EXPECT_TRUE(contains(frameRecorder, "syncOverlaySectionPlanePushConstants"));
  EXPECT_TRUE(
      contains(frameRecorder, "pushConstants.wireframe->sectionPlaneEnabled"));
  EXPECT_TRUE(contains(frameRecorder,
                       "pushConstants.normalValidation->sectionPlaneEnabled"));
  EXPECT_TRUE(contains(frameRecorder,
                       "pushConstants.surfaceNormal->sectionPlaneEnabled"));
  EXPECT_TRUE(contains(frameRecorder, "spc.sectionPlaneEnabled"));
  EXPECT_TRUE(contains(pipelineBuilder, "sizeof(WireframePushConstants)"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "sizeof(NormalValidationPushConstants)"));
  EXPECT_TRUE(contains(pipelineBuilder, "sizeof(SurfaceNormalPushConstants)"));
  EXPECT_TRUE(contains(pipelineBuilder, "sizeof(ShadowPushConstants)"));
  EXPECT_TRUE(contains(sceneController, "sectionPlaneClips"));
  EXPECT_TRUE(contains(bimManager, "sectionPlaneClips"));
  EXPECT_TRUE(contains(bimManager, "accumulateSectionCapCrossing"));
  EXPECT_TRUE(contains(bimManager, "intersectRaySectionPlane"));
  EXPECT_TRUE(contains(bimManager, "insideSectionCapBounds"));
  EXPECT_TRUE(contains(bimManager, "sectionCapCrossings & 1u"));
  EXPECT_TRUE(contains(bimManager, "sectionCapCrossesObject"));
  EXPECT_TRUE(
      contains(rendererFrontend, "hoverPickCache_.sectionPlaneEnabled"));
  EXPECT_TRUE(
      contains(rendererFrontend, "depthVisibility_.sectionPlaneEnabled"));
}

TEST(RenderingConventionTests, SectionedSelectionUsesVisibleGpuPickFirst) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const size_t selectStart =
      rendererFrontend.find("void RendererFrontend::selectMeshNodeAtCursor");
  ASSERT_NE(selectStart, std::string::npos);
  const size_t selectEnd = rendererFrontend.find(
      "void RendererFrontend::hoverMeshNodeAtCursor", selectStart);
  ASSERT_NE(selectEnd, std::string::npos);
  const std::string selectBlock =
      rendererFrontend.substr(selectStart, selectEnd - selectStart);

  const size_t sectionPlane =
      selectBlock.find("const bool sectionPlaneEnabled");
  const size_t gpuPick = selectBlock.find("samplePickIdAtCursor");
  ASSERT_NE(sectionPlane, std::string::npos);
  ASSERT_NE(gpuPick, std::string::npos);
  EXPECT_LT(sectionPlane, gpuPick);
  EXPECT_FALSE(contains(selectBlock, "!sectionPlaneEnabled &&"));
  EXPECT_TRUE(
      contains(selectBlock, "const bool hasGpuPick = samplePickIdAtCursor"));
}

TEST(RenderingConventionTests, HoverPickingUsesGpuPickBeforeCpuTraversal) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const size_t hoverStart =
      rendererFrontend.find("void RendererFrontend::hoverMeshNodeAtCursor");
  ASSERT_NE(hoverStart, std::string::npos);
  const size_t hoverEnd = rendererFrontend.find(
      "void RendererFrontend::transformSelectedNodeByDrag", hoverStart);
  ASSERT_NE(hoverEnd, std::string::npos);
  const std::string hoverBlock =
      rendererFrontend.substr(hoverStart, hoverEnd - hoverStart);

  const size_t gpuPick = hoverBlock.find("samplePickIdAtCursor");
  const size_t cpuScenePick = hoverBlock.find("pickRenderableNodeHit");
  const size_t cpuBimPick = hoverBlock.find("pickRenderableObject");
  ASSERT_NE(gpuPick, std::string::npos);
  ASSERT_NE(cpuScenePick, std::string::npos);
  ASSERT_NE(cpuBimPick, std::string::npos);
  EXPECT_LT(gpuPick, cpuScenePick);
  EXPECT_LT(gpuPick, cpuBimPick);
  EXPECT_TRUE(contains(hoverBlock, "GpuPickTarget target"));
  EXPECT_TRUE(contains(hoverBlock, "} else {"));
}

TEST(RenderingConventionTests, TransformGizmoOverlaysAfterOitResolve) {
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/PipelineTypes.h");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string deferredPostProcess =
      readRepoTextFile("src/renderer/DeferredRasterPostProcess.cpp");
  const std::string deferredTransformGizmo =
      readRepoTextFile("src/renderer/DeferredRasterTransformGizmo.cpp");

  EXPECT_TRUE(contains(pipelineTypes, "transformGizmoOverlay"));
  EXPECT_TRUE(contains(pipelineBuilder, "transform_gizmo_overlay_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "tgOverlayPCI.renderPass = renderPasses.postProcess"));
  EXPECT_TRUE(contains(frameRecorder, "recordTransformGizmoOverlay"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.pipeline.pipelines.transformGizmoOverlay"));
  EXPECT_TRUE(contains(frameRecorder, "recordDeferredTransformGizmoOverlay"));
  EXPECT_TRUE(contains(frameRecorder, "recordDeferredTransformGizmoPass"));
  EXPECT_FALSE(contains(frameRecorder, "transformGizmoVertexCount"));
  EXPECT_FALSE(contains(frameRecorder, "updateTransformGizmoPushConstants"));
  EXPECT_TRUE(
      contains(deferredTransformGizmo, "deferredTransformGizmoVertexCount"));
  EXPECT_TRUE(contains(deferredTransformGizmo,
                       "updateDeferredTransformGizmoPushConstants"));
  EXPECT_TRUE(
      contains(deferredTransformGizmo, "recordDeferredTransformGizmoOverlay"));
  EXPECT_TRUE(
      contains(deferredTransformGizmo, "recordDeferredTransformGizmoPass"));
  EXPECT_TRUE(contains(deferredPostProcess,
                       "DeferredPostProcessPassScope::recordFullscreenDraw"));
  EXPECT_TRUE(
      contains(deferredPostProcess, "vkCmdDraw(commandBuffer_, 3, 1, 0, 0);"));

  const size_t postProcessPass =
      frameRecorder.find("void FrameRecorder::recordPostProcessPass");
  ASSERT_NE(postProcessPass, std::string::npos);
  const size_t screenshotPass = frameRecorder.find(
      "void FrameRecorder::recordScreenshotCopy", postProcessPass);
  ASSERT_NE(screenshotPass, std::string::npos);
  const std::string postProcessBlock =
      frameRecorder.substr(postProcessPass, screenshotPass - postProcessPass);
  const size_t postDraw =
      postProcessBlock.find("postProcessPass.recordFullscreenDraw");
  ASSERT_NE(postDraw, std::string::npos);
  const size_t gizmoOverlay =
      postProcessBlock.find("recordTransformGizmoOverlay(cmd, p);", postDraw);
  ASSERT_NE(gizmoOverlay, std::string::npos);
  const size_t imguiRender =
      postProcessBlock.find("guiManager_->render(cmd);", gizmoOverlay);
  ASSERT_NE(imguiRender, std::string::npos);
  EXPECT_LT(postDraw, gizmoOverlay);
  EXPECT_LT(gizmoOverlay, imguiRender);
  EXPECT_FALSE(contains(postProcessBlock, "vkCmdBeginRenderPass"));
  EXPECT_FALSE(contains(postProcessBlock, "vkCmdBindPipeline"));
  EXPECT_FALSE(contains(postProcessBlock, "vkCmdDraw(cmd, 3, 1, 0, 0);"));
}

TEST(RenderingConventionTests, BimFloorPlanOverlayCanBeEnabledInViewer) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/BimManager.h");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/PipelineTypes.h");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string bimLightingOverlayHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayPlanner.h");
  const std::string bimLightingOverlayPlanner =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayPlanner.cpp");
  const std::string primitivePlanner =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassPlanner.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(guiManagerHeader, "BimFloorPlanOverlayState"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimFloorPlanElevationMode"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimFloorPlanOverlayState()"));
  EXPECT_TRUE(contains(guiManager, "Floor plan overlay"));
  EXPECT_TRUE(contains(guiManager, "Floor plan elevation"));
  EXPECT_TRUE(contains(guiManager, "Projected on ground"));
  EXPECT_TRUE(contains(guiManager, "Source elevation"));
  EXPECT_TRUE(contains(guiManager, "Floor plan depth test"));

  EXPECT_TRUE(contains(bimManagerHeader, "floorPlanDrawCommands()"));
  EXPECT_TRUE(contains(bimManagerHeader, "floorPlanGroundDrawCommands()"));
  EXPECT_TRUE(
      contains(bimManagerHeader, "floorPlanSourceElevationDrawCommands()"));
  EXPECT_TRUE(contains(bimManagerHeader, "hasFloorPlanOverlay()"));
  EXPECT_TRUE(contains(bimManager, "appendFloorPlanOverlayGeometry"));
  EXPECT_TRUE(contains(bimManager, "floorPlanGround_.firstIndex"));
  EXPECT_TRUE(contains(bimManager, "floorPlanSourceElevation_.firstIndex"));
  EXPECT_TRUE(contains(bimManager, "appendFloorPlanOverlay(floorPlanGround_)"));
  EXPECT_TRUE(contains(bimManager,
                       "appendFloorPlanOverlay(floorPlanSourceElevation_)"));

  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimFloorPlanOverlayState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "floorPlanDrawCommands"));
  EXPECT_TRUE(contains(pipelineTypes, "bimFloorPlanDepth"));
  EXPECT_TRUE(contains(pipelineTypes, "bimFloorPlanNoDepth"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_floor_plan_depth_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_floor_plan_no_depth_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder,
                       "floorPlanPCI.pInputAssemblyState = &lineAssembly"));

  EXPECT_TRUE(contains(rendererFrontend, "bimFloorPlanOverlayState()"));
  EXPECT_TRUE(contains(rendererFrontend, "floorPlan.elevationMode"));
  EXPECT_TRUE(
      contains(rendererFrontend, "BimFloorPlanElevationMode::SourceElevation"));
  EXPECT_TRUE(contains(rendererFrontend, "floorPlanGroundDrawCommands()"));
  EXPECT_TRUE(
      contains(rendererFrontend, "floorPlanSourceElevationDrawCommands()"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.floorPlan.enabled"));
  EXPECT_TRUE(contains(frameRecorder, "buildBimLightingOverlayPlan"));
  EXPECT_TRUE(contains(frameRecorder, "bimLightingOverlayPlan.floorPlan"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.pipeline.pipelines.bimFloorPlanDepth"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.pipeline.pipelines.bimFloorPlanNoDepth"));
  EXPECT_TRUE(contains(frameRecorder, "p.bim.floorPlan.opacity"));
  EXPECT_TRUE(contains(bimLightingOverlayHeader,
                       "BimLightingFloorPlanOverlayInputs"));
  EXPECT_TRUE(contains(bimLightingOverlayPlanner,
                       "BimLightingOverlayKind::FloorPlan"));
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
  EXPECT_TRUE(contains(
      sceneManagerHeader,
      "std::vector<container::gpu::PointLightData> authoredPointLights_{}"));
  EXPECT_TRUE(contains(sceneManagerHeader, "struct AuthoredDirectionalLight"));
  EXPECT_TRUE(
      contains(sceneManagerHeader, "authoredDirectionalLights() const"));
  EXPECT_TRUE(contains(
      sceneManagerHeader,
      "std::vector<AuthoredDirectionalLight> authoredDirectionalLights_{}"));
  EXPECT_TRUE(contains(sceneManager, "collectAuthoredPunctualLights()"));
  EXPECT_TRUE(contains(sceneManager, "activeSceneRootNodes(gltfModel_)"));
  EXPECT_TRUE(contains(sceneManager, "node.light"));
  EXPECT_TRUE(contains(sceneManager, "lightDefinition.type == \"point\""));
  EXPECT_TRUE(
      contains(sceneManager, "lightDefinition.type == \"directional\""));
  EXPECT_TRUE(contains(sceneManager, "gltfDirectionalLightDirection"));
  EXPECT_TRUE(contains(sceneManager, "glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)"));
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
  EXPECT_TRUE(
      contains(lightingManager, "sceneManager_->authoredPointLights()"));
  EXPECT_TRUE(
      contains(lightingManager, "sceneManager_->authoredDirectionalLights()"));
  EXPECT_TRUE(contains(lightingManager, "authoredDirectionalLights().front()"));
  EXPECT_TRUE(contains(lightingManager,
                       "glm::vec4(glm::vec3(authoredLight.direction), 0.0f)"));
  EXPECT_TRUE(contains(
      lightingManager,
      "lightingData_.directionalDirection = glm::vec4(worldDirection, 0.0f)"));
  EXPECT_TRUE(contains(lightingManager,
                       "lightingData_.directionalColorIntensity = "
                       "authoredLight.colorIntensity"));
  EXPECT_TRUE(
      contains(lightingManager, "applyAuthoredDirectionalLight(anchor)"));
  EXPECT_TRUE(contains(lightingManager, "appendAuthoredPointLights(anchor)"));
  EXPECT_TRUE(contains(lightingManager,
                       "!sceneManager_->authoredPointLights().empty()"));
  EXPECT_TRUE(
      contains(lightingManager, "world_.replacePointLights(pointLightsSsbo_)"));
}

TEST(RenderingConventionTests,
     PostProcessPushConstantsExposeExposureAndCascadeMetadata) {
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
  const std::string postProcess =
      readRepoTextFile("shaders/post_process.slang");

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
  EXPECT_NE(pushConstants.find("float minLogLuminance;"), std::string::npos);
  EXPECT_NE(pushConstants.find("float maxLogLuminance;"), std::string::npos);
  EXPECT_NE(postProcess.find("ConstantBuffer<PostProcessPushConstants> pc"),
            std::string::npos);
  EXPECT_NE(postProcess.find("ResolveExposureForToneMapping"),
            std::string::npos);
  EXPECT_NE(postProcess.find("pc.exposureMode"), std::string::npos);
  EXPECT_NE(postProcess.find("pc.minExposure"), std::string::npos);
  EXPECT_NE(postProcess.find("pc.maxExposure"), std::string::npos);
  EXPECT_NE(postProcess.find("finalHdr * resolvedExposure"), std::string::npos);
  EXPECT_NE(postProcess.find("compositedColor * resolvedExposure"),
            std::string::npos);
  EXPECT_NE(postProcess.find("overviewCompositedColor * resolvedExposure"),
            std::string::npos);
  EXPECT_EQ(postProcess.find("finalHdr * 0.25"), std::string::npos);
  EXPECT_EQ(postProcess.find("litColor *= 0.25"), std::string::npos);
}

TEST(RenderingConventionTests,
     ExposureSettingsFlowFromUiToPostProcessPushConstants) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string deferredPostProcessHeader = readRepoTextFile(
      "include/Container/renderer/DeferredRasterPostProcess.h");
  const std::string deferredPostProcess =
      readRepoTextFile("src/renderer/DeferredRasterPostProcess.cpp");
  const std::string deferredRasterTechnique =
      readRepoTextFile("src/renderer/DeferredRasterTechnique.cpp");
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

  EXPECT_TRUE(contains(
      guiHeader, "const container::gpu::ExposureSettings& exposureSettings()"));
  EXPECT_TRUE(contains(guiHeader,
                       "container::gpu::ExposureSettings exposureSettings_{}"));
  EXPECT_TRUE(contains(guiManager, "\"Exposure Mode\""));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.manualExposure"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.targetLuminance"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.minExposure"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.maxExposure"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.adaptationRate"));
  EXPECT_TRUE(contains(guiManager, "&exposureSettings_.meteringLowPercentile"));
  EXPECT_TRUE(
      contains(guiManager, "&exposureSettings_.meteringHighPercentile"));
  EXPECT_TRUE(contains(frameRecorder, "sanitizeExposureSettings"));
  EXPECT_TRUE(contains(frameRecorder, "resolvePostProcessExposure"));
  EXPECT_FALSE(contains(frameRecorder, "float resolvePostProcessExposure"));
  EXPECT_TRUE(contains(frameRecorder, "buildDeferredPostProcessFrameState"));
  EXPECT_TRUE(contains(frameRecorder, "DeferredPostProcessPassScope"));
  EXPECT_FALSE(contains(frameRecorder, "displayModeRecordsBloom(displayMode)"));
  EXPECT_FALSE(
      contains(frameRecorder, "displayModeRecordsTileCull(displayMode)"));
  EXPECT_TRUE(
      contains(deferredRasterTechnique, "RenderPassId::ExposureAdaptation"));
  EXPECT_TRUE(contains(deferredRasterTechnique, "exposureManager_->dispatch"));
  EXPECT_TRUE(
      contains(deferredPostProcessHeader, "resolvePostProcessExposure"));
  EXPECT_TRUE(contains(deferredPostProcess, "resolvePostProcessExposure"));
  EXPECT_TRUE(contains(frameRecorder, "exposureManager_->resolvedExposure"));
  EXPECT_TRUE(contains(exposureHeader, "kHistogramBinCount = 64"));
  EXPECT_TRUE(contains(exposureManager, "collectReadback"));
  EXPECT_TRUE(contains(exposureManager, "settings.targetLuminance"));
  EXPECT_TRUE(contains(exposureManager, "settings.meteringLowPercentile"));
  EXPECT_TRUE(contains(exposureManager, "settings.meteringHighPercentile"));
  EXPECT_TRUE(contains(exposureHistogram,
                       "RWStructuredBuffer<uint> luminanceHistogram"));
  EXPECT_TRUE(
      contains(exposureHistogram, "InterlockedAdd(luminanceHistogram[bin]"));
  EXPECT_TRUE(
      contains(frameResourceManager, "fallbackExposure.exposure = 0.25f"));
  EXPECT_TRUE(
      contains(frameResourceManager, "fallbackExposure.initialized = 0.0f"));
  EXPECT_TRUE(contains(renderGraph, "\"ExposureAdaptation\""));
  EXPECT_TRUE(contains(renderGraph, "\"ExposureState\""));
  EXPECT_TRUE(contains(deferredPostProcessHeader,
                       "class DeferredPostProcessPassScope"));
  EXPECT_TRUE(contains(deferredPostProcess, "vkCmdBeginRenderPass"));
  EXPECT_TRUE(contains(deferredPostProcess, "vkCmdEndRenderPass"));
  EXPECT_TRUE(contains(deferredPostProcess, "vkCmdPushConstants"));
  EXPECT_TRUE(contains(deferredPostProcess,
                       "displayModeRecordsBloom(inputs_.displayMode)"));
  EXPECT_TRUE(contains(deferredPostProcess,
                       "displayModeRecordsTileCull(inputs_.displayMode)"));
  EXPECT_TRUE(
      contains(deferredPostProcess,
               "pushConstants.exposureMode = inputs.exposureSettings.mode"));
  EXPECT_TRUE(contains(deferredPostProcess,
                       "pushConstants.targetLuminance = "
                       "inputs.exposureSettings.targetLuminance"));
  EXPECT_TRUE(contains(
      deferredPostProcess,
      "pushConstants.minExposure = inputs.exposureSettings.minExposure"));
  EXPECT_TRUE(contains(
      deferredPostProcess,
      "pushConstants.maxExposure = inputs.exposureSettings.maxExposure"));
  EXPECT_TRUE(contains(
      deferredPostProcess,
      "pushConstants.adaptationRate = inputs.exposureSettings.adaptationRate"));
}

TEST(RenderingConventionTests, DeferredRasterLightingUsesFrameStatePlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string deferredLightingHeader =
      readRepoTextFile("include/Container/renderer/DeferredRasterLighting.h");
  const std::string deferredLighting =
      readRepoTextFile("src/renderer/DeferredRasterLighting.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t lightingPass =
      frameRecorder.find("void FrameRecorder::recordLightingPass");
  ASSERT_NE(lightingPass, std::string::npos);
  const size_t transformPass =
      frameRecorder.find("void FrameRecorder::recordTransformGizmoPass",
                         lightingPass);
  ASSERT_NE(transformPass, std::string::npos);
  const std::string lightingBlock =
      frameRecorder.substr(lightingPass, transformPass - lightingPass);

  EXPECT_TRUE(contains(frameRecorder, "DeferredRasterLighting.h"));
  EXPECT_TRUE(contains(lightingBlock, "deferredLightingFrameInputs"));
  EXPECT_TRUE(contains(lightingBlock, "buildDeferredLightingFrameState"));
  EXPECT_TRUE(contains(lightingBlock, "lightingState.transparentOitEnabled"));
  EXPECT_TRUE(contains(lightingBlock, "lightingState.pointLighting.path"));
  EXPECT_TRUE(contains(lightingBlock,
                       "lightingState.surfaceNormalLinesEnabled"));
  EXPECT_TRUE(contains(lightingBlock, "lightingState.lightGizmosEnabled"));
  EXPECT_FALSE(contains(lightingBlock, "guiManager_->wireframeSettings()"));
  EXPECT_FALSE(contains(lightingBlock, "guiManager_->showNormalValidation()"));
  EXPECT_FALSE(contains(lightingBlock, "guiManager_->showGeometryOverlay()"));
  EXPECT_FALSE(
      contains(lightingBlock, "guiManager_->normalValidationSettings()"));
  EXPECT_FALSE(contains(lightingBlock, "const bool useTiled"));
  EXPECT_FALSE(contains(lightingBlock, "kMaxDeferredPointLights"));

  EXPECT_TRUE(contains(deferredLightingHeader, "DeferredPointLightingPath"));
  EXPECT_TRUE(contains(deferredLightingHeader, "DeferredLightingDisplayMode"));
  EXPECT_TRUE(
      contains(deferredLightingHeader, "DeferredLightingWireframeSettings"));
  EXPECT_TRUE(contains(deferredLightingHeader, "DeferredLightingFrameInputs"));
  EXPECT_TRUE(contains(deferredLightingHeader, "DeferredLightingFrameState"));
  EXPECT_TRUE(contains(deferredLighting, "buildDeferredLightingFrameState"));
  EXPECT_TRUE(contains(deferredLighting, "wireframeFullMode"));
  EXPECT_TRUE(contains(deferredLighting, "objectSpaceNormalsEnabled"));
  EXPECT_TRUE(contains(deferredLighting, "pointLighting.path"));
  EXPECT_TRUE(contains(deferredLighting, "transparentOitEnabled"));
  EXPECT_TRUE(contains(deferredLighting,
                       "container::gpu::kMaxDeferredPointLights"));
  EXPECT_EQ(deferredLighting.find("vkCmd"), std::string::npos);
  EXPECT_EQ(deferredLighting.find("GuiManager"), std::string::npos);
  EXPECT_EQ(deferredLighting.find("FrameRecordParams"), std::string::npos);
  EXPECT_TRUE(contains(srcCmake, "renderer/DeferredRasterLighting.cpp"));
  EXPECT_TRUE(contains(testsCmake, "deferred_raster_lighting_tests"));
}

TEST(RenderingConventionTests, ScreenshotCopyRecordingUsesCaptureHelper) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string screenshotRecorder =
      readRepoTextFile("src/renderer/ScreenshotCaptureRecorder.cpp");

  const size_t screenshotPass =
      frameRecorder.find("void FrameRecorder::recordScreenshotCopy");
  ASSERT_NE(screenshotPass, std::string::npos);
  const size_t namespaceEnd =
      frameRecorder.find("} // namespace container::renderer", screenshotPass);
  ASSERT_NE(namespaceEnd, std::string::npos);
  const std::string screenshotBlock =
      frameRecorder.substr(screenshotPass, namespaceEnd - screenshotPass);

  EXPECT_TRUE(contains(screenshotBlock, "recordScreenshotCaptureCopy"));
  EXPECT_FALSE(contains(screenshotBlock, "vkCmdCopyImageToBuffer"));
  EXPECT_FALSE(contains(screenshotBlock, "VK_PIPELINE_STAGE_HOST_BIT"));
  EXPECT_TRUE(contains(screenshotRecorder, "hasScreenshotCopyWork"));
  EXPECT_TRUE(contains(screenshotRecorder, "vkCmdCopyImageToBuffer"));
  EXPECT_TRUE(contains(screenshotRecorder, "VK_PIPELINE_STAGE_HOST_BIT"));
  EXPECT_TRUE(contains(screenshotRecorder, "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR"));
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
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(container::gpu::kExposureModeAuto,
                                             0.25f, 1.0f, 2.0f),
                  1.0f);
  EXPECT_FLOAT_EQ(resolvePostProcessExposure(container::gpu::kExposureModeAuto,
                                             3.0f, 1.0f, 2.0f),
                  2.0f);
}

TEST(RenderingConventionTests, BloomThresholdFilterIsSceneLinear) {
  const glm::vec3 hdrColor{5.0f, 1.25f, 0.5f};
  constexpr float kThreshold = 2.0f;
  constexpr float kKnee = 0.5f;
  constexpr float kScale = 3.0f;

  const glm::vec3 filtered = bloomThresholdFilter(hdrColor, kThreshold, kKnee);
  const glm::vec3 scaledFiltered = bloomThresholdFilter(
      hdrColor * kScale, kThreshold * kScale, kKnee * kScale);

  EXPECT_NEAR(scaledFiltered.r, filtered.r * kScale, 1e-4f);
  EXPECT_NEAR(scaledFiltered.g, filtered.g * kScale, 1e-4f);
  EXPECT_NEAR(scaledFiltered.b, filtered.b * kScale, 1e-4f);

  const std::string bloomDownsample =
      readRepoTextFile("shaders/bloom_downsample.slang");
  EXPECT_TRUE(
      contains(bloomDownsample,
               "color = ThresholdFilter(color, pc.threshold, pc.knee)"));
  EXPECT_FALSE(contains(bloomDownsample, "pc.exposure"));
}

TEST(RenderingConventionTests,
     ShadowCascadeMetadataLayoutMatchesShaderContract) {
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
            sizeof(ShadowCascadeData) * container::gpu::kShadowCascadeCount +
                32u);
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
  EXPECT_EQ(sizeof(ShadowCullData), sizeof(ShadowCascadeCullData) *
                                        container::gpu::kShadowCascadeCount);
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

  EXPECT_TRUE(
      contains(guiHeader,
               "const container::gpu::ShadowSettings& shadowSettings() const"));
  EXPECT_TRUE(
      contains(guiHeader, "container::gpu::ShadowSettings shadowSettings_{}"));
  EXPECT_TRUE(contains(rendererFrontend, "subs_.guiManager->shadowSettings()"));
  EXPECT_TRUE(contains(shadowManager,
                       "const container::gpu::ShadowSettings& shadowSettings"));

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
  EXPECT_TRUE(
      contains(shadowManager, "shadowData_.filterSettings = glm::vec4("));
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
  EXPECT_TRUE(contains(frameRecorderHeader, "container::gpu::ShadowSettings"));
  EXPECT_TRUE(contains(rendererFrontend, "p.shadows.shadowSettings"));
  EXPECT_TRUE(
      contains(rendererFrontend,
               "subs_.guiManager ? subs_.guiManager->shadowSettings()"));
  EXPECT_TRUE(contains(rendererFrontend, ": container::gpu::ShadowSettings{}"));

  EXPECT_TRUE(contains(pipelineBuilder, "VK_DYNAMIC_STATE_DEPTH_BIAS"));
  EXPECT_TRUE(contains(pipelineBuilder, "shadowDynState"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "shadowRaster.depthBiasConstantFactor = 0.0f"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "shadowRaster.depthBiasSlopeFactor    = 0.0f"));
  EXPECT_TRUE(
      contains(pipelineBuilder, "sdPCI.pDynamicState       = &shadowDynState"));

  EXPECT_TRUE(contains(frameRecorder, "vkCmdSetDepthBias(cmd"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.shadows.shadowSettings.rasterConstantBias"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.shadows.shadowSettings.rasterSlopeBias"));

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
  EXPECT_TRUE(contains(
      shadowManager,
      "std::max(shadowSettings.normalBiasMaxTexels, normalBiasMinTexels)"));
  EXPECT_TRUE(
      contains(shadowManager, "std::max(shadowSettings.slopeBiasScale, 0.0f)"));
  EXPECT_TRUE(contains(
      shadowManager, "std::max(shadowSettings.receiverPlaneBiasScale, 0.0f)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.filterRadiusTexels, 0.25f)"));
  EXPECT_TRUE(
      contains(shadowManager,
               "std::clamp(shadowSettings.cascadeBlendFraction, 0.0f, 0.45f)"));
  EXPECT_TRUE(contains(shadowManager,
                       "std::max(shadowSettings.constantDepthBias, 0.0f)"));
  EXPECT_TRUE(
      contains(shadowManager, "std::max(shadowSettings.maxDepthBias, 0.0f)"));
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
  const std::string postProcess =
      readRepoTextFile("shaders/post_process.slang");
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");

  EXPECT_NE(lightingStructs.find("static const uint SHADOW_CASCADE_COUNT = 4u"),
            std::string::npos);
  EXPECT_NE(lightingStructs.find("struct ShadowCascadeData"),
            std::string::npos);
  EXPECT_NE(lightingStructs.find("float splitDepth;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float texelSize;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float worldRadius;"), std::string::npos);
  EXPECT_NE(lightingStructs.find("float depthRange;"), std::string::npos);
  EXPECT_NE(
      lightingStructs.find("ShadowCascadeData cascades[SHADOW_CASCADE_COUNT]"),
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
  const std::string postProcess =
      readRepoTextFile("shaders/post_process.slang");

  EXPECT_TRUE(contains(guiHeader, "ShadowCascades = 11"));
  EXPECT_TRUE(contains(guiHeader, "ShadowTexelDensity = 13"));
  EXPECT_TRUE(contains(guiManager, "\"Shadow Cascades\""));
  EXPECT_TRUE(contains(guiManager, "\"Shadow Texel Density\""));

  EXPECT_TRUE(contains(postProcess, "if (outputMode == 11u)"));
  EXPECT_TRUE(contains(postProcess, "pc.cascadeSplits[cascadeIndex]"));
  EXPECT_TRUE(contains(
      postProcess,
      "float blendFraction = clamp(uShadow.filterSettings.y, 0.0, 0.45)"));
  EXPECT_TRUE(
      contains(postProcess,
               "lerp(color, cascadeColors[cascadeIndex + 1], blendIndicator)"));
  EXPECT_FALSE(contains(postProcess, "float blendFraction = 0.1"));
  EXPECT_FALSE(contains(postProcess, "cascadeRange * 0.1"));
  EXPECT_FALSE(contains(postProcess, "cascadeRange * 0.10"));

  EXPECT_TRUE(contains(postProcess, "if (outputMode == 13u)"));
  EXPECT_TRUE(contains(
      postProcess,
      "float texelSize = max(uShadow.cascades[cascadeIndex].texelSize"));
  EXPECT_TRUE(contains(postProcess, "float texelsPerMeter = 1.0 / texelSize"));
  EXPECT_TRUE(contains(postProcess,
                       "float density = saturate(log2(max(texelsPerMeter"));
  EXPECT_FALSE(contains(postProcess, "float texelSize = 1.0"));
  EXPECT_FALSE(contains(postProcess, "float density = 1.0"));
  EXPECT_FALSE(contains(postProcess, "float density = 0.0"));
}

TEST(RenderingConventionTests, ShadowShadersUseDataDrivenDepthBiasSettings) {
  const std::string shadowCommon =
      readRepoTextFile("shaders/shadow_common.slang");
  const std::string shadowDepth =
      readRepoTextFile("shaders/shadow_depth.slang");

  EXPECT_TRUE(contains(shadowCommon, "float ComputeReceiverPlaneDepthBias"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.biasSettings.w"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.filterSettings.w"));
  EXPECT_TRUE(contains(shadowCommon, "float ComputeSlopeScaledBias"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.filterSettings.z"));
  EXPECT_TRUE(contains(shadowCommon, "shadowData.biasSettings.z"));
  EXPECT_TRUE(contains(shadowCommon, "float bias = ComputeSlopeScaledBias"));
  EXPECT_TRUE(contains(shadowCommon,
                       "ComputeReceiverPlaneDepthBias(shadowNDC, shadowData)"));
  EXPECT_TRUE(
      contains(shadowCommon, "float compareDepth = shadowNDC.z + bias"));

  EXPECT_TRUE(contains(shadowDepth, "ConstantBuffer<ShadowBuffer> uShadow"));
  EXPECT_TRUE(
      contains(shadowDepth, "uShadow.cascades[pc.cascadeIndex].viewProj"));
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
  const float conservativeProjectedRadius = rowXyzLength(proj, 1) *
                                            kSphereRadius /
                                            (kSphereViewDepth - kSphereRadius);

  EXPECT_LT(oldProjectedRadius, actualNdcYExtent);
  EXPECT_GE(conservativeProjectedRadius, actualNdcYExtent);
}

TEST(RenderingConventionTests,
     OcclusionCullSphereDepthUsesNearestPointProjection) {
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
  EXPECT_FALSE(config.enableValidationLayers);
}

TEST(RenderingConventionTests,
     DefaultSceneModelListContainsTriangleCubeAndSphere) {
  const auto &modelPaths = container::app::kDefaultSceneModelRelativePaths;

  ASSERT_EQ(modelPaths.size(), 3u);
  EXPECT_NE(modelPaths[0].find("Triangle"), std::string_view::npos);
  EXPECT_NE(modelPaths[1].find("Cube"), std::string_view::npos);
  EXPECT_EQ(modelPaths[2], std::string_view("__procedural_uv_sphere__"));
}

TEST(RenderingConventionTests,
     BimIdentityLookupSupportsDuplicateStringViewKeys) {
  container::renderer::BimObjectIndexLookup lookup;
  lookup["wall-guid"].push_back(4u);
  lookup["wall-guid"].push_back(9u);
  lookup["storey/source/path"].push_back(12u);

  const std::string_view guid = "wall-guid";
  const auto guidIt = lookup.find(guid);
  ASSERT_NE(guidIt, lookup.end());
  ASSERT_EQ(guidIt->second.size(), 2u);
  EXPECT_EQ(guidIt->second[0], 4u);
  EXPECT_EQ(guidIt->second[1], 9u);

  const auto literalGuidIt = lookup.find("wall-guid");
  ASSERT_NE(literalGuidIt, lookup.end());
  EXPECT_EQ(literalGuidIt->second.size(), 2u);

  const auto sourceIt = lookup.find(std::string_view("storey/source/path"));
  ASSERT_NE(sourceIt, lookup.end());
  ASSERT_EQ(sourceIt->second.size(), 1u);
  EXPECT_EQ(sourceIt->second[0], 12u);

  EXPECT_EQ(lookup.find(std::string_view("missing-guid")), lookup.end());
}

TEST(RenderingConventionTests, BimDrawFilterActivationTracksTypeAndSelection) {
  container::renderer::BimDrawFilter filter;

  EXPECT_FALSE(filter.active());

  filter.typeFilterEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.type = "IfcWall";
  EXPECT_TRUE(filter.active());

  filter.typeFilterEnabled = false;
  filter.type.clear();
  EXPECT_FALSE(filter.active());

  filter.storeyFilterEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.storey = "Level 01";
  EXPECT_TRUE(filter.active());

  filter.storeyFilterEnabled = false;
  filter.storey.clear();
  EXPECT_FALSE(filter.active());

  filter.materialFilterEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.material = "Concrete";
  EXPECT_TRUE(filter.active());

  filter.materialFilterEnabled = false;
  filter.material.clear();
  EXPECT_FALSE(filter.active());

  filter.phaseFilterEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.phase = "New construction";
  EXPECT_TRUE(filter.active());

  filter.phaseFilterEnabled = false;
  filter.phase.clear();
  EXPECT_FALSE(filter.active());

  filter.statusFilterEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.status = "Existing";
  EXPECT_TRUE(filter.active());

  filter.statusFilterEnabled = false;
  filter.status.clear();
  EXPECT_FALSE(filter.active());

  filter.drawBudgetEnabled = true;
  EXPECT_FALSE(filter.active());

  filter.drawBudgetMaxObjects = 1000u;
  EXPECT_TRUE(filter.active());

  filter.drawBudgetEnabled = false;
  filter.drawBudgetMaxObjects = 0u;
  EXPECT_FALSE(filter.active());

  filter.isolateSelection = true;
  EXPECT_FALSE(filter.active());

  filter.selectedObjectIndex = 7u;
  EXPECT_TRUE(filter.active());

  filter.isolateSelection = false;
  filter.hideSelection = true;
  EXPECT_TRUE(filter.active());

  filter.selectedObjectIndex = std::numeric_limits<uint32_t>::max();
  EXPECT_FALSE(filter.active());
}

TEST(RenderingConventionTests, BimStoreyMaterialAndHideFiltersArePlumbed) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/BimManager.h");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(bimManagerHeader, "storeyFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "materialFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "disciplineFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "phaseFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "fireRatingFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "loadBearingFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "statusFilterEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawBudgetEnabled"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawBudgetMaxObjects"));
  EXPECT_TRUE(contains(bimManagerHeader, "hideSelection"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementStoreys()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementMaterials()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementDisciplines()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementPhases()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementFireRatings()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementLoadBearingValues()"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementStatuses()"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementStoreys"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementMaterials"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementDisciplines"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementPhases"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementFireRatings"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementLoadBearingValues"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementStatuses"));

  EXPECT_TRUE(contains(bimManager, "bimMetadataStoreyLabel"));
  EXPECT_TRUE(contains(bimManager, "bimMetadataMaterialLabel"));
  EXPECT_TRUE(contains(bimManager, "sameBimProductIdentity(*selectedMetadata"));
  EXPECT_TRUE(contains(bimManager, "filter.storeyFilterEnabled"));
  EXPECT_TRUE(contains(bimManager, "filter.materialFilterEnabled"));
  EXPECT_TRUE(contains(bimManager, "filter.disciplineFilterEnabled"));
  EXPECT_TRUE(contains(bimManager, "filter.phaseFilterEnabled"));
  EXPECT_TRUE(contains(bimManager, "filter.fireRatingFilterEnabled"));
  EXPECT_TRUE(contains(bimManager, "filter.loadBearingFilterEnabled"));
  EXPECT_TRUE(contains(bimManager, "filter.statusFilterEnabled"));
  EXPECT_TRUE(contains(bimManager, "filter.drawBudgetEnabled"));
  EXPECT_TRUE(contains(bimManager, "filter.drawBudgetMaxObjects"));
  EXPECT_TRUE(contains(bimManager, "elementStoreys_.push_back"));
  EXPECT_TRUE(contains(bimManager, "elementMaterials_.push_back"));
  EXPECT_TRUE(contains(bimManager, "elementDisciplines_.push_back"));
  EXPECT_TRUE(contains(bimManager, "elementPhases_.push_back"));
  EXPECT_TRUE(contains(bimManager, "elementFireRatings_.push_back"));
  EXPECT_TRUE(contains(bimManager, "elementLoadBearingValues_.push_back"));
  EXPECT_TRUE(contains(bimManager, "elementStatuses_.push_back"));

  EXPECT_TRUE(contains(rendererFrontend, "filter.storeyFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.materialFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.disciplineFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.phaseFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.fireRatingFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.loadBearingFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.statusFilterEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.drawBudgetEnabled"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.drawBudgetMaxObjects"));
  EXPECT_TRUE(contains(rendererFrontend, "filter.hideSelection"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementStoreys"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementMaterials"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementDisciplines"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementPhases"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementFireRatings"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.elementLoadBearingValues"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementStatuses"));

  EXPECT_TRUE(contains(guiManager, "Storey filter"));
  EXPECT_TRUE(contains(guiManager, "Material filter"));
  EXPECT_TRUE(contains(guiManager, "Discipline filter"));
  EXPECT_TRUE(contains(guiManager, "Phase filter"));
  EXPECT_TRUE(contains(guiManager, "Fire rating filter"));
  EXPECT_TRUE(contains(guiManager, "Load-bearing filter"));
  EXPECT_TRUE(contains(guiManager, "Status filter"));
  EXPECT_TRUE(contains(guiManager, "Draw budget enabled"));
  EXPECT_TRUE(contains(guiManager, "Hide selected"));
}

TEST(RenderingConventionTests, BimSemanticColorModesAreBimScoped) {
  const std::string semanticModeHeader =
      readRepoTextFile("include/Container/renderer/BimSemanticColorMode.h");
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/BimManager.h");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string pushConstants =
      readRepoTextFile("shaders/push_constants_common.slang");
  const std::string materialCommon =
      readRepoTextFile("shaders/material_data_common.slang");
  const std::string gbuffer = readRepoTextFile("shaders/gbuffer.slang");
  const std::string transparent =
      readRepoTextFile("shaders/forward_transparent.slang");

  EXPECT_TRUE(contains(semanticModeHeader, "BimSemanticColorMode"));
  EXPECT_TRUE(contains(semanticModeHeader, "Type"));
  EXPECT_TRUE(contains(semanticModeHeader, "Storey"));
  EXPECT_TRUE(contains(semanticModeHeader, "Material"));
  EXPECT_TRUE(contains(semanticModeHeader, "FireRating"));
  EXPECT_TRUE(contains(semanticModeHeader, "LoadBearing"));
  EXPECT_TRUE(contains(semanticModeHeader, "Status"));
  EXPECT_TRUE(contains(semanticModeHeader, "Original materials"));

  EXPECT_TRUE(contains(bimManagerHeader, "setSemanticColorMode"));
  EXPECT_TRUE(contains(bimManagerHeader, "semanticColorMode()"));
  EXPECT_TRUE(contains(bimManagerHeader, "semanticColorIdsDirty_"));
  EXPECT_TRUE(contains(bimManagerHeader, "semanticIdForMetadata"));
  EXPECT_TRUE(contains(bimManager, "BimManager::setSemanticColorMode"));
  EXPECT_TRUE(contains(bimManager, "mode == semanticColorMode_"));
  EXPECT_TRUE(contains(bimManager, "!semanticColorIdsDirty_"));
  EXPECT_TRUE(contains(bimManager, "BimManager::semanticIdForMetadata"));
  EXPECT_TRUE(contains(bimManager, "object.objectInfo.w = semanticId"));
  EXPECT_TRUE(contains(bimManager, "bimMetadataStoreyLabel(metadata)"));
  EXPECT_TRUE(contains(bimManager, "bimMetadataMaterialLabel(metadata)"));

  EXPECT_TRUE(contains(guiManagerHeader, "bimSemanticColorMode()"));
  EXPECT_TRUE(contains(guiManager, "Semantic color"));
  EXPECT_TRUE(contains(guiManager, "kBimSemanticColorModes"));
  EXPECT_TRUE(contains(guiManager, "Semantic color legend"));
  EXPECT_TRUE(contains(guiManager, "BimSemanticLegendValues"));
  EXPECT_TRUE(contains(guiManager, "DrawSemanticLegendEntry"));

  EXPECT_TRUE(contains(rendererFrontend, "applyBimSemanticColorMode"));
  EXPECT_TRUE(contains(rendererFrontend, "bimSemanticColorMode()"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.semanticColorMode"));
  EXPECT_TRUE(contains(frameRecorderHeader, "semanticColorMode"));
  EXPECT_TRUE(contains(frameRecorder,
                       "bimPc.semanticColorMode = p.bim.semanticColorMode"));
  EXPECT_TRUE(contains(frameRecorder, "drawTransparentLists(p.draws, 0u)"));
  EXPECT_TRUE(contains(
      frameRecorder, "drawTransparentLists(*draws, p.bim.semanticColorMode)"));

  EXPECT_TRUE(contains(sceneData, "semanticColorMode"));
  EXPECT_TRUE(contains(pushConstants, "uint semanticColorMode"));
  EXPECT_TRUE(contains(materialCommon, "EvaluateBimSemanticColor"));
  EXPECT_TRUE(contains(gbuffer, "pc.semanticColorMode"));
  EXPECT_TRUE(contains(gbuffer, "EvaluateBimSemanticColor"));
  EXPECT_TRUE(contains(transparent, "pc.semanticColorMode"));
  EXPECT_TRUE(contains(transparent, "EvaluateBimSemanticColor"));
}

TEST(RenderingConventionTests,
     BimInspectionExposesElementBoundsAndFloorElevation) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/BimManager.h");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(bimManagerHeader, "BimElementBounds"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementBoundsForObject"));
  EXPECT_TRUE(contains(bimManager, "BimManager::elementBoundsForObject"));
  EXPECT_TRUE(contains(bimManager, "includeBoundsPoint"));
  EXPECT_TRUE(contains(bimManager, "includeBoundsBox"));
  EXPECT_TRUE(contains(bimManager, "includeIndexedBounds"));
  EXPECT_TRUE(contains(bimManager, "objectIndicesForGuid(metadata->guid)"));
  EXPECT_TRUE(
      contains(bimManager, "objectIndicesForSourceId(metadata->sourceId)"));
  EXPECT_TRUE(contains(bimManagerHeader, "objectDrawCommands_"));
  EXPECT_TRUE(contains(bimManager, "appendCommandsForObject"));
  EXPECT_TRUE(contains(guiManagerHeader, "hasSelectionBounds"));
  EXPECT_TRUE(contains(guiManagerHeader, "geometryKind"));
  EXPECT_TRUE(contains(rendererFrontend, "elementBoundsForObject"));
  EXPECT_TRUE(contains(rendererFrontend, "selectionFloorElevation"));
  EXPECT_TRUE(contains(rendererFrontend, "bimGeometryKindLabel"));
  EXPECT_TRUE(contains(guiManager, "Floor elevation"));
  EXPECT_TRUE(contains(guiManager, "Bounds"));
}

TEST(RenderingConventionTests, BimMeasurementToolsUseSelectionBoundsInGui) {
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(guiManagerHeader, "BimMeasurementPointState"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimMeasurementAnnotationState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimMeasurementPointA_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimMeasurementPointB_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimMeasurementAnnotations_"));
  EXPECT_TRUE(
      contains(guiManagerHeader, "bimMeasurementEffectiveImportScale_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimMeasurementObjectCount_"));
  EXPECT_TRUE(contains(guiManager, "BimMeasurementDimensions"));
  EXPECT_TRUE(contains(guiManager, "BimMeasurementFootprintArea"));
  EXPECT_TRUE(contains(guiManager, "BimMeasurementVolume"));
  EXPECT_TRUE(contains(guiManager, "BimMeasurementBetweenCenters"));
  EXPECT_TRUE(contains(guiManager, "measurementSourceChanged"));
  EXPECT_TRUE(contains(guiManager, "bimInspection.effectiveImportScale"));
  EXPECT_TRUE(contains(guiManager, "bimInspection.objectCount"));
  EXPECT_TRUE(contains(guiManager, "selectionBoundsSize"));
  EXPECT_TRUE(contains(guiManager, "selectionBoundsCenter"));
  EXPECT_TRUE(contains(guiManager, "Measurements"));
  EXPECT_TRUE(contains(guiManager, "Dimensions (X/Y/Z)"));
  EXPECT_TRUE(contains(guiManager, "Footprint (X*Z)"));
  EXPECT_TRUE(contains(guiManager, "Volume (bounds)"));
  EXPECT_TRUE(contains(guiManager, "Set A from selection center"));
  EXPECT_TRUE(contains(guiManager, "Set B from selection center"));
  EXPECT_TRUE(contains(guiManager, "Center-to-center"));
  EXPECT_TRUE(contains(guiManager, "Distance: %.3f"));
  EXPECT_TRUE(contains(guiManager, "Horizontal distance"));
  EXPECT_TRUE(contains(guiManager, "Elevation delta"));
  EXPECT_TRUE(contains(guiManager, "Slope angle"));
  EXPECT_TRUE(contains(guiManager, "elevationAxisAngleDegrees"));
  EXPECT_TRUE(contains(guiManager, "Angle to elevation axis"));
  EXPECT_TRUE(contains(guiManager, "Live floor elevation"));
  EXPECT_TRUE(contains(guiManager, "Save measurement annotation"));
  EXPECT_TRUE(contains(guiManager, "Measurement annotations"));
  EXPECT_TRUE(contains(guiManager, "Clear annotations"));
}

TEST(RenderingConventionTests, BimViewpointsExposeBcfImportExportInGui) {
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string bcfHeader =
      readRepoTextFile("include/Container/utility/BcfViewpoint.h");
  const std::string bcfSource =
      readRepoTextFile("src/utility/BcfViewpoint.cpp");

  EXPECT_TRUE(contains(guiManagerHeader, "bcfViewpointPath_"));
  EXPECT_TRUE(contains(guiManagerHeader, "BcfTopicArchiveEntryState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bcfTopicFolderPath_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bcfTopicArchivePath_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bcfTopicArchiveEntries_"));
  EXPECT_TRUE(contains(guiManager, "Container/utility/BcfViewpoint.h"));
  EXPECT_TRUE(contains(guiManager, "BCF viewpoint file"));
  EXPECT_TRUE(contains(guiManager, "Export current BCF"));
  EXPECT_TRUE(contains(guiManager, "Import BCF"));
  EXPECT_TRUE(contains(guiManager, "Export selected BCF"));
  EXPECT_TRUE(contains(guiManager, "BCF Topics"));
  EXPECT_TRUE(contains(guiManager, "BCF topic folder"));
  EXPECT_TRUE(contains(guiManager, "BCF topic archive file"));
  EXPECT_TRUE(contains(guiManager, "Topic title"));
  EXPECT_TRUE(contains(guiManager, "Archive current topic"));
  EXPECT_TRUE(contains(guiManager, "Save topic folder"));
  EXPECT_TRUE(contains(guiManager, "Load topic folder"));
  EXPECT_TRUE(contains(guiManager, "Save topic archive"));
  EXPECT_TRUE(contains(guiManager, "Load topic archive"));
  EXPECT_TRUE(contains(guiManager, "Restore archived topic viewpoint"));
  EXPECT_TRUE(contains(guiManager, "issue server sync is not connected"));
  EXPECT_TRUE(contains(guiManager, "bcf::SaveVisualizationInfo"));
  EXPECT_TRUE(contains(guiManager, "bcf::LoadVisualizationInfo"));
  EXPECT_TRUE(contains(guiManager, "bcf::SaveTopicFolder"));
  EXPECT_TRUE(contains(guiManager, "bcf::LoadTopicFolder"));
  EXPECT_TRUE(contains(guiManager, "bcf::SaveBcfArchive"));
  EXPECT_TRUE(contains(guiManager, "bcf::LoadBcfArchive"));
  EXPECT_TRUE(contains(bcfHeader, "ExportVisualizationInfo"));
  EXPECT_TRUE(contains(bcfHeader, "ImportVisualizationInfo"));
  EXPECT_TRUE(contains(bcfHeader, "SaveTopicFolder"));
  EXPECT_TRUE(contains(bcfHeader, "LoadTopicFolder"));
  EXPECT_TRUE(contains(bcfHeader, "SaveBcfArchive"));
  EXPECT_TRUE(contains(bcfHeader, "LoadBcfArchive"));
  EXPECT_TRUE(contains(bcfSource, "readStandardPerspectiveCamera"));
  EXPECT_TRUE(contains(bcfSource, "CameraViewPoint"));
  EXPECT_TRUE(contains(bcfSource, "CameraDirection"));
  EXPECT_TRUE(contains(bcfSource, "IfcGuid"));
}

TEST(RenderingConventionTests, BimFloorPlanSkipsPointCurvePlaceholders) {
  const std::string dotBimHeader =
      readRepoTextFile("include/Container/geometry/DotBimLoader.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string usdLoader = readRepoTextFile("src/geometry/UsdLoader.cpp");
  const std::string ifcxLoader =
      readRepoTextFile("src/geometry/IfcxLoader.cpp");

  EXPECT_TRUE(contains(dotBimHeader, "enum class GeometryKind"));
  EXPECT_TRUE(contains(dotBimHeader, "GeometryKind geometryKind"));
  EXPECT_TRUE(contains(bimManager, "int64_t y{0}"));
  EXPECT_TRUE(contains(bimManager, "bool preserveElevation"));
  EXPECT_TRUE(contains(bimManager, "preserveElevation ?"));
  EXPECT_TRUE(contains(bimManager, "computeElementBaseElevation"));
  EXPECT_TRUE(contains(bimManager, "storeyBaseElevations"));
  EXPECT_TRUE(contains(bimManager, "floorPlanGround_"));
  EXPECT_TRUE(contains(bimManager, "floorPlanSourceElevation_"));
  EXPECT_TRUE(contains(bimManager, "element.geometryKind !="));
  EXPECT_TRUE(contains(bimManager, "bimGeometryKind(element.geometryKind)"));
  EXPECT_TRUE(contains(usdLoader, "dotbim::GeometryKind::Points"));
  EXPECT_TRUE(contains(usdLoader, "dotbim::GeometryKind::Curves"));
  EXPECT_TRUE(contains(usdLoader, "appendTinyUsdCurvePrim"));
  EXPECT_TRUE(contains(usdLoader, "appendTinyUsdPointsFromStage"));
  EXPECT_TRUE(contains(usdLoader, "tinyUsdPrimNeedsPlaceholderGeometry"));
  EXPECT_TRUE(contains(ifcxLoader, "dotbim::GeometryKind::Points"));
  EXPECT_TRUE(contains(ifcxLoader, "dotbim::GeometryKind::Curves"));
}

TEST(RenderingConventionTests, BimStoreyRangesFeedFloorSliceUi) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/BimManager.h");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(bimManagerHeader, "BimStoreyRange"));
  EXPECT_TRUE(contains(bimManagerHeader, "elementStoreyRanges()"));
  EXPECT_TRUE(contains(guiManagerHeader, "elementStoreyRanges"));
  EXPECT_TRUE(contains(guiManagerHeader, "selectedBimStoreyRangeIndex_"));
  EXPECT_TRUE(contains(bimManager, "elementStoreyRanges_.push_back"));
  EXPECT_TRUE(contains(bimManager, "minElevation"));
  EXPECT_TRUE(contains(bimManager, "maxElevation"));
  EXPECT_TRUE(contains(rendererFrontend, "elementStoreyRanges()"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.elementStoreyRanges"));
  EXPECT_TRUE(contains(guiManager, "Floor slice"));
  EXPECT_TRUE(contains(guiManager, "Spatial Browser"));
  EXPECT_TRUE(contains(guiManager, "Plan storey"));
  EXPECT_TRUE(contains(guiManager, "Previous storey"));
  EXPECT_TRUE(contains(guiManager, "Next storey"));
  EXPECT_TRUE(contains(guiManager, "selectRelativePlanStorey"));
  EXPECT_TRUE(contains(guiManager, "Plan navigation storey"));
  EXPECT_TRUE(contains(guiManager, "Show storey only"));
  EXPECT_TRUE(contains(guiManager, "Plan slice preset"));
  EXPECT_TRUE(contains(guiManager, "Clear storey preset"));
  EXPECT_TRUE(contains(guiManager, "applyStoreyPlanSlice"));
  EXPECT_TRUE(contains(guiManager, "BimSpatialStoreys"));
  EXPECT_TRUE(contains(guiManager, "sectionPlaneState_.normal = {0.0f, -1.0f"));
  EXPECT_TRUE(contains(guiManager, "sectionPlaneState_.offset"));
  EXPECT_TRUE(
      contains(guiManager, "bimFilterState_.storeyFilterEnabled = true"));
  EXPECT_TRUE(contains(guiManager, "floorSliceUsesStoreyFilter"));
  EXPECT_TRUE(contains(guiManager, "sectionPlaneState_ = {}"));
}

TEST(RenderingConventionTests, BimUiGapControlsExposeGracefulPlaceholders) {
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(guiManagerHeader, "BimLayerVisibilityState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimLayerVisibilityState()"));
  EXPECT_TRUE(contains(guiManagerHeader, "pointCloudVisible"));
  EXPECT_TRUE(contains(guiManagerHeader, "curvesVisible"));
  EXPECT_TRUE(contains(guiManagerHeader, "xrayLayerVisible"));
  EXPECT_TRUE(contains(guiManagerHeader, "clashLayerVisible"));
  EXPECT_TRUE(contains(guiManagerHeader, "markupLayerVisible"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimQuickFilterSearch_"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimPropertySearch_"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimSelectionSetMemberState"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimSelectionSetState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimSelectionSets_"));
  EXPECT_TRUE(contains(guiManagerHeader, "selectedBimSelectionSetIndex_"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimLodStreamingUiState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimLodStreamingUiState_"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimClipCapHatchingUiState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimClipCapHatchingUiState_"));
  EXPECT_TRUE(contains(guiManagerHeader, "screenErrorPixels"));
  EXPECT_TRUE(contains(guiManagerHeader, "hatchSpacing"));
  EXPECT_TRUE(contains(guiManagerHeader, "hatchAngleDegrees"));

  EXPECT_TRUE(contains(guiManager, "Property Browser"));
  EXPECT_TRUE(contains(guiManager, "BimPropertyBrowser"));
  EXPECT_TRUE(contains(guiManager, "Property search"));
  EXPECT_TRUE(contains(guiManager, "Clear property search"));
  EXPECT_TRUE(contains(guiManager, "PropertyMatchesSearch"));
  EXPECT_TRUE(contains(guiManager, "Extended properties"));
  EXPECT_TRUE(contains(guiManager, "BimExtendedProperties"));
  EXPECT_TRUE(contains(guiManager, "bimInspection.properties"));
  EXPECT_TRUE(contains(guiManager, "Georeference and Units"));
  EXPECT_TRUE(contains(guiManager, "BimGeoreferenceAndUnits"));
  EXPECT_TRUE(contains(guiManager, "Source up axis"));
  EXPECT_TRUE(contains(guiManager, "Coordinate offset"));
  EXPECT_TRUE(contains(guiManager, "CRS:"));
  EXPECT_TRUE(contains(guiManager, "No CRS metadata exposed"));
  EXPECT_TRUE(contains(guiManager, "Search and Filters"));
  EXPECT_TRUE(contains(guiManager, "Search filter values"));
  EXPECT_TRUE(contains(guiManager, "Clear all BIM filters"));
  EXPECT_TRUE(contains(guiManager, "BimQuickFilterMatches"));
  EXPECT_TRUE(contains(guiManager, "Use selected type"));
  EXPECT_TRUE(contains(guiManager, "Use selected storey"));
  EXPECT_TRUE(contains(guiManager, "Use selected material"));
  EXPECT_TRUE(contains(guiManager, "Selection Sets"));
  EXPECT_TRUE(contains(guiManager, "Create selection set"));
  EXPECT_TRUE(contains(guiManager, "Create set from selection"));
  EXPECT_TRUE(contains(guiManager, "Active selection set"));
  EXPECT_TRUE(contains(guiManager, "Add selection to set"));
  EXPECT_TRUE(contains(guiManager, "BimSelectionSetMembers"));
  EXPECT_TRUE(contains(guiManager, "Restored BIM selection set member"));
  EXPECT_TRUE(contains(guiManager, "multi-select rendering is not connected"));
  EXPECT_TRUE(contains(guiManager, "IFC Relationships"));
  EXPECT_TRUE(contains(guiManager, "BimIfcRelationshipBrowser"));
  EXPECT_TRUE(contains(guiManager, "Spatial containment"));
  EXPECT_TRUE(contains(guiManager, "Material assignment"));
  EXPECT_TRUE(contains(guiManager, "Type/classification"));
  EXPECT_TRUE(contains(guiManager, "Filter related storey"));
  EXPECT_TRUE(contains(guiManager, "Filter related material"));
  EXPECT_TRUE(contains(guiManager, "Filter related type"));
  EXPECT_TRUE(contains(guiManager, "BimIfcPropertySetSummary"));
  EXPECT_TRUE(contains(
      guiManager,
      "Full IFC inverse relationship graph is not exposed by backend"));
  EXPECT_TRUE(contains(
      guiManager, "Select an element to browse inferred IFC relationships"));
  EXPECT_TRUE(contains(guiManager, "No spatial hierarchy exposed by renderer"));
  EXPECT_TRUE(contains(guiManager, "Storey elevation ranges are not exposed"));
  EXPECT_TRUE(contains(guiManager, "Layer Visibility"));
  EXPECT_TRUE(contains(guiManager, "Point-cloud visibility"));
  EXPECT_TRUE(contains(guiManager, "Curve visibility"));
  EXPECT_TRUE(contains(guiManager, "X-ray layer (placeholder)"));
  EXPECT_TRUE(contains(guiManager, "Clash layer (placeholder)"));
  EXPECT_TRUE(contains(guiManager, "Markup layer (placeholder)"));
  EXPECT_TRUE(contains(guiManager, "Point-cloud and curve toggles mask"));
  EXPECT_TRUE(contains(guiManager, "LOD / Streaming"));
  EXPECT_TRUE(contains(guiManager, "Loaded BIM objects"));
  EXPECT_TRUE(contains(guiManager, "Auto LOD request"));
  EXPECT_TRUE(contains(guiManager, "Draw budget enabled"));
  EXPECT_TRUE(contains(guiManager, "Max visible BIM objects"));
  EXPECT_TRUE(contains(guiManager, "LOD bias"));
  EXPECT_TRUE(contains(guiManager, "Screen error pixels"));
  EXPECT_TRUE(contains(guiManager, "Pause streaming request"));
  EXPECT_TRUE(contains(guiManager, "Keep visible storeys resident"));
  EXPECT_TRUE(contains(guiManager, "Optimized metadata cacheable"));
  EXPECT_TRUE(contains(guiManager, "Budget visible"));
  EXPECT_TRUE(contains(guiManager, "meshlet residency buffers are GPU-side"));
  EXPECT_TRUE(contains(guiManager, "Clip / Cap / Hatching"));
  EXPECT_TRUE(contains(guiManager, "Section cap fill"));
  EXPECT_TRUE(contains(guiManager, "Section hatch overlay"));
  EXPECT_TRUE(contains(guiManager, "Cap fill opacity"));
  EXPECT_TRUE(contains(guiManager, "Hatch line width"));
  EXPECT_TRUE(contains(guiManager, "Hatch spacing"));
  EXPECT_TRUE(contains(guiManager, "Hatch angle degrees"));
  EXPECT_TRUE(
      contains(guiManager, "Section caps rebuild when the section plane"));
}

TEST(RenderingConventionTests,
     HiddenBimSelectionDoesNotPopulateSelectedOverlayDrawCommands) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");

  const size_t blockStart = rendererFrontend.find(
      "p.bim.draws.hoveredDrawCommands = &hoveredBimDrawCommands_");
  ASSERT_NE(blockStart, std::string::npos);
  const size_t selectedCollection = rendererFrontend.find(
      "subs_.bimManager->collectDrawCommandsForObject(selectedBimObjectIndex_",
      blockStart);
  ASSERT_NE(selectedCollection, std::string::npos);
  const size_t blockEnd =
      rendererFrontend.find("p.bim.floorPlanDrawCommands", selectedCollection);
  ASSERT_NE(blockEnd, std::string::npos);

  const std::string selectedOverlayBlock =
      rendererFrontend.substr(blockStart, blockEnd - blockStart);
  EXPECT_TRUE(contains(selectedOverlayBlock, "bimFilter.hideSelection"));
  EXPECT_TRUE(
      contains(selectedOverlayBlock, "selectedBimDrawCommands_.clear()"));
  EXPECT_TRUE(
      contains(selectedOverlayBlock, "p.bim.draws.selectedDrawCommands"));
}

TEST(RenderingConventionTests, BimProductIdentityMatchesGuidOrSourceId) {
  container::renderer::BimElementMetadata selected;
  selected.objectIndex = 7u;
  selected.guid = "wall-guid";
  selected.sourceId = "/Project/Site/Wall";

  container::renderer::BimElementMetadata sameObject;
  sameObject.objectIndex = 7u;
  EXPECT_TRUE(
      container::renderer::sameBimProductIdentity(selected, sameObject));

  container::renderer::BimElementMetadata sameGuid;
  sameGuid.objectIndex = 12u;
  sameGuid.guid = "wall-guid";
  EXPECT_TRUE(container::renderer::sameBimProductIdentity(selected, sameGuid));

  container::renderer::BimElementMetadata sameSource;
  sameSource.objectIndex = 13u;
  sameSource.sourceId = "/Project/Site/Wall";
  EXPECT_TRUE(
      container::renderer::sameBimProductIdentity(selected, sameSource));

  container::renderer::BimElementMetadata conflictingGuidSameSource;
  conflictingGuidSameSource.objectIndex = 14u;
  conflictingGuidSameSource.guid = "door-guid";
  conflictingGuidSameSource.sourceId = "/Project/Site/Wall";
  EXPECT_FALSE(container::renderer::sameBimProductIdentity(
      selected, conflictingGuidSameSource));

  container::renderer::BimElementMetadata differentProduct;
  differentProduct.objectIndex = 21u;
  differentProduct.guid = "door-guid";
  differentProduct.sourceId = "/Project/Site/Door";
  EXPECT_FALSE(
      container::renderer::sameBimProductIdentity(selected, differentProduct));

  container::renderer::BimElementMetadata emptyIdentity;
  emptyIdentity.objectIndex = 22u;
  EXPECT_FALSE(
      container::renderer::sameBimProductIdentity(selected, emptyIdentity));
}

TEST(RenderingConventionTests, BimPointAndCurveDrawListsAreRendererScoped) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");

  EXPECT_TRUE(contains(bimManagerHeader, "BimGeometryDrawLists"));
  EXPECT_TRUE(contains(bimManagerHeader, "pointDrawLists()"));
  EXPECT_TRUE(contains(bimManagerHeader, "curveDrawLists()"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativePointDrawLists()"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativeCurveDrawLists()"));
  EXPECT_TRUE(contains(bimManagerHeader, "pointObjectCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "curveObjectCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "pointOpaqueDrawCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "curveOpaqueDrawCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativePointOpaqueDrawCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativeCurveOpaqueDrawCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletClusterCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletSourceClusterCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletEstimatedClusterCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimOptimizedModelMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawLists"));
  EXPECT_TRUE(contains(bimManagerHeader, "points{}"));
  EXPECT_TRUE(contains(bimManagerHeader, "curves{}"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativePoints{}"));
  EXPECT_TRUE(contains(bimManagerHeader, "nativeCurves{}"));

  EXPECT_TRUE(contains(bimManager, "BimGeometryKind::Points"));
  EXPECT_TRUE(contains(bimManager, "BimGeometryKind::Curves"));
  EXPECT_TRUE(contains(bimManager, "appendDrawCommandToGeometryLists"));
  EXPECT_TRUE(contains(bimManager, "pointDrawLists_"));
  EXPECT_TRUE(contains(bimManager, "curveDrawLists_"));
  EXPECT_TRUE(contains(bimManager, "nativePointDrawLists_"));
  EXPECT_TRUE(contains(bimManager, "nativeCurveDrawLists_"));
  EXPECT_TRUE(contains(bimManager, "meshletClusterCountForModel"));
  EXPECT_TRUE(contains(bimManager, "buildMeshletClusterMetadataForModel"));
  EXPECT_TRUE(contains(bimManager, "optimizedModelMetadata_"));
  EXPECT_TRUE(contains(bimManager, "testGeometryDrawLists(pointDrawLists_)"));
  EXPECT_TRUE(contains(bimManager, "testGeometryDrawLists(curveDrawLists_)"));

  EXPECT_TRUE(contains(frameRecorderHeader, "pointDraws"));
  EXPECT_TRUE(contains(frameRecorderHeader, "curveDraws"));
  EXPECT_TRUE(contains(frameRecorder, "bimSurfaceDrawListSet"));
  EXPECT_TRUE(contains(frameRecorder, "bim.pointDraws"));
  EXPECT_TRUE(contains(frameRecorder, "bim.curveDraws"));
  EXPECT_TRUE(contains(frameRecorder, "hasBimOpaqueDrawCommands"));
  EXPECT_TRUE(contains(frameRecorder, "for (const FrameDrawLists *draws"));

  EXPECT_TRUE(contains(rendererFrontend, "pointDrawLists()"));
  EXPECT_TRUE(contains(rendererFrontend, "curveDrawLists()"));
  EXPECT_TRUE(contains(rendererFrontend, "nativePointDrawLists()"));
  EXPECT_TRUE(contains(rendererFrontend, "nativeCurveDrawLists()"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "plan.pointPrimitivePassEnabled = hasAnyGeometry"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "plan.curvePrimitivePassEnabled = hasAnyGeometry"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.pointDraws"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.curveDraws"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.nativePointDraws"));
  EXPECT_TRUE(contains(rendererFrontend, "p.bim.nativeCurveDraws"));
  EXPECT_TRUE(contains(rendererFrontend, "bimLayerVisibilityState()"));
  EXPECT_TRUE(contains(rendererFrontend, "bimObjectVisibleByLayer"));
  EXPECT_TRUE(contains(rendererFrontend, "pointCloudVisible"));
  EXPECT_TRUE(contains(rendererFrontend, "curvesVisible"));
}

TEST(RenderingConventionTests, BimDrawCompactionPreparationUsesPlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string compactionPlannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimDrawCompactionPlanner.h");
  const std::string compactionPlanner =
      readRepoTextFile("src/renderer/bim/BimDrawCompactionPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");

  const size_t recordStart =
      frameRecorder.find("void FrameRecorder::record(VkCommandBuffer");
  ASSERT_NE(recordStart, std::string::npos);
  const size_t recordEnd =
      frameRecorder.find("void FrameRecorder::recordDepthPrepass", recordStart);
  ASSERT_NE(recordEnd, std::string::npos);
  const std::string recordBlock =
      frameRecorder.substr(recordStart, recordEnd - recordStart);

  EXPECT_TRUE(contains(recordBlock, "makeBimDrawCompactionPlanInputs"));
  EXPECT_TRUE(contains(recordBlock, "buildBimDrawCompactionPlan"));
  EXPECT_TRUE(contains(recordBlock, "prepareDrawCompaction"));
  EXPECT_FALSE(contains(recordBlock, "BimDrawCompactionSlot::"));

  EXPECT_TRUE(contains(compactionPlannerHeader, "BimDrawCompactionPlanner"));
  EXPECT_TRUE(contains(compactionPlannerHeader, "BimDrawCompactionPlanInputs"));
  EXPECT_TRUE(contains(compactionPlanner, "transparentSingleSidedSource"));
  EXPECT_TRUE(contains(compactionPlanner,
                       "BimDrawCompactionSlot::TransparentSingleSided"));
  EXPECT_TRUE(
      contains(compactionPlanner, "BimDrawCompactionSlot::NativePointOpaque"));
  EXPECT_TRUE(contains(compactionPlanner,
                       "BimDrawCompactionSlot::NativeCurveTransparent"));
  EXPECT_TRUE(contains(srcCmake, "renderer/bim/BimDrawCompactionPlanner.cpp"));
}

TEST(RenderingConventionTests, BimSurfaceDrawRoutingUsesPlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string routingPlannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimSurfaceDrawRoutingPlanner.h");
  const std::string routingPlanner =
      readRepoTextFile("src/renderer/bim/BimSurfaceDrawRoutingPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");

  const size_t depthStart =
      frameRecorder.find("void FrameRecorder::recordBimDepthPrepass");
  ASSERT_NE(depthStart, std::string::npos);
  const size_t gbufferStart = frameRecorder.find(
      "void FrameRecorder::recordBimGBufferPass", depthStart);
  ASSERT_NE(gbufferStart, std::string::npos);
  const size_t pickStart = frameRecorder.find(
      "void FrameRecorder::recordTransparentPickPass", gbufferStart);
  ASSERT_NE(pickStart, std::string::npos);
  const size_t shadowStart =
      frameRecorder.find("bool FrameRecorder::canRecordShadowPass", pickStart);
  ASSERT_NE(shadowStart, std::string::npos);
  const size_t lightingStart =
      frameRecorder.find("void FrameRecorder::recordLightingPass", shadowStart);
  ASSERT_NE(lightingStart, std::string::npos);
  const size_t transformGizmoStart = frameRecorder.find(
      "void FrameRecorder::recordTransformGizmoPass", lightingStart);
  ASSERT_NE(transformGizmoStart, std::string::npos);
  const std::string depthBlock =
      frameRecorder.substr(depthStart, gbufferStart - depthStart);
  const std::string gbufferBlock =
      frameRecorder.substr(gbufferStart, pickStart - gbufferStart);
  const std::string pickBlock =
      frameRecorder.substr(pickStart, shadowStart - pickStart);
  const std::string lightingBlock =
      frameRecorder.substr(lightingStart, transformGizmoStart - lightingStart);

  EXPECT_TRUE(contains(depthBlock, "buildBimSurfaceDrawRoutingPlan"));
  EXPECT_TRUE(contains(gbufferBlock, "buildBimSurfaceDrawRoutingPlan"));
  EXPECT_TRUE(contains(pickBlock, "buildBimSurfaceDrawRoutingPlan"));
  EXPECT_TRUE(contains(lightingBlock, "buildBimSurfaceDrawRoutingPlan"));
  EXPECT_TRUE(contains(frameRecorder, "pipelineForBimSurfaceRoute"));
  EXPECT_TRUE(contains(frameRecorder, "surfaceDrawLists"));
  EXPECT_TRUE(contains(frameRecorder, "route.gpuCompactionAllowed"));
  EXPECT_TRUE(contains(frameRecorder, "route.cpuFallbackAllowed"));
  EXPECT_TRUE(contains(frameRecorder,
                       ".gpuCompactionEligible = &draws == &p.bim.draws"));
  EXPECT_TRUE(contains(frameRecorder, ".gpuVisibilityOwnsCpuFallback ="));
  EXPECT_FALSE(
      contains(depthBlock, "BimDrawCompactionSlot::OpaqueSingleSided"));
  EXPECT_FALSE(
      contains(gbufferBlock, "BimDrawCompactionSlot::OpaqueWindingFlipped"));
  EXPECT_FALSE(
      contains(pickBlock, "BimDrawCompactionSlot::TransparentSingleSided"));
  EXPECT_FALSE(
      contains(lightingBlock, "BimDrawCompactionSlot::TransparentDoubleSided"));

  EXPECT_TRUE(contains(routingPlannerHeader, "BimSurfaceDrawRoutingPlanner"));
  EXPECT_TRUE(contains(routingPlannerHeader, "BimSurfaceDrawRoutingInputs"));
  EXPECT_TRUE(contains(routingPlanner, "primaryOpaqueDrawCommands"));
  EXPECT_TRUE(contains(routingPlanner, "primaryTransparentDrawCommands"));
  EXPECT_TRUE(contains(routingPlanner,
                       "BimDrawCompactionSlot::TransparentDoubleSided"));
  EXPECT_TRUE(contains(routingPlannerHeader, "gpuCompactionEligible"));
  EXPECT_TRUE(contains(routingPlanner, "gpuCompactionEligible"));
  EXPECT_TRUE(contains(routingPlanner, "gpuVisibilityOwnsCpuFallback"));
  EXPECT_EQ(routingPlanner.find("vkCmd"), std::string::npos);
  EXPECT_TRUE(
      contains(srcCmake, "renderer/bim/BimSurfaceDrawRoutingPlanner.cpp"));
}

TEST(RenderingConventionTests, BimMeshletLodStreamingMetadataIsIdentitySafe) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");

  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletClusterMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimObjectLodStreamingMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimOptimizedModelMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawBudgetLodStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "optimizedModelMetadata()"));
  EXPECT_TRUE(contains(bimManagerHeader, "objectLodStreamingMetadata()"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawBudgetLodStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "cacheHit"));
  EXPECT_TRUE(contains(bimManagerHeader, "cacheStale"));
  EXPECT_TRUE(contains(bimManagerHeader, "cacheWriteAttempted"));
  EXPECT_TRUE(contains(bimManagerHeader, "cacheWriteSucceeded"));
  EXPECT_TRUE(contains(bimManagerHeader, "cachePath"));
  EXPECT_TRUE(contains(bimManagerHeader, "cacheStatus"));

  EXPECT_TRUE(contains(bimManager, "buildMeshletClusterMetadataForModel"));
  EXPECT_TRUE(contains(bimManager, "meshletClusterSpansByMeshId"));
  EXPECT_TRUE(contains(bimManager, "optimizedModelCacheKey"));
  EXPECT_TRUE(contains(bimManager, "optimizedModelMetadataCachePath"));
  EXPECT_TRUE(contains(bimManager, "probeOptimizedModelMetadataCache"));
  EXPECT_TRUE(contains(bimManager, "optimizedModelMetadataCacheMatches"));
  EXPECT_TRUE(contains(bimManager, "writeOptimizedModelMetadataCache"));
  EXPECT_TRUE(contains(bimManager, "refreshOptimizedModelMetadataCache"));
  EXPECT_TRUE(contains(bimManager, "14695981039346656037ull"));
  EXPECT_TRUE(contains(bimManager, "\"schemaVersion\""));
  EXPECT_TRUE(contains(bimManager, "\"residentCandidateClusters\""));
  EXPECT_TRUE(contains(bimManager, "stale refreshed"));
  EXPECT_TRUE(contains(bimManager, "miss written"));
  EXPECT_TRUE(contains(bimManager, "objectLodMetadata_.push_back"));
  EXPECT_TRUE(contains(bimManager, "lodMetadata.objectIndex = objectIndex"));
  EXPECT_TRUE(contains(bimManager, "metadataMatchesFilter(objectIndex"));
  EXPECT_TRUE(contains(bimManager, "visibleMeshletClusterReferences"));

  EXPECT_TRUE(contains(rendererFrontend, "optimizedModelMetadata()"));
  EXPECT_TRUE(contains(rendererFrontend, "drawBudgetLodStats"));
  EXPECT_TRUE(contains(rendererFrontend, "meshletSourceClusterCount"));
  EXPECT_TRUE(
      contains(rendererFrontend, "drawBudgetVisibleMeshletClusterCount"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheHit"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheStale"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheWritten"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCachePath"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheStatus"));

  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheable"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheKey"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheHit"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheStale"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheWritten"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCachePath"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheStatus"));
  EXPECT_TRUE(contains(guiManagerHeader, "drawBudgetVisibleObjectCount"));
  EXPECT_TRUE(
      contains(guiManagerHeader, "drawBudgetVisibleMeshletClusterCount"));
  EXPECT_TRUE(contains(guiManager, "Optimized metadata cacheable"));
  EXPECT_TRUE(contains(guiManager, "Optimized metadata cache: %s"));
  EXPECT_TRUE(contains(guiManager, "Cache key:"));
  EXPECT_TRUE(contains(guiManager, "Cache path: %s"));
  EXPECT_TRUE(contains(guiManager, "Budget visible"));
  EXPECT_TRUE(contains(guiManager, "meshlet residency buffers are GPU-side"));
}

TEST(RenderingConventionTests, BimPointCurveStylesAreRendererPlumbed) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string bimLightingOverlayHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayPlanner.h");
  const std::string bimLightingOverlayPlanner =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayPlanner.cpp");

  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimPointStyleState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimCurveStyleState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimPointCurveStyleState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "float pointSize"));
  EXPECT_TRUE(contains(frameRecorderHeader, "float lineWidth"));
  EXPECT_TRUE(contains(frameRecorderHeader, "pointCurveStyle"));

  EXPECT_TRUE(contains(frameRecorder, "buildBimLightingOverlayPlan"));
  EXPECT_TRUE(contains(frameRecorder, "bimLightingOverlayDrawLists"));
  EXPECT_TRUE(contains(frameRecorder, "bimLightingOverlayPlan.pointStyle"));
  EXPECT_TRUE(contains(frameRecorder, "bimLightingOverlayPlan.curveStyle"));
  EXPECT_TRUE(contains(frameRecorder, "p.bim.pointDraws"));
  EXPECT_TRUE(contains(frameRecorder, "p.bim.curveDraws"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.bim.pointCurveStyle.points.pointSize"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.bim.pointCurveStyle.curves.lineWidth"));
  EXPECT_TRUE(contains(frameRecorder, "bindWireframePipeline"));
  EXPECT_TRUE(contains(frameRecorder, "debugOverlay_.drawWireframe"));
  EXPECT_TRUE(contains(frameRecorder, "vkCmdSetLineWidth"));
  EXPECT_FALSE(contains(frameRecorder, "drawBimPointCurveStyleOverlay"));
  EXPECT_FALSE(contains(frameRecorder, "drawStyledGeometryKind"));
  EXPECT_TRUE(contains(bimLightingOverlayHeader,
                       "BimLightingOverlayStyleInputs"));
  EXPECT_TRUE(contains(bimLightingOverlayPlanner, "buildStylePlan"));
  EXPECT_TRUE(contains(bimLightingOverlayPlanner,
                       "sanitizeBimLightingOverlayOpacity"));
  EXPECT_TRUE(contains(bimLightingOverlayPlanner,
                       "WireframeDepthFrontCull"));
}

TEST(RenderingConventionTests, BimLightingOverlaysUsePlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimLightingOverlayPlanner.h");
  const std::string plannerSource =
      readRepoTextFile("src/renderer/bim/BimLightingOverlayPlanner.cpp");
  const std::string drawCommandHeader =
      readRepoTextFile("include/Container/renderer/DrawCommand.h");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t lightingPass =
      frameRecorder.find("void FrameRecorder::recordLightingPass");
  ASSERT_NE(lightingPass, std::string::npos);
  const size_t transformPass =
      frameRecorder.find("void FrameRecorder::recordTransformGizmoPass",
                         lightingPass);
  ASSERT_NE(transformPass, std::string::npos);
  const std::string lightingBlock =
      frameRecorder.substr(lightingPass, transformPass - lightingPass);

  EXPECT_TRUE(contains(frameRecorder, "BimLightingOverlayPlanner.h"));
  EXPECT_TRUE(contains(lightingBlock, "bimLightingOverlayInputs"));
  EXPECT_TRUE(contains(lightingBlock, "buildBimLightingOverlayPlan"));
  EXPECT_TRUE(contains(lightingBlock, "pipelineForBimLightingOverlay"));
  EXPECT_TRUE(contains(lightingBlock, "bimLightingOverlayPlan.nativePointHover"));
  EXPECT_TRUE(
      contains(lightingBlock, "bimLightingOverlayPlan.sceneSelectionOutline"));
  EXPECT_FALSE(contains(lightingBlock, "drawBimPointCurveStyleOverlay"));
  EXPECT_FALSE(contains(lightingBlock, "drawBimFloorPlanOverlay"));
  EXPECT_FALSE(contains(lightingBlock, "drawInteractionWireframe"));
  EXPECT_FALSE(contains(lightingBlock, "drawNativePrimitiveHighlight"));
  EXPECT_FALSE(contains(lightingBlock, "drawSelectionOutline ="));
  EXPECT_FALSE(
      contains(lightingBlock, "std::clamp(p.bim.floorPlan.opacity"));
  EXPECT_FALSE(contains(
      lightingBlock, "std::max(p.bim.primitivePasses.pointCloud.pointSize"));
  EXPECT_FALSE(contains(lightingBlock,
                        "std::max(p.bim.primitivePasses.curves.lineWidth"));

  EXPECT_TRUE(contains(plannerHeader, "BimLightingOverlayPlanner"));
  EXPECT_TRUE(contains(plannerHeader, "BimLightingOverlayInputs"));
  EXPECT_TRUE(contains(plannerHeader, "BimLightingSelectionOutlinePlan"));
  EXPECT_TRUE(contains(plannerSource, "nativePointHoverWidth"));
  EXPECT_TRUE(contains(plannerSource, "nativeCurveSelectionWidth"));
  EXPECT_TRUE(contains(plannerSource,
                       "sanitizeBimLightingOverlayLineWidth"));
  EXPECT_TRUE(contains(drawCommandHeader, "struct DrawCommand"));
  EXPECT_FALSE(contains(plannerSource, "vkCmd"));
  EXPECT_FALSE(contains(plannerSource, "VkPipeline"));
  EXPECT_FALSE(contains(plannerSource, "FrameRecordParams"));
  EXPECT_FALSE(contains(plannerSource, "GuiManager"));
  EXPECT_FALSE(contains(plannerSource, "DebugOverlayRenderer"));
  EXPECT_FALSE(contains(plannerSource, "LightingManager"));
  EXPECT_TRUE(
      contains(srcCmake, "renderer/bim/BimLightingOverlayPlanner.cpp"));
  EXPECT_TRUE(
      contains(testsCmake, "bim_lighting_overlay_planner_tests"));
}

TEST(RenderingConventionTests,
     BimNativePrimitivePassBoundariesAreRendererScoped) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/PipelineTypes.h");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  const std::string primitivePlannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimPrimitivePassPlanner.h");
  const std::string primitivePlanner =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string wireframeShader =
      readRepoTextFile("shaders/wireframe_debug.slang");

  EXPECT_TRUE(
      contains(frameRecorderHeader, "FrameBimPointCloudPrimitivePassState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimCurvePrimitivePassState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameBimPrimitivePassState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "placeholderRangePreviewEnabled"));
  EXPECT_TRUE(contains(frameRecorderHeader, "primitivePasses"));
  EXPECT_TRUE(
      contains(frameRecorderHeader, "recordBimPointCloudPrimitivePass"));
  EXPECT_TRUE(contains(frameRecorderHeader, "recordBimCurvePrimitivePass"));

  EXPECT_TRUE(
      contains(frameRecorder, "recordBimPointCloudPrimitivePass(cmd, p)"));
  EXPECT_TRUE(contains(frameRecorder, "recordBimCurvePrimitivePass(cmd, p)"));
  EXPECT_TRUE(contains(frameRecorder, "p.bim.primitivePasses.pointCloud"));
  EXPECT_TRUE(contains(frameRecorder, "p.bim.primitivePasses.curves"));
  EXPECT_TRUE(contains(frameRecorder, "buildBimPrimitivePassPlan"));
  EXPECT_TRUE(contains(frameRecorder, "BimPrimitivePassKind::Points"));
  EXPECT_TRUE(contains(frameRecorder, "BimPrimitivePassKind::Curves"));
  EXPECT_TRUE(contains(frameRecorder, "primitivePassDrawLists"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.pipeline.pipelines.bimPointCloudDepth"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.pipeline.pipelines.bimPointCloudNoDepth"));
  EXPECT_TRUE(contains(frameRecorder, "p.pipeline.pipelines.bimCurveDepth"));
  EXPECT_TRUE(contains(frameRecorder, "p.pipeline.pipelines.bimCurveNoDepth"));
  EXPECT_TRUE(contains(primitivePlanner, "hasBimPrimitivePassDrawCommands"));
  EXPECT_TRUE(contains(primitivePlanner, "placeholderRangePreviewEnabled"));
  EXPECT_TRUE(contains(primitivePlanner, "nativeDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(primitivePlanner, "std::clamp(inputs_.opacity"));
  EXPECT_TRUE(contains(primitivePlanner, "std::max(inputs_.primitiveSize"));
  EXPECT_TRUE(contains(primitivePlannerHeader, "BimPrimitivePassPlanner"));
  EXPECT_TRUE(contains(srcCmake, "renderer/bim/BimPrimitivePassPlanner.cpp"));
  EXPECT_TRUE(contains(frameRecorder, "primitivePlan.cpuDrawSources"));
  EXPECT_TRUE(contains(primitivePlanner, "cpuDrawSources"));
  EXPECT_TRUE(contains(primitivePlanner, "draws.opaqueDrawCommands"));
  EXPECT_TRUE(contains(primitivePlanner, "draws.transparentDrawCommands"));
  EXPECT_TRUE(contains(frameRecorder, "draws.opaqueSingleSidedDrawCommands"));
  EXPECT_TRUE(
      contains(frameRecorder, "draws.transparentSingleSidedDrawCommands"));
  EXPECT_TRUE(contains(frameRecorder, "p.bim.nativePointDraws"));
  EXPECT_TRUE(contains(frameRecorder, "p.bim.nativeCurveDraws"));

  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "plan.pointPrimitivePassEnabled = hasAnyGeometry"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "plan.curvePrimitivePassEnabled = hasAnyGeometry"));
  EXPECT_FALSE(contains(rendererFrontend, "if (!nativePointDrawsAvailable)"));
  EXPECT_FALSE(contains(rendererFrontend, "if (!nativeCurveDrawsAvailable)"));
  EXPECT_TRUE(
      contains(rendererFrontend, "assignFrameDrawLists(p.bim.pointDraws"));
  EXPECT_TRUE(
      contains(rendererFrontend, "assignFrameDrawLists(p.bim.curveDraws"));
  EXPECT_TRUE(
      contains(rendererFrontend, "p.bim.primitivePasses.pointCloud.enabled"));
  EXPECT_TRUE(
      contains(rendererFrontend, "p.bim.primitivePasses.curves.enabled"));

  EXPECT_TRUE(contains(pipelineTypes, "bimPointCloudDepth"));
  EXPECT_TRUE(contains(pipelineTypes, "bimPointCloudNoDepth"));
  EXPECT_TRUE(contains(pipelineTypes, "bimCurveDepth"));
  EXPECT_TRUE(contains(pipelineTypes, "bimCurveNoDepth"));

  EXPECT_TRUE(contains(pipelineBuilder, "VK_PRIMITIVE_TOPOLOGY_POINT_LIST"));
  EXPECT_TRUE(contains(pipelineBuilder, "VK_PRIMITIVE_TOPOLOGY_LINE_LIST"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_point_cloud_depth_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_point_cloud_no_depth_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_curve_depth_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_curve_no_depth_pipeline"));
  EXPECT_TRUE(contains(wireframeShader, "SV_PointSize"));
  EXPECT_TRUE(contains(wireframeShader, "output.pointSize = max(pc.lineWidth"));
}

TEST(RenderingConventionTests,
     BimNativePrimitiveGpuVisibilityHasRecorderContract) {
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/BimManager.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string primitivePlanner =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassPlanner.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");

  const size_t slotStart =
      bimManagerHeader.find("enum class BimDrawCompactionSlot");
  ASSERT_NE(slotStart, std::string::npos);
  const size_t slotEnd = bimManagerHeader.find("Count", slotStart);
  ASSERT_NE(slotEnd, std::string::npos);
  const std::string slotBlock =
      bimManagerHeader.substr(slotStart, slotEnd - slotStart);
  const bool nativePrimitiveCompactionSlots =
      contains(slotBlock, "NativePoint") || contains(slotBlock, "NativeCurve");

  const size_t pointPass = frameRecorder.find(
      "void FrameRecorder::recordBimPointCloudPrimitivePass");
  ASSERT_NE(pointPass, std::string::npos);
  const size_t curvePass = frameRecorder.find(
      "void FrameRecorder::recordBimCurvePrimitivePass", pointPass);
  ASSERT_NE(curvePass, std::string::npos);
  const size_t sectionCapPass = frameRecorder.find(
      "void FrameRecorder::recordBimSectionClipCapPass", curvePass);
  ASSERT_NE(sectionCapPass, std::string::npos);
  const std::string pointBlock =
      frameRecorder.substr(pointPass, curvePass - pointPass);
  const std::string curveBlock =
      frameRecorder.substr(curvePass, sectionCapPass - curvePass);

  EXPECT_TRUE(contains(slotBlock, "NativePointOpaque"));
  EXPECT_TRUE(contains(slotBlock, "NativePointTransparent"));
  EXPECT_TRUE(contains(slotBlock, "NativeCurveOpaque"));
  EXPECT_TRUE(contains(slotBlock, "NativeCurveTransparent"));
  EXPECT_TRUE(contains(frameRecorder, "prepareDrawCompaction"));
  EXPECT_TRUE(contains(pointBlock, "primitivePlan.cpuDrawSources"));
  EXPECT_TRUE(contains(curveBlock, "primitivePlan.cpuDrawSources"));
  EXPECT_FALSE(contains(pointBlock, "p.bim.draws"));
  EXPECT_FALSE(contains(curveBlock, "p.bim.draws"));
  EXPECT_TRUE(nativePrimitiveCompactionSlots);
  const bool recorderConsumesNativePrimitiveCompaction =
      contains(pointBlock, "drawGpuCompactedNativePoint") &&
      contains(curveBlock, "drawGpuCompactedNativeCurve") &&
      contains(pointBlock, "p.bim.nativePointDrawsUseGpuVisibility") &&
      contains(curveBlock, "p.bim.nativeCurveDrawsUseGpuVisibility");
  if (recorderConsumesNativePrimitiveCompaction) {
    EXPECT_TRUE(contains(pointBlock, "primitivePlan.gpuSlots"));
    EXPECT_TRUE(contains(curveBlock, "primitivePlan.gpuSlots"));
    EXPECT_TRUE(
        contains(primitivePlanner, "BimDrawCompactionSlot::NativePointOpaque"));
    EXPECT_TRUE(contains(primitivePlanner,
                         "BimDrawCompactionSlot::NativePointTransparent"));
    EXPECT_TRUE(
        contains(primitivePlanner, "BimDrawCompactionSlot::NativeCurveOpaque"));
    EXPECT_TRUE(contains(primitivePlanner,
                         "BimDrawCompactionSlot::NativeCurveTransparent"));
  } else {
    EXPECT_TRUE(
        contains(rendererFrontend, "gpuFilteredNativePrimitiveDrawSources"));
    EXPECT_TRUE(contains(rendererFrontend, "return {};"))
        << "RendererFrontend must keep CPU filtering enabled until "
           "FrameRecorder "
           "can consume native primitive GPU visibility.";
  }
}

TEST(RenderingConventionTests,
     BimNativePointCurveRangesBypassTriangleSurfacePasses) {
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string primitivePlanner =
      readRepoTextFile("src/renderer/bim/BimPrimitivePassPlanner.cpp");
  const std::string deferredFrameState =
      readRepoTextFile("src/renderer/DeferredRasterFrameState.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");

  const size_t drawBuildSwitch =
      bimManager.find("switch (pending.metadata.geometryKind)");
  ASSERT_NE(drawBuildSwitch, std::string::npos);
  const size_t pointCase =
      bimManager.find("case BimGeometryKind::Points:", drawBuildSwitch);
  ASSERT_NE(pointCase, std::string::npos);
  const size_t curveCase =
      bimManager.find("case BimGeometryKind::Curves:", pointCase);
  ASSERT_NE(curveCase, std::string::npos);
  const size_t meshCase =
      bimManager.find("case BimGeometryKind::Mesh:", curveCase);
  ASSERT_NE(meshCase, std::string::npos);

  const std::string pointBlock =
      bimManager.substr(pointCase, curveCase - pointCase);
  const std::string curveBlock =
      bimManager.substr(curveCase, meshCase - curveCase);
  EXPECT_TRUE(contains(pointBlock, "if (pending.nativeIndexCount > 0u)"));
  EXPECT_TRUE(contains(pointBlock, "nativePointDrawLists_"));
  EXPECT_TRUE(contains(pointBlock, "} else {"));
  EXPECT_TRUE(contains(pointBlock, "pointDrawLists_"));
  EXPECT_TRUE(contains(curveBlock, "if (pending.nativeIndexCount > 0u)"));
  EXPECT_TRUE(contains(curveBlock, "nativeCurveDrawLists_"));
  EXPECT_TRUE(contains(curveBlock, "} else {"));
  EXPECT_TRUE(contains(curveBlock, "curveDrawLists_"));
  EXPECT_TRUE(contains(bimManager, "pointCurvePickingCommands"));
  EXPECT_TRUE(contains(bimManager, "geometryDrawListsCoverObject"));
  EXPECT_TRUE(contains(bimManager, "nativePointCurveObjectHasNativeDraw"));

  const size_t collectStart =
      bimManager.find("void BimManager::collectDrawCommandsForObject");
  ASSERT_NE(collectStart, std::string::npos);
  const size_t collectEnd =
      bimManager.find("std::filesystem::path", collectStart);
  ASSERT_NE(collectEnd, std::string::npos);
  const std::string collectBlock =
      bimManager.substr(collectStart, collectEnd - collectStart);
  const size_t nativeSkip =
      collectBlock.find("nativePointCurveObjectHasNativeDraw");
  const size_t placeholderAppend =
      collectBlock.find("outCommands.push_back(objectDrawCommands_");
  ASSERT_NE(nativeSkip, std::string::npos);
  ASSERT_NE(placeholderAppend, std::string::npos);
  EXPECT_LT(nativeSkip, placeholderAppend);

  EXPECT_TRUE(contains(deferredFrameState, "bimSurfaceDrawListSet"));
  EXPECT_FALSE(contains(frameRecorder, "bimDrawListSet"));
  EXPECT_TRUE(
      contains(deferredFrameState,
               "return {&bim.draws, &bim.pointDraws, &bim.curveDraws};"));
  EXPECT_TRUE(
      contains(deferredFrameState,
               "Native point/curve ranges are submitted by the dedicated"));
  EXPECT_TRUE(contains(frameRecorder, ".nativeDraws = primitivePassDrawLists"));
  EXPECT_TRUE(
      contains(primitivePlanner, "hasNativeDraws ? inputs_.nativeDraws"));
  EXPECT_TRUE(
      contains(primitivePlanner, "inputs_.placeholderRangePreviewEnabled"));
  const size_t selectionStart =
      frameRecorder.find("const auto drawSelectionOutlinePlan");
  ASSERT_NE(selectionStart, std::string::npos);
  const size_t selectionEnd =
      frameRecorder.find("lightingState.lightGizmosEnabled", selectionStart);
  ASSERT_NE(selectionEnd, std::string::npos);
  const std::string selectionBlock =
      frameRecorder.substr(selectionStart, selectionEnd - selectionStart);
  EXPECT_TRUE(contains(frameRecorder, "inputs.bimSelectionCommands"));
  EXPECT_TRUE(contains(frameRecorder, "p.bim.draws.selectedDrawCommands"));
  EXPECT_TRUE(contains(frameRecorder, "inputs.nativePointSelectionCommands"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.bim.nativePointDraws.selectedDrawCommands"));
  EXPECT_TRUE(contains(frameRecorder, "inputs.nativeCurveSelectionCommands"));
  EXPECT_TRUE(
      contains(frameRecorder, "p.bim.nativeCurveDraws.selectedDrawCommands"));
  EXPECT_TRUE(
      contains(selectionBlock, "bimLightingOverlayPlan.bimSelectionOutline"));
  EXPECT_TRUE(
      contains(selectionBlock, "bimLightingOverlayPlan.nativePointSelection"));
  EXPECT_TRUE(
      contains(selectionBlock, "bimLightingOverlayPlan.nativeCurveSelection"));
  EXPECT_TRUE(contains(frameRecorder,
                       "BimLightingOverlayPipeline::BimPointCloudDepth"));
  EXPECT_TRUE(
      contains(frameRecorder, "BimLightingOverlayPipeline::BimCurveDepth"));

  const size_t pointFrontendBlockStart =
      rendererFrontend.find("if (bimLayers.pointCloudVisible)");
  ASSERT_NE(pointFrontendBlockStart, std::string::npos);
  const size_t curveFrontendBlockStart = rendererFrontend.find(
      "if (bimLayers.curvesVisible)", pointFrontendBlockStart);
  ASSERT_NE(curveFrontendBlockStart, std::string::npos);
  const size_t frontendBlockEnd = rendererFrontend.find(
      "hoveredBimDrawCommands_.clear()", curveFrontendBlockStart);
  ASSERT_NE(frontendBlockEnd, std::string::npos);
  const std::string frontendBlock = rendererFrontend.substr(
      pointFrontendBlockStart, frontendBlockEnd - pointFrontendBlockStart);
  EXPECT_TRUE(contains(frontendBlock, "assignFrameDrawLists(p.bim.pointDraws"));
  EXPECT_TRUE(
      contains(frontendBlock, "assignFrameDrawLists(p.bim.nativePointDraws"));
  EXPECT_TRUE(contains(frontendBlock, "assignFrameDrawLists(p.bim.curveDraws"));
  EXPECT_TRUE(
      contains(frontendBlock, "assignFrameDrawLists(p.bim.nativeCurveDraws"));
  EXPECT_FALSE(contains(frontendBlock, "if (!nativePointDrawsAvailable)"));
  EXPECT_FALSE(contains(frontendBlock, "if (!nativeCurveDrawsAvailable)"));
  EXPECT_TRUE(
      contains(rendererFrontend, "collectNativePointDrawCommandsForObject"));
  EXPECT_TRUE(
      contains(rendererFrontend, "collectNativeCurveDrawCommandsForObject"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "p.bim.nativePointDraws.selectedDrawCommands"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "p.bim.nativeCurveDraws.selectedDrawCommands"));
  EXPECT_TRUE(
      contains(bimManager, "BimManager::collectNativeDrawCommandsForObject"));
  EXPECT_TRUE(contains(bimManager, "selectedCommand.instanceCount = 1u"));
}

TEST(RenderingConventionTests,
     BimNativePrimitiveTransparencyDoesNotForcePickDepthFiltering) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");

  const size_t surfaceHelperStart = rendererFrontend.find(
      "bool hasTransparentBimSurfaceGeometry(const BimManager&");
  ASSERT_NE(surfaceHelperStart, std::string::npos);
  const size_t surfaceHelperEnd = rendererFrontend.find(
      "BimFrameGpuVisibilityInputs bimFrameGpuVisibilityInputs",
      surfaceHelperStart);
  ASSERT_NE(surfaceHelperEnd, std::string::npos);
  const std::string surfaceHelperBlock = rendererFrontend.substr(
      surfaceHelperStart, surfaceHelperEnd - surfaceHelperStart);
  EXPECT_TRUE(contains(surfaceHelperBlock, "pointDrawLists()"));
  EXPECT_TRUE(contains(surfaceHelperBlock, "curveDrawLists()"));
  EXPECT_FALSE(contains(surfaceHelperBlock, "nativePointDrawLists()"));
  EXPECT_FALSE(contains(surfaceHelperBlock, "nativeCurveDrawLists()"));

  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "inputs_.pointCloudVisible &&"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner, "inputs_.curvesVisible &&"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "inputs_.unfilteredNativePointDraws"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "inputs_.unfilteredNativeCurveDraws"));

  const size_t depthStart = rendererFrontend.find(
      "void RendererFrontend::markDepthVisibilityFrameComplete");
  ASSERT_NE(depthStart, std::string::npos);
  const size_t depthEnd = rendererFrontend.find(
      "bool RendererFrontend::depthVisibilityFrameMatchesCurrentState",
      depthStart);
  ASSERT_NE(depthEnd, std::string::npos);
  const std::string depthBlock =
      rendererFrontend.substr(depthStart, depthEnd - depthStart);
  EXPECT_TRUE(contains(depthBlock, "transparentPickSurfaceGeometry"));
  EXPECT_TRUE(contains(depthBlock, "cpuFilteredSurfaceGeometryRequired"));
  EXPECT_TRUE(contains(depthBlock,
                       "hasAnyGeometry(subs_.bimManager->pointDrawLists())"));
  EXPECT_TRUE(contains(depthBlock,
                       "hasAnyGeometry(subs_.bimManager->curveDrawLists())"));
  EXPECT_TRUE(contains(depthBlock, "hasTransparentBimSurfaceGeometry("));
  EXPECT_TRUE(contains(depthBlock, "filteredDraws"));
  EXPECT_FALSE(contains(depthBlock, "nativePointDrawLists()"));
  EXPECT_FALSE(contains(depthBlock, "nativeCurveDrawLists()"));
  EXPECT_FALSE(
      contains(depthBlock, "hasTransparentBimGeometry(*subs_.bimManager"));
}

TEST(RenderingConventionTests,
     BimNativePrimitiveGpuVisibilityHandoffBypassesCpuFiltering) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "nativePointDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "nativeCurveDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "BimFrameDrawSource::GpuFiltered"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner, "chooseNativePrimitiveDraws"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "plan.cpuFilteredDrawsRequired"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "inputs_.unfilteredNativePointDraws"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "inputs_.unfilteredNativeCurveDraws"));

  const size_t frameStart =
      rendererFrontend.find("RendererFrontend::buildFrameRecordParams");
  ASSERT_NE(frameStart, std::string::npos);
  const size_t frameEnd = rendererFrontend.find(
      "p.transformGizmo = buildTransformGizmoState", frameStart);
  ASSERT_NE(frameEnd, std::string::npos);
  const std::string frameBlock =
      rendererFrontend.substr(frameStart, frameEnd - frameStart);
  const size_t sourceCall =
      frameBlock.find("buildBimFrameDrawRoutingPlan(bimRoutingInputs)");
  const size_t cpuNeed = frameBlock.find("bimRouting.cpuFilteredDrawsRequired");
  const size_t filteredCall = frameBlock.find("filteredDrawLists(bimFilter)");
  ASSERT_NE(sourceCall, std::string::npos);
  ASSERT_NE(cpuNeed, std::string::npos);
  ASSERT_NE(filteredCall, std::string::npos);
  EXPECT_LT(sourceCall, cpuNeed);
  EXPECT_LT(cpuNeed, filteredCall);
  EXPECT_TRUE(contains(frameBlock, "bimFrameDrawRoutingInputs"));
  EXPECT_TRUE(contains(frameBlock, "bimRoutingInputs.cpuFilteredDraws"));
  EXPECT_TRUE(contains(frameBlock, "bimRouting.nativePointDraws"));
  EXPECT_TRUE(contains(frameBlock, "bimRouting.nativeCurveDraws"));
  EXPECT_TRUE(contains(frameBlock, "p.bim.nativePointDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(frameBlock, "p.bim.nativeCurveDrawsUseGpuVisibility"));
}

TEST(RenderingConventionTests, BimFrameDrawRoutingUsesPlanner) {
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/bim/BimFrameDrawRoutingPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t frameStart =
      rendererFrontend.find("RendererFrontend::buildFrameRecordParams");
  ASSERT_NE(frameStart, std::string::npos);
  const size_t frameEnd = rendererFrontend.find(
      "p.transformGizmo = buildTransformGizmoState", frameStart);
  ASSERT_NE(frameEnd, std::string::npos);
  const std::string frameBlock =
      rendererFrontend.substr(frameStart, frameEnd - frameStart);

  EXPECT_TRUE(
      contains(rendererFrontend, "BimFrameDrawRoutingPlanner.h"));
  EXPECT_TRUE(contains(frameBlock, "bimFrameDrawRoutingInputs"));
  EXPECT_TRUE(contains(frameBlock, "buildBimFrameDrawRoutingPlan"));
  EXPECT_TRUE(contains(frameBlock, "bimRouting.cpuFilteredDrawsRequired"));
  EXPECT_TRUE(contains(frameBlock, "assignFrameDrawLists(p.bim.draws"));
  EXPECT_TRUE(contains(frameBlock, "bimRouting.pointPrimitivePassEnabled"));
  EXPECT_TRUE(contains(frameBlock, "bimRouting.curvePrimitivePassEnabled"));
  EXPECT_FALSE(contains(frameBlock, "gpuFilteredNativePrimitiveDrawSources"));
  EXPECT_FALSE(contains(frameBlock, "needsCpuFilteredBimDrawLists"));
  EXPECT_FALSE(contains(frameBlock, "chooseNativePrimitiveDrawLists"));

  EXPECT_TRUE(contains(plannerHeader, "BimFrameDrawRoutingPlanner"));
  EXPECT_TRUE(contains(plannerHeader, "BimFrameDrawSource"));
  EXPECT_TRUE(contains(planner, "meshDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(planner, "nativePointDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(planner, "nativeCurveDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(planner, "chooseNativePrimitiveDraws"));
  EXPECT_EQ(planner.find("vkCmd"), std::string::npos);
  EXPECT_TRUE(
      contains(srcCmake, "renderer/bim/BimFrameDrawRoutingPlanner.cpp"));
  EXPECT_TRUE(contains(testsCmake, "bim_frame_draw_routing_planner_tests"));
}

TEST(RenderingConventionTests, BimMeshletLodStreamingMetadataIsSurfaced) {
  const std::string dotBimHeader =
      readRepoTextFile("include/Container/geometry/DotBimLoader.h");
  const std::string usdLoader = readRepoTextFile("src/geometry/UsdLoader.cpp");
  const std::string ifcxLoader =
      readRepoTextFile("src/geometry/IfcxLoader.cpp");
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string bimFrameRoutingPlanner =
      readRepoTextFile("src/renderer/bim/BimFrameDrawRoutingPlanner.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string meshletResidencyShader =
      readRepoTextFile("shaders/bim_meshlet_residency.slang");
  const std::string visibilityFilterShader =
      readRepoTextFile("shaders/bim_visibility_filter.slang");
  const std::string drawCompactionShader =
      readRepoTextFile("shaders/bim_draw_compact.slang");
  const std::string shadersCmake = readRepoTextFile("cmake/Shaders.cmake");

  EXPECT_TRUE(contains(dotBimHeader, "MeshletClusterRange"));
  EXPECT_TRUE(contains(dotBimHeader,
                       "std::vector<MeshletClusterRange> meshletClusters"));
  EXPECT_TRUE(contains(dotBimHeader, "uint32_t lodLevel"));
  EXPECT_TRUE(contains(dotBimHeader, "boundsCenter"));
  EXPECT_TRUE(contains(dotBimHeader, "boundsRadius"));

  EXPECT_TRUE(contains(usdLoader, "appendMeshletClustersForRange"));
  EXPECT_TRUE(contains(usdLoader, "model.meshletClusters.push_back"));
  EXPECT_TRUE(contains(usdLoader, ".lodLevel = 0u"));
  EXPECT_TRUE(contains(ifcxLoader, "appendMeshletClustersForRange"));
  EXPECT_TRUE(contains(ifcxLoader, "model.meshletClusters.push_back"));
  EXPECT_TRUE(contains(ifcxLoader, ".lodLevel = 0u"));

  EXPECT_TRUE(contains(bimManagerHeader, "size_t meshletClusterCount"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletClusterMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimObjectLodStreamingMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimOptimizedModelMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawBudgetLodStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletGpuCluster"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletGpuObjectLod"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletResidencyEntry"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletResidencyPushConstants"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletResidencySettings"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimMeshletResidencyStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimVisibilityGpuObjectMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimVisibilityFilterPushConstants"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimVisibilityFilterStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawCompactionPushConstants"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawCompactionStats"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimDrawCompactionSlot"));
  EXPECT_TRUE(
      contains(bimManagerHeader, "visibilityMaskReadyForDrawCompaction"));
  EXPECT_TRUE(contains(bimManagerHeader, "inputSourceRevision"));
  EXPECT_TRUE(contains(bimManagerHeader, "OpaqueWindingFlipped"));
  EXPECT_TRUE(contains(bimManagerHeader, "OpaqueDoubleSided"));
  EXPECT_TRUE(contains(bimManagerHeader, "NativePointOpaque"));
  EXPECT_TRUE(contains(bimManagerHeader, "NativeCurveTransparent"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawCompactionSlots_"));
  EXPECT_TRUE(contains(bimManagerHeader, "computeReady"));
  EXPECT_TRUE(contains(bimManagerHeader, "dispatchPending"));
  EXPECT_TRUE(contains(bimManagerHeader, "createMeshletResidencyResources"));
  EXPECT_TRUE(contains(bimManagerHeader, "updateMeshletResidencySettings"));
  EXPECT_TRUE(contains(bimManagerHeader, "updateVisibilityFilterSettings"));
  EXPECT_TRUE(contains(bimManagerHeader, "prepareDrawCompaction"));
  EXPECT_TRUE(contains(bimManagerHeader, "recordMeshletResidencyUpdate"));
  EXPECT_TRUE(contains(bimManagerHeader, "recordVisibilityFilterUpdate"));
  EXPECT_TRUE(contains(bimManagerHeader, "recordDrawCompactionUpdate"));
  EXPECT_TRUE(
      contains(bimManagerHeader, "drawCompacted(BimDrawCompactionSlot"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawCompactedOpaqueSingleSided"));
  EXPECT_TRUE(contains(bimManagerHeader, "objectLodStreamingMetadata()"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletClusterBuffer()"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletResidencyBuffer()"));
  EXPECT_TRUE(contains(bimManagerHeader, "meshletResidencyStats()"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawCompactionStats()"));
  EXPECT_TRUE(contains(bimManagerHeader, "drawBudgetLodStats"));
  EXPECT_TRUE(contains(bimManager, "meshletClusterCountForModel"));
  EXPECT_TRUE(contains(bimManager, "buildMeshletClusterMetadataForModel"));
  EXPECT_TRUE(contains(bimManager, "model.meshletClusters.size()"));
  EXPECT_TRUE(contains(
      bimManager, "meshletClusters_ = buildMeshletClusterMetadataForModel"));
  EXPECT_TRUE(
      contains(bimManager, "meshletClusterCount_ = meshletClusters_.size()"));
  EXPECT_TRUE(contains(bimManager, "buildOptimizedModelMetadata"));
  EXPECT_TRUE(contains(bimManager, "objectLodMetadata_.push_back"));
  EXPECT_TRUE(contains(bimManager, "uploadMeshletResidencyBuffers"));
  EXPECT_TRUE(contains(bimManager, "uploadVisibilityFilterBuffers"));
  EXPECT_TRUE(contains(bimManager, "createVisibilityFilterResources"));
  EXPECT_TRUE(contains(bimManager, "writeVisibilityFilterDescriptorSet"));
  EXPECT_TRUE(contains(bimManager, "sameVisibilityFilterSettings"));
  EXPECT_TRUE(contains(bimManager, "visibilityFilterMaskCurrent_"));
  EXPECT_TRUE(contains(bimManager, "invalidateDrawCompactionOutputs"));
  EXPECT_TRUE(contains(bimManager, "writeMeshletResidencyDescriptorSet"));
  EXPECT_TRUE(contains(bimManager, "createDrawCompactionResources"));
  EXPECT_TRUE(contains(bimManager, "writeDrawCompactionDescriptorSet"));
  EXPECT_TRUE(contains(bimManager, "createComputePipeline"));
  EXPECT_TRUE(contains(bimManager, "vkCmdDispatch"));
  EXPECT_TRUE(contains(bimManager, "vkCmdDrawIndexedIndirectCount"));
  EXPECT_TRUE(contains(bimManager, "drawCompactionSlots_"));
  EXPECT_TRUE(contains(bimManager, "drawCompactionDescriptorSets_"));
  EXPECT_TRUE(contains(bimManager, "visibilityMaskBuffer_"));
  EXPECT_TRUE(contains(bimManager, "visibilityFilterSettings_"));
  EXPECT_TRUE(contains(bimManager, "cameraBuffer"));
  EXPECT_TRUE(contains(bimManager, "objectBuffer"));
  EXPECT_TRUE(contains(bimManager, "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER"));
  EXPECT_TRUE(contains(bimManager, "VK_PIPELINE_BIND_POINT_COMPUTE"));
  EXPECT_TRUE(contains(bimManager, "VK_BUFFER_USAGE_STORAGE_BUFFER_BIT"));
  EXPECT_TRUE(contains(bimManager, "meshletResidencyStats_"));
  EXPECT_TRUE(contains(rendererFrontend, "createMeshletResidencyResources"));
  EXPECT_TRUE(contains(rendererFrontend, "bimLodStreamingUiState()"));
  EXPECT_TRUE(contains(rendererFrontend, "updateMeshletResidencySettings"));
  EXPECT_TRUE(contains(rendererFrontend, "updateVisibilityFilterSettings"));
  EXPECT_TRUE(contains(rendererFrontend, "bimFrameGpuVisibilityInputs"));
  EXPECT_TRUE(contains(rendererFrontend, "buildBimFrameDrawRoutingPlan"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner, "BimFrameDrawRoutingPlanner"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "bimFrameGpuVisibilityAvailable"));
  EXPECT_TRUE(contains(bimFrameRoutingPlanner,
                       "plan.cpuFilteredDrawsRequired"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.meshletClusterCount"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.meshletSourceClusterCount"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.meshletEstimatedClusterCount"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.meshletObjectReferenceCount"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.meshletMaxLodLevel"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheable"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheHit"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheStale"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheWritten"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCachePath"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.optimizedModelMetadataCacheStatus"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.drawBudgetVisibleMeshletClusterCount"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "bimInspection.meshletGpuResidentObjectCount"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.meshletGpuBufferBytes"));
  EXPECT_TRUE(
      contains(rendererFrontend, "bimInspection.meshletGpuComputeReady"));
  EXPECT_TRUE(contains(frameRecorderHeader, "BimManager*") ||
              contains(frameRecorderHeader, "BimManager *"));
  EXPECT_TRUE(contains(frameRecorderHeader, "opaqueMeshDrawsUseGpuVisibility"));
  EXPECT_TRUE(contains(frameRecorder, "recordMeshletResidencyUpdate"));
  EXPECT_TRUE(contains(frameRecorder, "recordVisibilityFilterUpdate"));
  EXPECT_TRUE(contains(frameRecorder, "prepareDrawCompaction"));
  EXPECT_TRUE(contains(frameRecorder, "recordDrawCompactionUpdate"));
  EXPECT_TRUE(contains(frameRecorder, "gpuVisibilityOwnsCpuFallback"));
  EXPECT_TRUE(contains(frameRecorder, "usesGpuFilteredBimMeshShadowPath"));
  EXPECT_TRUE(contains(frameRecorder, "drawGpuFilteredBimShadowSlot"));
  EXPECT_TRUE(contains(frameRecorder, "drawCompactionReady(slot)"));
  EXPECT_TRUE(
      contains(frameRecorder, "BimDrawCompactionSlot::OpaqueWindingFlipped"));
  EXPECT_TRUE(
      contains(frameRecorder, "BimDrawCompactionSlot::OpaqueDoubleSided"));
  EXPECT_TRUE(contains(frameRecorder, "drawCompacted("));
  EXPECT_TRUE(contains(frameRecorder, "p.services.bimManager"));
  EXPECT_TRUE(contains(frameRecorder, "p.camera.cameraBuffer"));
  EXPECT_TRUE(contains(frameRecorder, "p.bim.scene.objectBuffer"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletClusterCount"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletSourceClusterCount"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletEstimatedClusterCount"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheHit"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheStale"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheWritten"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCachePath"));
  EXPECT_TRUE(contains(guiManagerHeader, "optimizedModelMetadataCacheStatus"));
  EXPECT_TRUE(
      contains(guiManagerHeader, "drawBudgetVisibleMeshletClusterCount"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletGpuResidentObjectCount"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletGpuBufferBytes"));
  EXPECT_TRUE(contains(guiManagerHeader, "meshletGpuComputeReady"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimLodStreamingUiState()"));
  EXPECT_TRUE(contains(guiManager,
                       "Meshlet clusters: %zu (%zu source, %zu estimated)"));
  EXPECT_TRUE(contains(guiManager, "GPU meshlet residency"));
  EXPECT_TRUE(contains(guiManager, "Optimized metadata cache: %s"));
  EXPECT_TRUE(contains(guiManager, "Cache path: %s"));
  EXPECT_TRUE(
      contains(guiManager, "Draw budget preserves BIM object identity"));
  EXPECT_TRUE(contains(guiManager, "GPU-side and cacheable"));
  EXPECT_TRUE(contains(meshletResidencyShader, "[numthreads(64, 1, 1)]"));
  EXPECT_TRUE(contains(meshletResidencyShader, "RWStructuredBuffer"));
  EXPECT_TRUE(contains(meshletResidencyShader, "BimMeshletResidencyEntry"));
  EXPECT_TRUE(contains(meshletResidencyShader, "forceResident"));
  EXPECT_TRUE(contains(meshletResidencyShader, "ConstantBuffer<CameraBuffer>"));
  EXPECT_TRUE(
      contains(meshletResidencyShader, "StructuredBuffer<ObjectBuffer>"));
  EXPECT_TRUE(contains(meshletResidencyShader, "projectedRadiusPixels"));
  EXPECT_TRUE(contains(meshletResidencyShader, "selectLodLevel"));
  EXPECT_TRUE(contains(meshletResidencyShader, "drawBudgetMaxObjects"));
  EXPECT_TRUE(contains(meshletResidencyShader, "screenErrorPixels"));
  EXPECT_TRUE(contains(meshletResidencyShader, "BIM_GEOMETRY_KIND_POINTS"));
  EXPECT_TRUE(contains(meshletResidencyShader, "nativePrimitive"));
  EXPECT_TRUE(contains(visibilityFilterShader, "[numthreads(64, 1, 1)]"));
  EXPECT_TRUE(contains(visibilityFilterShader, "BimVisibilityObjectMetadata"));
  EXPECT_TRUE(contains(visibilityFilterShader, "uVisibilityMask"));
  EXPECT_TRUE(contains(visibilityFilterShader, "BIM_VIS_FILTER_TYPE"));
  EXPECT_TRUE(
      contains(visibilityFilterShader, "BIM_VIS_FILTER_ISOLATE_SELECTION"));
  EXPECT_TRUE(contains(drawCompactionShader, "[numthreads(64, 1, 1)]"));
  EXPECT_TRUE(contains(drawCompactionShader, "InterlockedAdd"));
  EXPECT_TRUE(contains(drawCompactionShader, "objectIsResident"));
  EXPECT_TRUE(contains(drawCompactionShader, "uVisibilityMask"));
  EXPECT_TRUE(
      contains(drawCompactionShader, "source.firstInstance + instanceIndex"));
  EXPECT_TRUE(contains(drawCompactionShader, "DrawIndexedIndirectCommand"));
  EXPECT_TRUE(contains(shadersCmake, "bim_meshlet_residency.comp.spv"));
  EXPECT_TRUE(contains(shadersCmake, "bim_visibility_filter.comp.spv"));
  EXPECT_TRUE(contains(shadersCmake, "bim_draw_compact.comp.spv"));
}

TEST(RenderingConventionTests, BimGpuVisibilityFeedsShadowCasterSlots) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");

  const size_t secondaryDecision = frameRecorder.find(
      "bool FrameRecorder::shouldUseShadowSecondaryCommandBuffer");
  ASSERT_NE(secondaryDecision, std::string::npos);
  const size_t secondaryDecisionEnd = frameRecorder.find(
      "void FrameRecorder::recordShadowCascadeSecondaryCommandBuffers",
      secondaryDecision);
  ASSERT_NE(secondaryDecisionEnd, std::string::npos);
  const std::string secondaryBlock = frameRecorder.substr(
      secondaryDecision, secondaryDecisionEnd - secondaryDecision);
  EXPECT_TRUE(contains(secondaryBlock, "usesGpuFilteredBimMeshShadowPath(p)"));
  EXPECT_TRUE(contains(secondaryBlock, "return false;"));

  const size_t shadowBody =
      frameRecorder.find("void FrameRecorder::recordShadowPassBody");
  ASSERT_NE(shadowBody, std::string::npos);
  const size_t shadowBodyEnd = frameRecorder.find(
      "void FrameRecorder::recordBimPointCloudPrimitivePass", shadowBody);
  ASSERT_NE(shadowBodyEnd, std::string::npos);
  const std::string shadowBlock =
      frameRecorder.substr(shadowBody, shadowBodyEnd - shadowBody);

  const size_t bimGeometry = shadowBlock.find("if (hasBimShadowGeometry(p))");
  const size_t gpuSlot = shadowBlock.find("drawGpuFilteredBimShadowSlot");
  const size_t cpuCascade =
      shadowBlock.find("bimShadowCascadeSingleSidedDrawCommands_", gpuSlot);
  ASSERT_NE(bimGeometry, std::string::npos);
  ASSERT_NE(gpuSlot, std::string::npos);
  ASSERT_NE(cpuCascade, std::string::npos);
  EXPECT_LT(bimGeometry, gpuSlot);
  EXPECT_LT(gpuSlot, cpuCascade);

  EXPECT_TRUE(
      contains(shadowBlock, "BimDrawCompactionSlot::OpaqueSingleSided"));
  EXPECT_TRUE(
      contains(shadowBlock, "BimDrawCompactionSlot::OpaqueWindingFlipped"));
  EXPECT_TRUE(
      contains(shadowBlock, "BimDrawCompactionSlot::OpaqueDoubleSided"));
  EXPECT_TRUE(contains(shadowBlock, "drawCompactionReady(slot)"));
  EXPECT_TRUE(contains(shadowBlock, "drawCompacted(slot, cmd)"));

  const size_t prepareCascade = frameRecorder.find(
      "void FrameRecorder::prepareShadowCascadeDrawCommands");
  ASSERT_NE(prepareCascade, std::string::npos);
  const size_t prepareCascadeEnd = frameRecorder.find(
      "void FrameRecorder::bindSceneGeometryBuffers", prepareCascade);
  ASSERT_NE(prepareCascadeEnd, std::string::npos);
  const std::string prepareBlock =
      frameRecorder.substr(prepareCascade, prepareCascadeEnd - prepareCascade);
  EXPECT_TRUE(contains(prepareBlock, "ShadowCascadeDrawPlanner"));
  EXPECT_TRUE(contains(prepareBlock, "planner.build()"));
  EXPECT_TRUE(contains(frameRecorder, "p.bim.opaqueMeshDrawsUseGpuVisibility"));
}

TEST(RenderingConventionTests, ShadowCascadeDrawPlanningUsesPlanner) {
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string plannerHeader = readRepoTextFile(
      "include/Container/renderer/shadow/ShadowCascadeDrawPlanner.h");
  const std::string planner =
      readRepoTextFile("src/renderer/shadow/ShadowCascadeDrawPlanner.cpp");
  const std::string srcCmake = readRepoTextFile("src/CMakeLists.txt");
  const std::string testsCmake =
      readRepoTextFile("tests/CMakeLists.tests.cmake");

  const size_t prepareCascade = frameRecorder.find(
      "void FrameRecorder::prepareShadowCascadeDrawCommands");
  ASSERT_NE(prepareCascade, std::string::npos);
  const size_t prepareCascadeEnd = frameRecorder.find(
      "void FrameRecorder::bindSceneGeometryBuffers", prepareCascade);
  ASSERT_NE(prepareCascadeEnd, std::string::npos);
  const std::string prepareBlock =
      frameRecorder.substr(prepareCascade, prepareCascadeEnd - prepareCascade);

  EXPECT_TRUE(contains(frameRecorderHeader, "ShadowCascadeDrawPlannerInputs"));
  EXPECT_TRUE(contains(frameRecorder, "shadowCascadeDrawPlannerInputs"));
  EXPECT_TRUE(contains(prepareBlock, "ShadowCascadeDrawPlanner planner"));
  EXPECT_TRUE(contains(prepareBlock, "plan.sceneSingleSided"));
  EXPECT_FALSE(contains(prepareBlock, "cascadeIntersectsSphere"));
  EXPECT_TRUE(contains(plannerHeader, "ShadowCascadeDrawPlanner"));
  EXPECT_TRUE(contains(planner, "appendVisibleRun"));
  EXPECT_TRUE(contains(planner, "cpuFallbackAllowed"));
  EXPECT_TRUE(contains(planner, "sceneSingleSidedUsesGpuCull"));
  EXPECT_EQ(planner.find("vkCmd"), std::string::npos);
  EXPECT_TRUE(
      contains(srcCmake, "renderer/shadow/ShadowCascadeDrawPlanner.cpp"));
  EXPECT_TRUE(contains(testsCmake, "shadow_cascade_draw_planner_tests"));
}

TEST(RenderingConventionTests, BimSectionCapGeneratedMeshBuildsFillAndHatches) {
  using container::renderer::BimSectionCapBuildOptions;
  using container::renderer::BimSectionCapGeneratedMesh;
  using container::renderer::BimSectionCapTriangle;
  using container::renderer::BuildBimSectionCapMesh;

  const std::array<glm::vec3, 8> vertices{{
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
  std::array<BimSectionCapTriangle, 12> triangles{};
  for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
    triangles[faceIndex] = BimSectionCapTriangle{
        .objectIndex = 17u,
        .p0 = vertices[faces[faceIndex][0]],
        .p1 = vertices[faces[faceIndex][1]],
        .p2 = vertices[faces[faceIndex][2]],
    };
  }
  BimSectionCapBuildOptions options{};
  options.sectionPlane = {0.0f, 1.0f, 0.0f, 0.0f};
  options.hatchSpacing = 0.2f;
  options.hatchAngleRadians = 0.0f;
  options.capOffset = 0.0f;
  options.crossHatch = true;

  const BimSectionCapGeneratedMesh mesh =
      BuildBimSectionCapMesh(triangles, options);

  ASSERT_TRUE(mesh.valid());
  ASSERT_FALSE(mesh.fillDrawCommands.empty());
  ASSERT_FALSE(mesh.hatchDrawCommands.empty());
  EXPECT_EQ(mesh.fillDrawCommands[0].objectIndex, 17u);
  EXPECT_EQ(mesh.hatchDrawCommands[0].objectIndex, 17u);
  EXPECT_EQ(mesh.fillDrawCommands[0].firstIndex % 3u, 0u);
  EXPECT_EQ(mesh.fillDrawCommands[0].indexCount % 3u, 0u);
  EXPECT_EQ(mesh.hatchDrawCommands[0].indexCount % 2u, 0u);
  for (const container::geometry::Vertex &vertex : mesh.vertices) {
    EXPECT_NEAR(vertex.position.y, 0.0f, 1.0e-5f);
  }
}

TEST(RenderingConventionTests, BimSectionClipCapsExposeShaderStyleContract) {
  const std::string frameRecorderHeader =
      readRepoTextFile("include/Container/renderer/FrameRecorder.h");
  const std::string frameRecorder =
      readRepoTextFile("src/renderer/FrameRecorder.cpp");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/PipelineTypes.h");
  const std::string pipelineBuilder =
      readRepoTextFile("src/renderer/GraphicsPipelineBuilder.cpp");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string sceneData =
      readRepoTextFile("include/Container/utility/SceneData.h");
  const std::string sceneManagerHeader =
      readRepoTextFile("include/Container/utility/SceneManager.h");
  const std::string sceneManager =
      readRepoTextFile("src/utility/SceneManager.cpp");
  const std::string pushConstants =
      readRepoTextFile("shaders/push_constants_common.slang");
  const std::string sceneClipCommon =
      readRepoTextFile("shaders/scene_clip_common.slang");

  EXPECT_TRUE(contains(frameRecorderHeader, "FrameSectionBoxClipState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameSectionClipCapStyleState"));
  EXPECT_TRUE(contains(frameRecorderHeader, "FrameSectionClipCapHatchMode"));
  EXPECT_TRUE(contains(frameRecorderHeader, "hatchSpacing"));
  EXPECT_TRUE(contains(frameRecorderHeader, "hatchLineWidth"));
  EXPECT_TRUE(contains(frameRecorderHeader, "fillColor"));
  EXPECT_TRUE(contains(frameRecorderHeader, "hatchColor"));
  EXPECT_TRUE(contains(frameRecorderHeader, "std::array<glm::vec4, 6> planes"));
  EXPECT_TRUE(contains(frameRecorderHeader, "sectionClipCaps"));
  EXPECT_TRUE(contains(frameRecorderHeader, "recordBimSectionClipCapPass"));

  EXPECT_TRUE(contains(frameRecorder, "sectionClipCapStyleActive"));
  EXPECT_TRUE(contains(frameRecorder, "recordBimSectionClipCapPass(cmd, p)"));
  EXPECT_TRUE(contains(frameRecorder, "bimSectionClipCapFill"));
  EXPECT_TRUE(contains(frameRecorder, "bimSectionClipCapHatch"));
  EXPECT_TRUE(
      contains(frameRecorder, "sectionClipCapGeometry.fillDrawCommands"));
  EXPECT_TRUE(
      contains(frameRecorder, "sectionClipCapGeometry.hatchDrawCommands"));
  EXPECT_TRUE(contains(frameRecorder, "debugOverlay_.drawWireframe"));
  EXPECT_TRUE(contains(pipelineTypes, "bimSectionClipCapFill"));
  EXPECT_TRUE(contains(pipelineTypes, "bimSectionClipCapHatch"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_section_clip_cap_fill_pipeline"));
  EXPECT_TRUE(contains(pipelineBuilder, "bim_section_clip_cap_hatch_pipeline"));
  EXPECT_TRUE(contains(rendererFrontend, "bimClipCapHatchingUiState()"));
  EXPECT_TRUE(contains(rendererFrontend, "makeBoxClipPlanes"));
  EXPECT_TRUE(contains(rendererFrontend, "updateSceneClipState"));
  EXPECT_TRUE(contains(rendererFrontend, "capOptions.clipPlaneCount"));
  EXPECT_TRUE(contains(rendererFrontend,
                       "capOptions.clipPlanes = activeBoxClipPlanes"));
  EXPECT_TRUE(contains(rendererFrontend, "rebuildSectionClipCapGeometry"));
  EXPECT_TRUE(contains(rendererFrontend, "sectionClipCapDrawData()"));
  EXPECT_TRUE(contains(bimManager, "sameSectionCapBuildOptions"));
  EXPECT_TRUE(contains(bimManager, "sectionClipCapBuildOptionsValid_"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimClipCapHatchingUiState"));
  EXPECT_TRUE(contains(guiManagerHeader, "BimBoxClipUiState"));
  EXPECT_TRUE(contains(guiManagerHeader, "bimBoxClipState()"));
  EXPECT_TRUE(contains(guiManager, "Box clip"));
  EXPECT_TRUE(contains(guiManager, "Invert box clip"));
  EXPECT_TRUE(contains(sceneData, "struct SceneClipState"));
  EXPECT_TRUE(contains(sceneData, "boxClipPlanes[6]"));
  EXPECT_TRUE(contains(sceneManagerHeader, "updateSceneClipState"));
  EXPECT_TRUE(contains(sceneManager, "createSceneClipStateBuffer"));
  EXPECT_TRUE(contains(sceneManager, "dstBinding = 6"));
  EXPECT_TRUE(contains(sceneManager, "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER"));

  EXPECT_TRUE(contains(pushConstants, "struct BoxClipState"));
  EXPECT_TRUE(contains(pushConstants, "struct SectionClipCapStyle"));
  EXPECT_TRUE(contains(pushConstants, "SECTION_CLIP_CAP_HATCH_DIAGONAL"));
  EXPECT_TRUE(contains(pushConstants, "SECTION_CLIP_CAP_HATCH_CROSS"));
  EXPECT_TRUE(contains(pushConstants, "bool BoxClipPlanesClip"));
  EXPECT_TRUE(contains(pushConstants, "float SectionClipCapHatchMask"));
  EXPECT_TRUE(contains(pushConstants, "float4 fillColor"));
  EXPECT_TRUE(contains(pushConstants, "float4 hatchColor"));
  EXPECT_TRUE(contains(sceneClipCommon, "[[vk::binding(6, 0)]]"));
  EXPECT_TRUE(contains(sceneClipCommon, "SceneClipPlanesClip"));
  EXPECT_TRUE(contains(sceneClipCommon, "BoxClipPlanesClip"));
}

TEST(RenderingConventionTests, BimGenericPropertiesAndGeoreferenceReachUi) {
  const std::string dotBimHeader =
      readRepoTextFile("include/Container/geometry/DotBimLoader.h");
  const std::string bimManagerHeader =
      readRepoTextFile("include/Container/renderer/BimManager.h");
  const std::string bimManager =
      readRepoTextFile("src/renderer/BimManager.cpp");
  const std::string guiManagerHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");
  const std::string guiManager = readRepoTextFile("src/utility/GuiManager.cpp");
  const std::string usdLoader = readRepoTextFile("src/geometry/UsdLoader.cpp");

  EXPECT_TRUE(contains(dotBimHeader, "ElementProperty"));
  EXPECT_TRUE(contains(dotBimHeader, "ModelGeoreferenceMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimElementProperty"));
  EXPECT_TRUE(contains(bimManagerHeader, "BimModelGeoreferenceMetadata"));
  EXPECT_TRUE(contains(bimManagerHeader, "modelGeoreferenceMetadata()"));
  EXPECT_TRUE(
      contains(bimManagerHeader, "std::vector<BimElementProperty> properties"));
  EXPECT_TRUE(contains(bimManager, "bimElementProperties(element.properties)"));
  EXPECT_TRUE(contains(
      bimManager, "bimModelGeoreferenceMetadata(model.georeferenceMetadata)"));

  EXPECT_TRUE(contains(
      guiManagerHeader,
      "std::span<const container::renderer::BimElementProperty> properties"));
  EXPECT_TRUE(contains(guiManagerHeader, "sourceUpAxis"));
  EXPECT_TRUE(contains(guiManagerHeader, "coordinateOffset"));
  EXPECT_TRUE(contains(rendererFrontend, "bimInspection.properties"));
  EXPECT_TRUE(contains(rendererFrontend, "modelGeoreferenceMetadata()"));
  EXPECT_TRUE(contains(guiManager, "BimExtendedProperties"));
  EXPECT_TRUE(contains(guiManager, "Source up axis"));
  EXPECT_TRUE(contains(guiManager, "Coordinate offset"));
  EXPECT_TRUE(contains(guiManager, "CRS:"));
  EXPECT_TRUE(contains(usdLoader, "applySourceUpAxis"));
}
