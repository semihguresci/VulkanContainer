#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using Json = nlohmann::json;

std::filesystem::path repositoryRoot() {
  std::filesystem::path candidate =
      std::filesystem::absolute(__FILE__).parent_path().parent_path();
  if (std::filesystem::exists(candidate / "tests" / "fixtures" / "rendering")) {
    return candidate;
  }

  candidate = std::filesystem::current_path();
  while (!candidate.empty()) {
    if (std::filesystem::exists(candidate / "tests" / "fixtures" /
                                "rendering")) {
      return candidate;
    }
    candidate = candidate.parent_path();
  }

  return std::filesystem::absolute(__FILE__).parent_path().parent_path();
}

Json readJsonFixture(const std::filesystem::path &relativePath) {
  const std::filesystem::path path = repositoryRoot() / relativePath;
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << "Unable to open " << path.string();
  std::ostringstream contents;
  contents << file.rdbuf();
  return Json::parse(contents.str());
}

bool isSafeRelativePath(std::string_view value) {
  const std::filesystem::path path{std::string(value)};
  if (path.is_absolute()) {
    return false;
  }
  return value.find("..") == std::string_view::npos &&
         value.find('\\') == std::string_view::npos;
}

bool isLowerSnakeId(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

bool hasRequiredObjectFields(const Json &object,
                             std::initializer_list<std::string_view> fields) {
  return std::all_of(fields.begin(), fields.end(), [&](std::string_view field) {
    return object.contains(std::string(field));
  });
}

double pointLightAttenuation(double distanceMeters, double rangeMeters) {
  const double distanceSq = distanceMeters * distanceMeters;
  double smoothCutoff = 1.0;
  if (std::isfinite(rangeMeters) && rangeMeters > 0.0) {
    const double normalizedSq = distanceSq / (rangeMeters * rangeMeters);
    const double smoothFactor = std::clamp(1.0 - normalizedSq, 0.0, 1.0);
    smoothCutoff = smoothFactor * smoothFactor;
  }
  return smoothCutoff / std::max(distanceSq, 0.01);
}

double snapToTexel(double value, double texelSize) {
  return std::round(value / texelSize) * texelSize;
}

} // namespace

TEST(RealisticRenderingValidation, FixtureSchemaAndMetadataArePresent) {
  const Json schema = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.schema.json");
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");

  ASSERT_EQ(schema.at("properties").at("schemaVersion").at("const").get<int>(),
            1);
  ASSERT_EQ(fixtures.at("schemaVersion").get<int>(), 1);
  EXPECT_EQ(fixtures.at("screenshotRoot").get<std::string>(),
            "tests/visual-regression");

  EXPECT_TRUE(hasRequiredObjectFields(schema, {
                                                  "$schema",
                                                  "$id",
                                                  "required",
                                                  "properties",
                                                  "$defs",
                                              }));
  EXPECT_TRUE(hasRequiredObjectFields(fixtures, {
                                                    "capture",
                                                    "comparison",
                                                    "cpuChecks",
                                                    "scenes",
                                                }));
}

