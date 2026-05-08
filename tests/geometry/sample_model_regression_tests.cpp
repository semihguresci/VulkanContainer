#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <glm/geometric.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

#include "Container/geometry/DotBimLoader.h"
#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/geometry/Model.h"
#include "Container/geometry/UsdLoader.h"
#include "Container/utility/Platform.h"

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

std::filesystem::path manifestModelRoot(const Json& manifest,
                                        std::string_view rootKey) {
  if (rootKey == "sampleModelRoot") {
    if (const char* overrideRoot = std::getenv("CONTAINER_SAMPLE_MODEL_ROOT");
        overrideRoot != nullptr && overrideRoot[0] != '\0') {
      return container::util::pathFromUtf8(overrideRoot);
    }
  }

  const std::string rootKeyString(rootKey);
  const std::filesystem::path relativeRoot = container::util::pathFromUtf8(
      manifest.at(rootKeyString).get<std::string>());

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

std::filesystem::path regressionModelRoot(const Json& manifest,
                                          const Json& regression) {
  const std::string rootKey =
      regression.value("root", std::string("sampleModelRoot"));
  return manifestModelRoot(manifest, rootKey);
}

bool regressionRequirementsAvailable(const Json& regression) {
  const std::string requirement = regression.value("requires", std::string{});
  if (requirement.empty()) {
    return true;
  }
  if (requirement == "tinyusdz") {
#if defined(CONTAINER_HAS_TINYUSDZ)
    return true;
#else
    return false;
#endif
  }
  throw std::runtime_error("unknown sample model regression requirement: " +
                           requirement);
}

container::geometry::dotbim::Model loadAuxiliaryModel(
    const std::filesystem::path& path, std::string_view format) {
  if (format == "usd") {
    return container::geometry::usd::LoadFromFile(path);
  }
  if (format == "ifc") {
    return container::geometry::ifc::LoadFromFile(path);
  }
  if (format == "ifcx") {
    return container::geometry::ifcx::LoadFromFile(path);
  }

  throw std::runtime_error("unsupported auxiliary sample format: " +
                           std::string(format));
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

bool isFiniteMat4(const glm::mat4& value) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!std::isfinite(value[column][row])) {
        return false;
      }
    }
  }
  return true;
}

bool isFiniteVertex(const container::geometry::Vertex& vertex) {
  return isFiniteVec3(vertex.position) && isFiniteVec3(vertex.normal) &&
         isFiniteVec4(vertex.tangent) && isFiniteVec2(vertex.texCoord) &&
         isFiniteVec2(vertex.texCoord1) && isFiniteVec3(vertex.color);
}

void expectRenderableAuxiliaryModel(
    const std::string& regressionId,
    const container::geometry::dotbim::Model& model, const Json& check) {
  ASSERT_FALSE(model.vertices.empty()) << regressionId;
  ASSERT_FALSE(model.indices.empty()) << regressionId;
  ASSERT_FALSE(model.meshRanges.empty()) << regressionId;
  ASSERT_FALSE(model.elements.empty()) << regressionId;

  EXPECT_GE(model.vertices.size(), check.value("minVertices", size_t{1}))
      << regressionId;
  EXPECT_GE(model.indices.size(), check.value("minIndices", size_t{1}))
      << regressionId;
  EXPECT_GE(model.meshRanges.size(), check.value("minMeshRanges", size_t{1}))
      << regressionId;
  EXPECT_GE(model.elements.size(), check.value("minElements", size_t{1}))
      << regressionId;
  EXPECT_GE(model.materials.size(), check.value("minMaterials", size_t{0}))
      << regressionId;

  const uint32_t vertexCount = static_cast<uint32_t>(std::min<size_t>(
      model.vertices.size(), std::numeric_limits<uint32_t>::max()));
  for (size_t i = 0; i < model.vertices.size(); ++i) {
    EXPECT_TRUE(isFiniteVertex(model.vertices[i]))
        << regressionId << " vertex " << i;
  }
  for (size_t i = 0; i < model.indices.size(); ++i) {
    EXPECT_LT(model.indices[i], vertexCount) << regressionId << " index " << i;
  }

  std::unordered_set<uint32_t> meshIds;
  meshIds.reserve(model.meshRanges.size());
  for (size_t i = 0; i < model.meshRanges.size(); ++i) {
    const auto& range = model.meshRanges[i];
    meshIds.insert(range.meshId);
    EXPECT_LE(static_cast<size_t>(range.firstIndex) + range.indexCount,
              model.indices.size())
        << regressionId << " mesh range " << i;
    EXPECT_TRUE(isFiniteVec3(range.boundsCenter))
        << regressionId << " mesh range " << i;
    EXPECT_TRUE(std::isfinite(range.boundsRadius))
        << regressionId << " mesh range " << i;
    EXPECT_GE(range.boundsRadius, 0.0f) << regressionId << " mesh range " << i;
  }

  for (size_t i = 0; i < model.elements.size(); ++i) {
    const auto& element = model.elements[i];
    EXPECT_TRUE(meshIds.contains(element.meshId))
        << regressionId << " element " << i;
    EXPECT_TRUE(isFiniteMat4(element.transform))
        << regressionId << " element " << i;
    EXPECT_TRUE(isFiniteVec4(element.color))
        << regressionId << " element " << i;
    if (element.materialIndex != std::numeric_limits<uint32_t>::max()) {
      EXPECT_LT(element.materialIndex, model.materials.size())
          << regressionId << " element " << i;
    }
  }
}

