#include "Container/renderer/BimManager.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/Model.h"
#include "Container/geometry/UsdLoader.h"
#include "Container/renderer/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Material.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneManager.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <optional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

namespace container::renderer {
namespace {

float sanitizeImportScale(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(scale, 0.001f, 1000.0f);
}

std::string lowerAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

glm::mat4 importScaleTransform(float scale) {
  return glm::scale(glm::mat4(1.0f), glm::vec3(sanitizeImportScale(scale)));
}

bool transformFlipsWinding(const glm::mat4& transform) {
  const glm::vec3 x(transform[0]);
  const glm::vec3 y(transform[1]);
  const glm::vec3 z(transform[2]);
  const float determinant =
      x.x * (y.y * z.z - y.z * z.y) -
      y.x * (x.y * z.z - x.z * z.y) +
      z.x * (x.y * y.z - x.z * y.y);
  return determinant < 0.0f;
}

container::gpu::ObjectData makeObjectData(const glm::mat4& transform,
                                          uint32_t materialIndex,
                                          bool doubleSided,
                                          glm::vec3 boundsCenter,
                                          float boundsRadius) {
  container::gpu::ObjectData object{};
  object.model = transform;
  const glm::mat3 normalMatrix =
      glm::transpose(glm::inverse(glm::mat3(transform)));
  object.normalMatrix0 = glm::vec4(normalMatrix[0], 0.0f);
  object.normalMatrix1 = glm::vec4(normalMatrix[1], 0.0f);
  object.normalMatrix2 = glm::vec4(normalMatrix[2], 0.0f);
  object.objectInfo.x = materialIndex;
  object.objectInfo.y =
      doubleSided ? container::gpu::kObjectFlagDoubleSided : 0u;

  const glm::vec3 worldCenter =
      glm::vec3(transform * glm::vec4(boundsCenter, 1.0f));
  const float scaleMax = std::max({
      glm::length(glm::vec3(transform[0])),
      glm::length(glm::vec3(transform[1])),
      glm::length(glm::vec3(transform[2]))});
  object.boundingSphere = glm::vec4(worldCenter, boundsRadius * scaleMax);
  return object;
}

uint32_t packColor(glm::vec4 color) {
  auto pack = [](float component) {
    return static_cast<uint32_t>(
        std::clamp(std::lround(component * 255.0f), 0l, 255l));
  };
  return pack(color.r) | (pack(color.g) << 8u) | (pack(color.b) << 16u) |
         (pack(color.a) << 24u);
}

bool isTransparentColor(const glm::vec4& color) {
  return std::isfinite(color.a) && color.a < 0.999f;
}

constexpr uint32_t kInvalidMaterialIndex =
    std::numeric_limits<uint32_t>::max();

bool isValidMaterialIndex(
    uint32_t index,
    const container::geometry::dotbim::Model& model) {
  return index != kInvalidMaterialIndex && index < model.materials.size();
}

std::optional<uint32_t> tryLoadSceneTexture(
    container::scene::SceneManager& sceneManager,
    const container::geometry::dotbim::MaterialTextureAsset& texture,
    bool isSrgb) {
  if (texture.empty()) {
    return std::nullopt;
  }

  if (!texture.encodedBytes.empty()) {
    try {
      std::string textureName = texture.name;
      if (textureName.empty() && !texture.path.empty()) {
        textureName = container::util::pathToUtf8(texture.path.lexically_normal());
      }
      return sceneManager.loadMaterialTextureFromBytes(
          textureName, texture.encodedBytes, isSrgb, texture.samplerIndex);
    } catch (...) {
      if (texture.path.empty()) {
        return std::nullopt;
      }
    }
  }

  const std::filesystem::path normalized = texture.path.lexically_normal();
  std::error_code existsError;
  if (!std::filesystem::exists(normalized, existsError)) {
    return std::nullopt;
  }
  try {
    return sceneManager.loadMaterialTexture(normalized, isSrgb,
                                            texture.samplerIndex);
  } catch (...) {
    return std::nullopt;
  }
}

void assignTextureIndex(container::scene::SceneManager& sceneManager,
                        const container::geometry::dotbim::MaterialTextureAsset& texture,
                        bool isSrgb,
                        uint32_t& textureIndex) {
  if (const auto loaded = tryLoadSceneTexture(sceneManager, texture, isSrgb)) {
    textureIndex = *loaded;
  }
}

container::material::Material makeSceneMaterial(
    const container::geometry::dotbim::Material& source,
    container::scene::SceneManager& sceneManager) {
  container::material::Material material = source.pbr;
  material.baseColor =
      glm::clamp(material.baseColor, glm::vec4(0.0f), glm::vec4(1.0f));
  material.opacityFactor = std::clamp(material.opacityFactor, 0.0f, 1.0f);
  material.metallicFactor = std::clamp(material.metallicFactor, 0.0f, 1.0f);
  material.roughnessFactor = std::clamp(material.roughnessFactor, 0.0f, 1.0f);
  material.alphaCutoff = std::clamp(material.alphaCutoff, 0.0f, 1.0f);
  material.normalTextureScale =
      std::isfinite(material.normalTextureScale)
          ? material.normalTextureScale
          : 1.0f;
  material.occlusionStrength =
      std::clamp(material.occlusionStrength, 0.0f, 1.0f);

  assignTextureIndex(sceneManager, source.texturePaths.baseColor, true,
                     material.baseColorTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.emissive, true,
                     material.emissiveTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.normal, false,
                     material.normalTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.occlusion, false,
                     material.occlusionTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.metallicRoughness, false,
                     material.metallicRoughnessTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.roughness, false,
                     material.roughnessTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.metalness, false,
                     material.metalnessTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.specular, false,
                     material.specularTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.specularColor, true,
                     material.specularColorTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.opacity, false,
                     material.opacityTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.transmission, false,
                     material.transmissionTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.clearcoat, false,
                     material.clearcoatTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.clearcoatRoughness,
                     false, material.clearcoatRoughnessTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.clearcoatNormal, false,
                     material.clearcoatNormalTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.sheenColor, true,
                     material.sheenColorTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.sheenRoughness, false,
                     material.sheenRoughnessTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.iridescence, false,
                     material.iridescenceTextureIndex);
  assignTextureIndex(sceneManager, source.texturePaths.iridescenceThickness,
                     false, material.iridescenceThicknessTextureIndex);
  return material;
}

bool hasRenderableSourceGeometry(
    const container::geometry::dotbim::Model& model) {
  return !model.vertices.empty() && !model.indices.empty() &&
         !model.meshRanges.empty() && !model.elements.empty();
}

std::string modelLoadErrorPrefix(std::string_view format,
                                 const std::filesystem::path& path) {
  return std::string(format) + " file has no supported BIM geometry: " +
         container::util::pathToUtf8(path);
}

}  // namespace

BimManager::BimManager(container::gpu::AllocationManager& allocationManager)
    : allocationManager_(allocationManager) {}

BimManager::~BimManager() { clear(); }

void BimManager::clear() {
  if (vertexBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(vertexBuffer_);
  }
  if (indexBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(indexBuffer_);
  }
  if (objectBuffer_.buffer != VK_NULL_HANDLE) {
    allocationManager_.destroyBuffer(objectBuffer_);
  }
  objectBufferCapacity_ = 0;
  vertexSlice_ = {};
  indexSlice_ = {};
  objectData_.clear();
  opaqueDrawCommands_.clear();
  opaqueSingleSidedDrawCommands_.clear();
  opaqueWindingFlippedDrawCommands_.clear();
  opaqueDoubleSidedDrawCommands_.clear();
  transparentDrawCommands_.clear();
  transparentSingleSidedDrawCommands_.clear();
  transparentWindingFlippedDrawCommands_.clear();
  transparentDoubleSidedDrawCommands_.clear();
  modelPath_.clear();
  ++objectDataRevision_;
}

bool BimManager::hasScene() const {
  return vertexSlice_.buffer != VK_NULL_HANDLE &&
         indexSlice_.buffer != VK_NULL_HANDLE &&
         objectBuffer_.buffer != VK_NULL_HANDLE &&
         !objectData_.empty() &&
          (!opaqueDrawCommands_.empty() ||
           !opaqueSingleSidedDrawCommands_.empty() ||
           !opaqueWindingFlippedDrawCommands_.empty() ||
           !opaqueDoubleSidedDrawCommands_.empty() ||
           !transparentDrawCommands_.empty() ||
           !transparentSingleSidedDrawCommands_.empty() ||
           !transparentWindingFlippedDrawCommands_.empty() ||
           !transparentDoubleSidedDrawCommands_.empty());
}

VkDeviceSize BimManager::objectBufferSize() const {
  return static_cast<VkDeviceSize>(
      sizeof(container::gpu::ObjectData) * objectBufferCapacity_);
}

std::filesystem::path BimManager::resolveModelPath(
    const std::string& path) const {
  std::filesystem::path resolved = container::util::pathFromUtf8(path);
  if (!resolved.is_relative() || std::filesystem::exists(resolved)) {
    return resolved;
  }

  const std::filesystem::path exeRelative =
      container::util::executableDirectory() / resolved;
  if (std::filesystem::exists(exeRelative)) {
    return exeRelative;
  }
  return resolved;
}

void BimManager::loadModel(const std::string& path,
                           float importScale,
                           container::scene::SceneManager& sceneManager) {
  clear();
  if (path.empty()) {
    return;
  }

  const std::filesystem::path resolvedPath = resolveModelPath(path);
  const std::string extension = lowerAscii(resolvedPath.extension().string());
  if (extension == ".bim") {
    loadDotBim(resolvedPath, importScale, sceneManager);
  } else if (extension == ".ifc") {
    loadIfc(resolvedPath, importScale, sceneManager);
  } else if (extension == ".ifcx") {
    loadPreparedModel(
        container::geometry::ifcx::LoadFromFile(resolvedPath, importScale),
        resolvedPath, "IFCX", sceneManager);
  } else if (extension == ".usd" || extension == ".usda" ||
             extension == ".usdc" || extension == ".usdz") {
    loadPreparedModel(
        container::geometry::usd::LoadFromFile(resolvedPath, importScale),
        resolvedPath, "USD", sceneManager);
  } else if (extension == ".gltf" || extension == ".glb") {
    loadGltfFallback(resolvedPath, importScale, sceneManager);
  } else {
    throw std::runtime_error(
        "unsupported BIM model format '" + extension +
        "'; supported BIM sources are .bim, .ifc, .ifcx, .usd, .usda, "
        ".usdc, .usdz, .gltf, and .glb");
  }
  modelPath_ = path;
}

void BimManager::loadDotBim(
    const std::filesystem::path& path,
    float importScale,
    container::scene::SceneManager& sceneManager) {
  const auto model = container::geometry::dotbim::LoadFromFile(path, importScale);
  loadPreparedModel(model, path, "dotbim", sceneManager);
}

void BimManager::loadIfc(
    const std::filesystem::path& path,
    float importScale,
    container::scene::SceneManager& sceneManager) {
  const auto model = container::geometry::ifc::LoadFromFile(path, importScale);
  loadPreparedModel(model, path, "IFC", sceneManager);
}

void BimManager::loadPreparedModel(
    const container::geometry::dotbim::Model& model,
    const std::filesystem::path& path,
    std::string_view format,
    container::scene::SceneManager& sceneManager) {
  if (!hasRenderableSourceGeometry(model)) {
    throw std::runtime_error(modelLoadErrorPrefix(format, path));
  }
  uploadGeometry(model.vertices, model.indices);
  buildDrawDataFromModel(model, sceneManager);
  if (!hasScene()) {
    clear();
    throw std::runtime_error(modelLoadErrorPrefix(format, path));
  }
}

void BimManager::buildDrawDataFromModel(
    const container::geometry::dotbim::Model& model,
    container::scene::SceneManager& sceneManager) {
  std::unordered_map<uint32_t, uint32_t> colorMaterialCache;
  colorMaterialCache.reserve(model.elements.size());
  std::vector<uint32_t> sourceMaterialCache(model.materials.size(),
                                            kInvalidMaterialIndex);
  std::unordered_map<uint32_t, MeshRange> rangesByMeshId;
  rangesByMeshId.reserve(model.meshRanges.size());
  for (const auto& sourceRange : model.meshRanges) {
    rangesByMeshId.emplace(
        sourceRange.meshId,
        MeshRange{sourceRange.meshId, sourceRange.firstIndex,
                  sourceRange.indexCount, sourceRange.boundsCenter,
                  sourceRange.boundsRadius});
  }

  objectData_.reserve(model.elements.size());
  opaqueDrawCommands_.reserve(model.elements.size());
  opaqueSingleSidedDrawCommands_.reserve(model.elements.size());
  opaqueWindingFlippedDrawCommands_.reserve(model.elements.size());
  opaqueDoubleSidedDrawCommands_.reserve(model.elements.size());
  transparentDrawCommands_.reserve(model.elements.size());
  transparentSingleSidedDrawCommands_.reserve(model.elements.size());
  transparentWindingFlippedDrawCommands_.reserve(model.elements.size());
  transparentDoubleSidedDrawCommands_.reserve(model.elements.size());

  for (const auto& element : model.elements) {
    const auto rangeIt = rangesByMeshId.find(element.meshId);
    if (rangeIt == rangesByMeshId.end() || rangeIt->second.indexCount == 0u) {
      continue;
    }

    glm::vec4 color = glm::clamp(element.color, glm::vec4(0.0f),
                                 glm::vec4(1.0f));
    uint32_t materialIndex = kInvalidMaterialIndex;
    bool transparent = isTransparentColor(color);
    if (isValidMaterialIndex(element.materialIndex, model)) {
      materialIndex = sourceMaterialCache[element.materialIndex];
      if (materialIndex == kInvalidMaterialIndex) {
        const auto& sourceMaterial = model.materials[element.materialIndex];
        materialIndex = sceneManager.createMaterial(
            makeSceneMaterial(sourceMaterial, sceneManager));
        sourceMaterialCache[element.materialIndex] = materialIndex;
      }
      const auto properties =
          sceneManager.materialRenderProperties(materialIndex);
      transparent = transparent || properties.transparent;
    } else {
      const auto alphaMode = transparent
                                 ? container::material::AlphaMode::Blend
                                 : container::material::AlphaMode::Opaque;
      const uint32_t colorKey = packColor(color);
      auto materialIt = colorMaterialCache.find(colorKey);
      if (materialIt == colorMaterialCache.end()) {
        const uint32_t newMaterialIndex =
            sceneManager.createSolidMaterial(color, true, alphaMode);
        materialIt =
            colorMaterialCache.emplace(colorKey, newMaterialIndex).first;
      }
      materialIndex = materialIt->second;
    }

    const auto& range = rangeIt->second;
    const bool doubleSided = element.doubleSided;
    const bool windingFlipped = !doubleSided &&
                                transformFlipsWinding(element.transform);
    const uint32_t objectIndex = static_cast<uint32_t>(objectData_.size());
    objectData_.push_back(makeObjectData(
        element.transform, materialIndex, doubleSided, range.boundsCenter,
        range.boundsRadius));

    DrawCommand command{};
    command.objectIndex = objectIndex;
    command.firstIndex = range.firstIndex;
    command.indexCount = range.indexCount;
    if (transparent) {
      transparentDrawCommands_.push_back(command);
      if (doubleSided) {
        transparentDoubleSidedDrawCommands_.push_back(command);
      } else if (windingFlipped) {
        transparentWindingFlippedDrawCommands_.push_back(command);
      } else {
        transparentSingleSidedDrawCommands_.push_back(command);
      }
    } else {
      opaqueDrawCommands_.push_back(command);
      if (doubleSided) {
        opaqueDoubleSidedDrawCommands_.push_back(command);
      } else if (windingFlipped) {
        opaqueWindingFlippedDrawCommands_.push_back(command);
      } else {
        opaqueSingleSidedDrawCommands_.push_back(command);
      }
    }
  }

  if (objectData_.empty()) {
    return;
  }
  sceneManager.uploadMaterialResources();
  uploadObjects();
}

void BimManager::loadGltfFallback(
    const std::filesystem::path& path,
    float importScale,
    container::scene::SceneManager& sceneManager) {
  const auto model =
      container::geometry::Model::LoadFromGltf(container::util::pathToUtf8(path));
  if (model.vertices().empty() || model.indices().empty() ||
      model.primitiveRanges().empty()) {
    throw std::runtime_error(modelLoadErrorPrefix("glTF", path));
  }
  uploadGeometry(model.vertices(), model.indices());

  const glm::mat4 transform = importScaleTransform(importScale);
  const uint32_t materialIndex = sceneManager.defaultMaterialIndex();
  objectData_.push_back(makeObjectData(transform, materialIndex, false,
                                       glm::vec3(0.0f), 0.0f));

  opaqueDrawCommands_.reserve(model.primitiveRanges().size());
  opaqueSingleSidedDrawCommands_.reserve(model.primitiveRanges().size());
  opaqueDoubleSidedDrawCommands_.reserve(model.primitiveRanges().size());
  for (const auto& primitive : model.primitiveRanges()) {
    if (primitive.indexCount == 0u) {
      continue;
    }

    DrawCommand command{};
    command.objectIndex = 0u;
    command.firstIndex = primitive.firstIndex;
    command.indexCount = primitive.indexCount;
    opaqueDrawCommands_.push_back(command);
    if (primitive.disableBackfaceCulling) {
      opaqueDoubleSidedDrawCommands_.push_back(command);
    } else if (transformFlipsWinding(transform)) {
      opaqueWindingFlippedDrawCommands_.push_back(command);
    } else {
      opaqueSingleSidedDrawCommands_.push_back(command);
    }
  }

  uploadObjects();
  if (!hasScene()) {
    clear();
    throw std::runtime_error(modelLoadErrorPrefix("glTF", path));
  }
}

void BimManager::uploadGeometry(
    std::span<const container::geometry::Vertex> vertices,
    std::span<const uint32_t> indices) {
  if (vertices.empty() || indices.empty()) {
    return;
  }

  const VkDeviceSize vertexBufferSize =
      static_cast<VkDeviceSize>(sizeof(container::geometry::Vertex) *
                                vertices.size());
  const VkDeviceSize indexBufferSize =
      static_cast<VkDeviceSize>(sizeof(uint32_t) * indices.size());

  vertexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte*>(vertices.data()),
       static_cast<size_t>(vertexBufferSize)},
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  indexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte*>(indices.data()),
       static_cast<size_t>(indexBufferSize)},
      VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

  vertexSlice_ = {vertexBuffer_.buffer, 0, vertexBufferSize};
  indexSlice_ = {indexBuffer_.buffer, 0, indexBufferSize};
}

void BimManager::uploadObjects() {
  const bool recreated = SceneController::ensureObjectBufferCapacity(
      allocationManager_, objectBuffer_, objectBufferCapacity_,
      objectData_.size());
  (void)recreated;
  if (objectBuffer_.buffer == VK_NULL_HANDLE || objectData_.empty()) {
    return;
  }

  SceneController::writeToBuffer(allocationManager_, objectBuffer_,
                                 objectData_.data(),
                                 sizeof(container::gpu::ObjectData) *
                                     objectData_.size());
  ++objectDataRevision_;
}

}  // namespace container::renderer