TEST(RealisticRenderingValidation, ScreenshotScenesFollowStableContract) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");
  const Json &capture = fixtures.at("capture");
  const Json &comparison = fixtures.at("comparison");
  const Json &scenes = fixtures.at("scenes");

  ASSERT_TRUE(capture.at("resolution").is_array());
  ASSERT_EQ(capture.at("resolution").size(), 2u);
  EXPECT_EQ(capture.at("format").get<std::string>(), "png");
  EXPECT_GE(capture.at("warmupFrames").get<int>(), 2);
  EXPECT_GT(capture.at("captureFrame").get<int>(),
            capture.at("warmupFrames").get<int>());

  EXPECT_GT(comparison.at("maxMeanAbsoluteError").get<double>(), 0.0);
  EXPECT_GE(comparison.at("maxP95AbsoluteError").get<double>(),
            comparison.at("maxMeanAbsoluteError").get<double>());
  EXPECT_GT(comparison.at("maxDifferentPixelFraction").get<double>(), 0.0);
  EXPECT_LE(comparison.at("maxDifferentPixelFraction").get<double>(), 1.0);
  ASSERT_FALSE(comparison.at("platformBuckets").empty());

  ASSERT_GE(scenes.size(), 3u);
  std::set<std::string> sceneIds;
  std::set<std::string> categories;
  size_t activeSceneCount = 0;
  for (const Json &scene : scenes) {
    ASSERT_TRUE(hasRequiredObjectFields(scene, {
                                                   "id",
                                                   "category",
                                                   "status",
                                                   "asset",
                                                   "renderMode",
                                                   "camera",
                                                   "lighting",
                                                   "probes",
                                                   "screenshots",
                                               }));

    const std::string id = scene.at("id").get<std::string>();
    const std::string status = scene.at("status").get<std::string>();
    EXPECT_TRUE(status == "active" || status == "planned") << status;
    if (status == "active") {
      ++activeSceneCount;
    }
    EXPECT_TRUE(sceneIds.insert(id).second) << "duplicate scene id: " << id;
    EXPECT_TRUE(isLowerSnakeId(id)) << id;
    EXPECT_EQ(id.find('-'), std::string::npos)
        << "scene ids should use underscores, not dashes";
    categories.insert(scene.at("category").get<std::string>());

    EXPECT_TRUE(isSafeRelativePath(scene.at("asset").get<std::string>()));
    EXPECT_FALSE(scene.at("probes").empty());
    EXPECT_GT(scene.at("camera").at("verticalFovDegrees").get<double>(), 0.0);
    EXPECT_GT(scene.at("lighting").at("exposure").get<double>(), 0.0);
    EXPECT_GE(scene.at("lighting").at("environmentIntensity").get<double>(),
              0.0);

    const Json &screenshots = scene.at("screenshots");
    for (const std::string field : {"golden", "candidate", "diff"}) {
      const std::string path = screenshots.at(field).get<std::string>();
      EXPECT_TRUE(isSafeRelativePath(path)) << path;
      EXPECT_NE(path.find(id), std::string::npos) << path;
      EXPECT_NE(path.find(".png"), std::string::npos) << path;
    }
    EXPECT_NE(screenshots.at("golden").get<std::string>().find(
                  fixtures.at("screenshotRoot").get<std::string>()),
              std::string::npos);
    EXPECT_NE(screenshots.at("candidate")
                  .get<std::string>()
                  .find("test_results/visual-regression"),
              std::string::npos);
    EXPECT_NE(screenshots.at("diff").get<std::string>().find(
                  "test_results/visual-regression"),
              std::string::npos);
  }

  EXPECT_TRUE(categories.contains("material"));
  EXPECT_TRUE(categories.contains("exposure"));
  EXPECT_TRUE(categories.contains("lighting"));
  EXPECT_TRUE(categories.contains("shadow"));
  EXPECT_GE(activeSceneCount, 1u)
      << "GPU visual harness should always have at least one active fixture";
}

TEST(RealisticRenderingValidation, FixtureCpuChecksMatchRenderingMath) {
  const Json fixtures = readJsonFixture(
      "tests/fixtures/rendering/realistic_visual_regression.fixtures.json");
  const Json &cpuChecks = fixtures.at("cpuChecks");

  for (const Json &sample : cpuChecks.at("manualExposure")) {
    const double luminance = sample.at("inputLuminance").get<double>();
    const double exposure = sample.at("exposure").get<double>();
    const double expected = sample.at("expectedLuminance").get<double>();
    EXPECT_NEAR(luminance * exposure, expected, 1e-12);
  }

  const Json &attenuation = cpuChecks.at("pointLightAttenuation");
  const double nearAttenuation =
      pointLightAttenuation(attenuation.at("nearDistanceMeters").get<double>(),
                            attenuation.at("rangeMeters").get<double>());
  const double farAttenuation =
      pointLightAttenuation(attenuation.at("farDistanceMeters").get<double>(),
                            attenuation.at("rangeMeters").get<double>());
  ASSERT_GT(farAttenuation, 0.0);
  EXPECT_NEAR(nearAttenuation / farAttenuation,
              attenuation.at("expectedNearToFarRatio").get<double>(), 1e-3);

  const Json &shadow = cpuChecks.at("shadowTexelSnap");
  const double texelSize =
      (shadow.at("cascadeWorldRadius").get<double>() * 2.0) /
      shadow.at("shadowMapSize").get<double>();
  EXPECT_NEAR(texelSize, shadow.at("expectedTexelSize").get<double>(), 1e-12);

  const Json &center = shadow.at("unsnappedLightSpaceCenter");
  const double snappedX = snapToTexel(center.at(0).get<double>(), texelSize);
  const double snappedY = snapToTexel(center.at(1).get<double>(), texelSize);
  const double maxError =
      shadow.at("maxSnapErrorTexels").get<double>() * texelSize;
  EXPECT_LE(std::abs(snappedX - center.at(0).get<double>()), maxError);
  EXPECT_LE(std::abs(snappedY - center.at(1).get<double>()), maxError);

  for (const Json &material : cpuChecks.at("materialExpectations")) {
    ASSERT_TRUE(hasRequiredObjectFields(material, {
                                                      "name",
                                                      "baseColor",
                                                      "metallic",
                                                      "roughness",
                                                      "expectedClass",
                                                  }));
    EXPECT_EQ(material.at("baseColor").size(), 3u);
    for (const Json &channel : material.at("baseColor")) {
      EXPECT_GE(channel.get<double>(), 0.0);
      EXPECT_LE(channel.get<double>(), 1.0);
    }
    EXPECT_GE(material.at("metallic").get<double>(), 0.0);
    EXPECT_LE(material.at("metallic").get<double>(), 1.0);
    EXPECT_GE(material.at("roughness").get<double>(), 0.0);
    EXPECT_LE(material.at("roughness").get<double>(), 1.0);
  }
}