void expectFiniteVertexBasis(const std::string& regressionId,
                             const container::geometry::Model& model) {
  ASSERT_FALSE(model.vertices().empty()) << regressionId;
  ASSERT_FALSE(model.indices().empty()) << regressionId;

  for (size_t i = 0; i < model.vertices().size(); ++i) {
    const auto& vertex = model.vertices()[i];
    EXPECT_TRUE(isFiniteVec3(vertex.position))
        << regressionId << " vertex " << i;
    EXPECT_TRUE(isFiniteVec3(vertex.normal)) << regressionId << " vertex " << i;
    EXPECT_TRUE(isFiniteVec4(vertex.tangent))
        << regressionId << " vertex " << i;
    EXPECT_TRUE(isFiniteVec2(vertex.texCoord))
        << regressionId << " vertex " << i;
    EXPECT_TRUE(isFiniteVec2(vertex.texCoord1))
        << regressionId << " vertex " << i;

    const float normalLength = glm::length(vertex.normal);
    EXPECT_NEAR(normalLength, 1.0f, 1e-4f) << regressionId << " vertex " << i;

    const float tangentLength = glm::length(glm::vec3(vertex.tangent));
    EXPECT_NEAR(tangentLength, 1.0f, 1e-4f) << regressionId << " vertex " << i;
    EXPECT_TRUE(vertex.tangent.w == 1.0f || vertex.tangent.w == -1.0f)
        << regressionId << " vertex " << i;
  }

  const uint32_t vertexCount = static_cast<uint32_t>(model.vertices().size());
  for (size_t i = 0; i < model.indices().size(); ++i) {
    EXPECT_LT(model.indices()[i], vertexCount)
        << regressionId << " index " << i;
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
        << regressionId << " material " << materialIndex << " indexCount "
        << primitive.indexCount;
  }

  EXPECT_GE(matchingCount, minCount)
      << regressionId << " material " << materialIndex;
}

void runCheck(const std::string& regressionId,
              const container::geometry::Model& model, const Json& check) {
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

void runAuxiliaryCheck(const std::string& regressionId,
                       const container::geometry::dotbim::Model& model,
                       const Json& check) {
  const std::string kind = check.at("kind").get<std::string>();
  if (kind == "renderableAuxiliaryModel") {
    expectRenderableAuxiliaryModel(regressionId, model, check);
    return;
  }

  FAIL() << "unknown auxiliary sample model regression check kind: " << kind;
}

}  // namespace

TEST(SampleModelRegressionTests, ModelFolderRegressionAssetsStayStable) {
  const Json manifest = readJson(manifestPath());
  ASSERT_EQ(manifest.at("schemaVersion").get<int>(), 1);

  for (const Json& regression : manifest.at("regressions")) {
    const std::string regressionId = regression.at("id").get<std::string>();
    if (!regressionRequirementsAvailable(regression)) {
      GTEST_SKIP() << regressionId << " requirement is not available";
    }

    const std::filesystem::path root =
        regressionModelRoot(manifest, regression);
    if (!std::filesystem::exists(root)) {
      GTEST_SKIP() << regressionId
                   << " model root is not available: " << root.string();
    }

    const std::filesystem::path assetPath =
        root / container::util::pathFromUtf8(
                   regression.at("asset").get<std::string>());

    ASSERT_TRUE(std::filesystem::exists(assetPath))
        << regressionId << " asset missing: " << assetPath.string();

    const std::string format = regression.value("format", std::string("gltf"));
    if (format == "gltf") {
      const auto model =
          container::geometry::Model::LoadFromGltf(assetPath.string());
      ASSERT_FALSE(model.empty()) << regressionId;
      ASSERT_FALSE(model.primitiveRanges().empty()) << regressionId;

      for (const Json& check : regression.at("checks")) {
        runCheck(regressionId, model, check);
      }
      continue;
    }

    const auto model = loadAuxiliaryModel(assetPath, format);
    for (const Json& check : regression.at("checks")) {
      runAuxiliaryCheck(regressionId, model, check);
    }
  }
}
