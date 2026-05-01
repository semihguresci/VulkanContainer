#include "Container/geometry/Model.h"
#include "Container/utility/Platform.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <glm/geometric.hpp>

#ifndef CONTAINER_SOURCE_DIR
#define CONTAINER_SOURCE_DIR "."
#endif

#ifndef CONTAINER_BINARY_DIR
#define CONTAINER_BINARY_DIR "."
#endif

namespace {

using Json = nlohmann::json;

Json readJson(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("unable to open " + path.string());
  }

  std::ostringstream contents;
  contents << file.rdbuf();
  return Json::parse(contents.str());
}

std::filesystem::path manifestPath() {
  return std::filesystem::path(CONTAINER_SOURCE_DIR) / "models" /
         "sample_model_regressions.json";
}

std::filesystem::path sampleModelRoot(const Json& manifest) {
  if (const char* overrideRoot = std::getenv("CONTAINER_SAMPLE_MODEL_ROOT");
      overrideRoot != nullptr && overrideRoot[0] != '\0') {
    return container::util::pathFromUtf8(overrideRoot);
  }

  const std::filesystem::path relativeRoot =
      container::util::pathFromUtf8(
          manifest.at("sampleModelRoot").get<std::string>());

  const std::array candidates = {
      std::filesystem::path(CONTAINER_BINARY_DIR) / relativeRoot,
      std::filesystem::current_path() / relativeRoot,
      std::filesystem::path(CONTAINER_SOURCE_DIR) / relativeRoot,
  };

  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }

  return candidates.front();
}

bool isFiniteVec2(const glm::vec2& value) {
  return std::isfinite(value.x) && std::isfinite(value.y);
}

bool isFiniteVec3(const glm::vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool isFiniteVec4(const glm::vec4& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z) && std::isfinite(value.w);
}

void expectFiniteVertexBasis(const std::string& regressionId,
                             const container::geometry::Model& model) {
  ASSERT_FALSE(model.vertices().empty()) << regressionId;
  ASSERT_FALSE(model.indices().empty()) << regressionId;

  for (size_t i = 0; i < model.vertices().size(); ++i) {
    const auto& vertex = model.vertices()[i];
    EXPECT_TRUE(isFiniteVec3(vertex.position)) << regressionId << " vertex " << i;
    EXPECT_TRUE(isFiniteVec3(vertex.normal)) << regressionId << " vertex " << i;
    EXPECT_TRUE(isFiniteVec4(vertex.tangent)) << regressionId << " vertex " << i;
    EXPECT_TRUE(isFiniteVec2(vertex.texCoord)) << regressionId << " vertex " << i;
    EXPECT_TRUE(isFiniteVec2(vertex.texCoord1)) << regressionId << " vertex " << i;

    const float normalLength = glm::length(vertex.normal);
    EXPECT_NEAR(normalLength, 1.0f, 1e-4f)
        << regressionId << " vertex " << i;

    const float tangentLength = glm::length(glm::vec3(vertex.tangent));
    EXPECT_NEAR(tangentLength, 1.0f, 1e-4f)
        << regressionId << " vertex " << i;
    EXPECT_TRUE(vertex.tangent.w == 1.0f || vertex.tangent.w == -1.0f)
        << regressionId << " vertex " << i;
  }

  const uint32_t vertexCount = static_cast<uint32_t>(model.vertices().size());
  for (size_t i = 0; i < model.indices().size(); ++i) {
    EXPECT_LT(model.indices()[i], vertexCount) << regressionId << " index " << i;
  }
}

void expectPrimitiveMaterialCull(const std::string& regressionId,
                                 const container::geometry::Model& model,
                                 const Json& check) {
  const int32_t materialIndex = check.at("materialIndex").get<int32_t>();
  const bool expectedCullDisabled =
      check.at("disableBackfaceCulling").get<bool>();
  const uint32_t minIndexCount =
      check.value("minIndexCount", static_cast<uint32_t>(0));
  const size_t minCount = check.value("minCount", static_cast<size_t>(1));

  size_t matchingCount = 0;
  for (const auto& primitive : model.primitiveRanges()) {
    if (primitive.materialIndex != materialIndex ||
        primitive.indexCount < minIndexCount) {
      continue;
    }

    ++matchingCount;
    EXPECT_EQ(primitive.disableBackfaceCulling, expectedCullDisabled)
        << regressionId << " material " << materialIndex
        << " indexCount " << primitive.indexCount;
  }

  EXPECT_GE(matchingCount, minCount)
      << regressionId << " material " << materialIndex;
}

void runCheck(const std::string& regressionId,
              const container::geometry::Model& model,
              const Json& check) {
  const std::string kind = check.at("kind").get<std::string>();
  if (kind == "finiteVertexBasis") {
    expectFiniteVertexBasis(regressionId, model);
    return;
  }
  if (kind == "primitiveMaterialCull") {
    expectPrimitiveMaterialCull(regressionId, model, check);
    return;
  }

  FAIL() << "unknown sample model regression check kind: " << kind;
}

}  // namespace

TEST(SampleModelRegressionTests, ModelFolderRegressionAssetsStayStable) {
  const Json manifest = readJson(manifestPath());
  ASSERT_EQ(manifest.at("schemaVersion").get<int>(), 1);

  const std::filesystem::path root = sampleModelRoot(manifest);
  if (!std::filesystem::exists(root)) {
    GTEST_SKIP() << "sample model root is not available: " << root.string();
  }

  for (const Json& regression : manifest.at("regressions")) {
    const std::string regressionId = regression.at("id").get<std::string>();
    const std::filesystem::path assetPath =
        root / container::util::pathFromUtf8(
                   regression.at("asset").get<std::string>());

    ASSERT_TRUE(std::filesystem::exists(assetPath))
        << regressionId << " asset missing: " << assetPath.string();

    const auto model =
        container::geometry::Model::LoadFromGltf(assetPath.string());
    ASSERT_FALSE(model.empty()) << regressionId;
    ASSERT_FALSE(model.primitiveRanges().empty()) << regressionId;

    for (const Json& check : regression.at("checks")) {
      runCheck(regressionId, model, check);
    }
  }
}
