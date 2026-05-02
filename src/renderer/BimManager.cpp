#include "Container/renderer/BimManager.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/geometry/Model.h"
#include "Container/geometry/UsdLoader.h"
#include "Container/renderer/SceneController.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Material.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
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

bool transformFlipsWinding(const glm::mat4 &transform) {
  const glm::vec3 x(transform[0]);
  const glm::vec3 y(transform[1]);
  const glm::vec3 z(transform[2]);
  const float determinant = x.x * (y.y * z.z - y.z * z.y) -
                            y.x * (x.y * z.z - x.z * z.y) +
                            z.x * (x.y * y.z - x.z * y.y);
  return determinant < 0.0f;
}

bool intersectRaySphere(const glm::vec3& origin, const glm::vec3& direction,
                        const glm::vec3& center, float radius,
                        float& outDistance) {
  if (radius <= 0.0f) {
    return false;
  }

  const glm::vec3 toCenter = center - origin;
  const float projected = glm::dot(toCenter, direction);
  const float centerDistance2 = glm::dot(toCenter, toCenter);
  const float radius2 = radius * radius;
  const float closestDistance2 = centerDistance2 - projected * projected;
  if (closestDistance2 > radius2) {
    return false;
  }

  const float halfChord =
      std::sqrt(std::max(0.0f, radius2 - closestDistance2));
  const float t0 = projected - halfChord;
  const float t1 = projected + halfChord;
  if (t1 < 0.0f) {
    return false;
  }

  outDistance = t0 >= 0.0f ? t0 : 0.0f;
  return true;
}

enum class TriangleCullMode {
  None,
  Back,
  Front,
};

struct PickRay {
  glm::vec3 origin{0.0f};
  glm::vec3 direction{0.0f, 0.0f, -1.0f};
  bool valid{false};
};

PickRay makePickRay(const container::gpu::CameraData& cameraData,
                    VkExtent2D viewportExtent,
                    double cursorX,
                    double cursorY) {
  if (viewportExtent.width == 0u || viewportExtent.height == 0u) {
    return {};
  }

  const float ndcX =
      static_cast<float>((cursorX / static_cast<double>(viewportExtent.width)) *
                             2.0 -
                         1.0);
  const float ndcY =
      static_cast<float>(1.0 -
                         (cursorY / static_cast<double>(viewportExtent.height)) *
                             2.0);

  const glm::vec4 nearClip{ndcX, ndcY, 1.0f, 1.0f};
  const glm::vec4 farClip{ndcX, ndcY, 0.0f, 1.0f};
  glm::vec4 nearWorld = cameraData.inverseViewProj * nearClip;
  glm::vec4 farWorld = cameraData.inverseViewProj * farClip;
  if (nearWorld.w == 0.0f || farWorld.w == 0.0f) {
    return {};
  }
  nearWorld /= nearWorld.w;
  farWorld /= farWorld.w;

  PickRay ray{};
  ray.origin = glm::vec3(nearWorld);
  const glm::vec3 farPoint = glm::vec3(farWorld);
  ray.direction = farPoint - ray.origin;
  const float rayLength = glm::length(ray.direction);
  if (rayLength <= 0.0001f) {
    return {};
  }
  ray.direction /= rayLength;
  ray.valid = true;
  return ray;
}

bool intersectRayTriangle(const glm::vec3& origin,
                          const glm::vec3& direction,
                          const glm::vec3& v0,
                          const glm::vec3& v1,
                          const glm::vec3& v2,
                          TriangleCullMode cullMode,
                          float& outDistance) {
  constexpr float kEpsilon = 0.0000001f;
  const glm::vec3 edge1 = v1 - v0;
  const glm::vec3 edge2 = v2 - v0;
  const glm::vec3 pvec = glm::cross(direction, edge2);
  const float determinant = glm::dot(edge1, pvec);
  if ((cullMode == TriangleCullMode::Back && determinant <= kEpsilon) ||
      (cullMode == TriangleCullMode::Front && determinant >= -kEpsilon) ||
      (cullMode == TriangleCullMode::None &&
       std::abs(determinant) <= kEpsilon)) {
    return false;
  }

  const float inverseDeterminant = 1.0f / determinant;
  const glm::vec3 tvec = origin - v0;
  const float u = glm::dot(tvec, pvec) * inverseDeterminant;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }

  const glm::vec3 qvec = glm::cross(tvec, edge1);
  const float v = glm::dot(direction, qvec) * inverseDeterminant;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }

  const float distance = glm::dot(edge2, qvec) * inverseDeterminant;
  if (distance < 0.0f) {
    return false;
  }

  outDistance = distance;
  return true;
}

float projectDepth(const container::gpu::CameraData& cameraData,
                   const glm::vec3& worldPosition) {
  const glm::vec4 clip =
      cameraData.viewProj * glm::vec4(worldPosition, 1.0f);
  if (clip.w == 0.0f) {
    return 0.0f;
  }
  return std::clamp(clip.z / clip.w, 0.0f, 1.0f);
}

container::gpu::ObjectData
makeObjectData(const glm::mat4 &transform, uint32_t materialIndex,
               bool doubleSided, glm::vec3 boundsCenter, float boundsRadius) {
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
  object.objectInfo.z = container::gpu::kPickIdBimMask;

  const glm::vec3 worldCenter =
      glm::vec3(transform * glm::vec4(boundsCenter, 1.0f));
  const float scaleMax = std::max({glm::length(glm::vec3(transform[0])),
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

bool isTransparentColor(const glm::vec4 &color) {
  return std::isfinite(color.a) && color.a < 0.999f;
}

constexpr uint32_t kInvalidMaterialIndex = std::numeric_limits<uint32_t>::max();

bool isValidMaterialIndex(uint32_t index,
                          const container::geometry::dotbim::Model &model) {
  return index != kInvalidMaterialIndex && index < model.materials.size();
}

enum class DrawBucket : uint8_t {
  OpaqueSingleSided,
  OpaqueWindingFlipped,
  OpaqueDoubleSided,
  TransparentSingleSided,
  TransparentWindingFlipped,
  TransparentDoubleSided,
};

struct PendingDraw {
  container::gpu::ObjectData object{};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  DrawBucket bucket{DrawBucket::OpaqueSingleSided};
};

uint8_t bucketSortKey(DrawBucket bucket) {
  return static_cast<uint8_t>(bucket);
}

void appendDrawCommand(std::vector<DrawCommand> &commands,
                       uint32_t objectIndex, uint32_t firstIndex,
                       uint32_t indexCount, bool allowMerge) {
  if (indexCount == 0u) {
    return;
  }
  if (allowMerge && !commands.empty()) {
    DrawCommand &last = commands.back();
    const uint32_t lastInstanceCount = std::max(last.instanceCount, 1u);
    if (last.firstIndex == firstIndex && last.indexCount == indexCount &&
        last.objectIndex + lastInstanceCount == objectIndex &&
        lastInstanceCount < std::numeric_limits<uint32_t>::max()) {
      last.instanceCount = lastInstanceCount + 1u;
      return;
    }
  }

  DrawCommand command{};
  command.objectIndex = objectIndex;
  command.firstIndex = firstIndex;
  command.indexCount = indexCount;
  command.instanceCount = 1u;
  commands.push_back(command);
}

std::optional<uint32_t> tryLoadSceneTexture(
    container::scene::SceneManager &sceneManager,
    const container::geometry::dotbim::MaterialTextureAsset &texture,
    bool isSrgb) {
  if (texture.empty()) {
    return std::nullopt;
  }

  if (!texture.encodedBytes.empty()) {
    try {
      std::string textureName = texture.name;
      if (textureName.empty() && !texture.path.empty()) {
        textureName =
            container::util::pathToUtf8(texture.path.lexically_normal());
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

void assignTextureIndex(
    container::scene::SceneManager &sceneManager,
    const container::geometry::dotbim::MaterialTextureAsset &texture,
    bool isSrgb, uint32_t &textureIndex) {
  if (const auto loaded = tryLoadSceneTexture(sceneManager, texture, isSrgb)) {
    textureIndex = *loaded;
  }
}

container::material::Material
makeSceneMaterial(const container::geometry::dotbim::Material &source,
                  container::scene::SceneManager &sceneManager) {
  container::material::Material material = source.pbr;
  material.baseColor =
      glm::clamp(material.baseColor, glm::vec4(0.0f), glm::vec4(1.0f));
  material.opacityFactor = std::clamp(material.opacityFactor, 0.0f, 1.0f);
  material.metallicFactor = std::clamp(material.metallicFactor, 0.0f, 1.0f);
  material.roughnessFactor = std::clamp(material.roughnessFactor, 0.0f, 1.0f);
  material.alphaCutoff = std::clamp(material.alphaCutoff, 0.0f, 1.0f);
  material.normalTextureScale = std::isfinite(material.normalTextureScale)
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
    const container::geometry::dotbim::Model &model) {
  return !model.vertices.empty() && !model.indices.empty() &&
         !model.meshRanges.empty() && !model.elements.empty();
}

std::string modelLoadErrorPrefix(std::string_view format,
                                 const std::filesystem::path &path) {
  return std::string(format) + " file has no supported BIM geometry: " +
         container::util::pathToUtf8(path);
}

} // namespace

BimManager::BimManager(container::gpu::AllocationManager &allocationManager)
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
  vertices_.clear();
  indices_.clear();
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
         objectBuffer_.buffer != VK_NULL_HANDLE && !objectData_.empty() &&
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
  return static_cast<VkDeviceSize>(sizeof(container::gpu::ObjectData) *
                                   objectBufferCapacity_);
}

BimPickHit BimManager::pickRenderableObject(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY) const {
  return pickRenderableObjectForDraws(cameraData, viewportExtent, cursorX,
                                      cursorY, true, true);
}

BimPickHit BimManager::pickTransparentRenderableObject(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY) const {
  return pickRenderableObjectForDraws(cameraData, viewportExtent, cursorX,
                                      cursorY, false, true);
}

BimPickHit BimManager::pickRenderableObjectForDraws(
    const container::gpu::CameraData& cameraData,
    VkExtent2D viewportExtent,
    double cursorX,
    double cursorY,
    bool includeOpaque,
    bool includeTransparent) const {
  if (objectData_.empty() || vertices_.empty() || indices_.empty()) {
    return {};
  }

  const PickRay ray = makePickRay(cameraData, viewportExtent, cursorX, cursorY);
  if (!ray.valid) {
    return {};
  }

  BimPickHit nearest{};
  auto testDrawCommands = [&](const std::vector<DrawCommand>& commands,
                              TriangleCullMode cullMode) {
    for (const DrawCommand& command : commands) {
      if (command.firstIndex >= indices_.size() || command.indexCount < 3u) {
        continue;
      }
      const size_t firstIndex = command.firstIndex;
      const size_t indexCount =
          std::min(static_cast<size_t>(command.indexCount),
                   indices_.size() - firstIndex);
      const size_t endIndex = firstIndex + indexCount;
      const uint32_t instanceCount = std::max(command.instanceCount, 1u);

      for (uint32_t instanceOffset = 0u; instanceOffset < instanceCount;
           ++instanceOffset) {
        if (command.objectIndex >
            std::numeric_limits<uint32_t>::max() - instanceOffset) {
          break;
        }
        const uint32_t objectIndex = command.objectIndex + instanceOffset;
        if (objectIndex >= objectData_.size()) {
          continue;
        }

        const glm::vec4 sphere = objectData_[objectIndex].boundingSphere;
        float sphereDistance = 0.0f;
        if (sphere.w > 0.0f &&
            (!intersectRaySphere(ray.origin, ray.direction, glm::vec3(sphere),
                                 sphere.w, sphereDistance) ||
             sphereDistance > nearest.distance)) {
          continue;
        }

        const glm::mat4& model = objectData_[objectIndex].model;
        for (size_t index = firstIndex; index + 2u < endIndex; index += 3u) {
          const uint32_t i0 = indices_[index];
          const uint32_t i1 = indices_[index + 1u];
          const uint32_t i2 = indices_[index + 2u];
          if (i0 >= vertices_.size() || i1 >= vertices_.size() ||
              i2 >= vertices_.size()) {
            continue;
          }

          const glm::vec3 v0 =
              glm::vec3(model * glm::vec4(vertices_[i0].position, 1.0f));
          const glm::vec3 v1 =
              glm::vec3(model * glm::vec4(vertices_[i1].position, 1.0f));
          const glm::vec3 v2 =
              glm::vec3(model * glm::vec4(vertices_[i2].position, 1.0f));
          float hitDistance = 0.0f;
          if (intersectRayTriangle(ray.origin, ray.direction, v0, v1, v2,
                                   cullMode, hitDistance) &&
              hitDistance < nearest.distance) {
            const glm::vec3 hitPosition =
                ray.origin + ray.direction * hitDistance;
            nearest.objectIndex = objectIndex;
            nearest.distance = hitDistance;
            nearest.depth = projectDepth(cameraData, hitPosition);
            nearest.hit = true;
          }
        }
      }
    }
  };

  const bool hasSplitDrawCommands =
      !opaqueSingleSidedDrawCommands_.empty() ||
      !transparentSingleSidedDrawCommands_.empty() ||
      !opaqueWindingFlippedDrawCommands_.empty() ||
      !transparentWindingFlippedDrawCommands_.empty() ||
      !opaqueDoubleSidedDrawCommands_.empty() ||
      !transparentDoubleSidedDrawCommands_.empty();
  if (hasSplitDrawCommands) {
    if (includeOpaque) {
      testDrawCommands(opaqueSingleSidedDrawCommands_, TriangleCullMode::Back);
    }
    if (includeTransparent) {
      testDrawCommands(transparentSingleSidedDrawCommands_,
                       TriangleCullMode::Back);
    }
    if (includeOpaque) {
      testDrawCommands(opaqueWindingFlippedDrawCommands_,
                       TriangleCullMode::Front);
    }
    if (includeTransparent) {
      testDrawCommands(transparentWindingFlippedDrawCommands_,
                       TriangleCullMode::Front);
    }
    if (includeOpaque) {
      testDrawCommands(opaqueDoubleSidedDrawCommands_, TriangleCullMode::None);
    }
    if (includeTransparent) {
      testDrawCommands(transparentDoubleSidedDrawCommands_,
                       TriangleCullMode::None);
    }
  } else {
    if (includeOpaque) {
      testDrawCommands(opaqueDrawCommands_, TriangleCullMode::None);
    }
    if (includeTransparent) {
      testDrawCommands(transparentDrawCommands_, TriangleCullMode::None);
    }
  }

  return nearest;
}

void BimManager::collectDrawCommandsForObject(
    uint32_t objectIndex, std::vector<DrawCommand>& outCommands) const {
  outCommands.clear();
  if (objectIndex == std::numeric_limits<uint32_t>::max()) {
    return;
  }

  auto appendMatching = [&](const std::vector<DrawCommand>& commands) {
    for (const DrawCommand& command : commands) {
      const uint32_t instanceCount = std::max(command.instanceCount, 1u);
      if (objectIndex < command.objectIndex ||
          objectIndex - command.objectIndex >= instanceCount) {
        continue;
      }

      DrawCommand selected = command;
      selected.objectIndex = objectIndex;
      selected.instanceCount = 1u;
      outCommands.push_back(selected);
    }
  };

  appendMatching(opaqueDrawCommands_);
  appendMatching(transparentDrawCommands_);
}

std::filesystem::path
BimManager::resolveModelPath(const std::string &path) const {
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

void BimManager::loadModel(const std::string &path, float importScale,
                           container::scene::SceneManager &sceneManager) {
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

void BimManager::loadDotBim(const std::filesystem::path &path,
                            float importScale,
                            container::scene::SceneManager &sceneManager) {
  const auto model =
      container::geometry::dotbim::LoadFromFile(path, importScale);
  loadPreparedModel(model, path, "dotbim", sceneManager);
}

void BimManager::loadIfc(const std::filesystem::path &path, float importScale,
                         container::scene::SceneManager &sceneManager) {
  const auto model = container::geometry::ifc::LoadFromFile(path, importScale);
  loadPreparedModel(model, path, "IFC", sceneManager);
}

void BimManager::loadPreparedModel(
    const container::geometry::dotbim::Model &model,
    const std::filesystem::path &path, std::string_view format,
    container::scene::SceneManager &sceneManager) {
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
    const container::geometry::dotbim::Model &model,
    container::scene::SceneManager &sceneManager) {
  std::unordered_map<uint32_t, uint32_t> colorMaterialCache;
  colorMaterialCache.reserve(model.elements.size());
  std::vector<uint32_t> sourceMaterialCache(model.materials.size(),
                                            kInvalidMaterialIndex);
  std::unordered_map<uint32_t, MeshRange> rangesByMeshId;
  rangesByMeshId.reserve(model.meshRanges.size());
  for (const auto &sourceRange : model.meshRanges) {
    rangesByMeshId.emplace(sourceRange.meshId,
                           MeshRange{sourceRange.meshId, sourceRange.firstIndex,
                                     sourceRange.indexCount,
                                     sourceRange.boundsCenter,
                                     sourceRange.boundsRadius});
  }

  std::vector<PendingDraw> opaquePendingDraws;
  std::vector<PendingDraw> transparentPendingDraws;
  opaquePendingDraws.reserve(model.elements.size());
  transparentPendingDraws.reserve(model.elements.size());

  for (const auto &element : model.elements) {
    const auto rangeIt = rangesByMeshId.find(element.meshId);
    if (rangeIt == rangesByMeshId.end() || rangeIt->second.indexCount == 0u) {
      continue;
    }

    glm::vec4 color =
        glm::clamp(element.color, glm::vec4(0.0f), glm::vec4(1.0f));
    uint32_t materialIndex = kInvalidMaterialIndex;
    bool transparent = isTransparentColor(color);
    bool materialDoubleSided = false;
    if (isValidMaterialIndex(element.materialIndex, model)) {
      materialIndex = sourceMaterialCache[element.materialIndex];
      if (materialIndex == kInvalidMaterialIndex) {
        const auto &sourceMaterial = model.materials[element.materialIndex];
        materialIndex = sceneManager.createMaterial(
            makeSceneMaterial(sourceMaterial, sceneManager));
        sourceMaterialCache[element.materialIndex] = materialIndex;
      }
      const auto properties =
          sceneManager.materialRenderProperties(materialIndex);
      transparent = transparent || properties.transparent;
      materialDoubleSided = properties.doubleSided;
    } else {
      const auto alphaMode = transparent
                                 ? container::material::AlphaMode::Blend
                                 : container::material::AlphaMode::Opaque;
      const uint32_t colorKey = packColor(color);
      auto materialIt = colorMaterialCache.find(colorKey);
      if (materialIt == colorMaterialCache.end()) {
        const uint32_t newMaterialIndex =
            sceneManager.createSolidMaterial(color, false, alphaMode);
        materialIt =
            colorMaterialCache.emplace(colorKey, newMaterialIndex).first;
      }
      materialIndex = materialIt->second;
    }

    const auto &range = rangeIt->second;
    const bool doubleSided = element.doubleSided || materialDoubleSided;
    const bool windingFlipped =
        !doubleSided && transformFlipsWinding(element.transform);
    PendingDraw pending{};
    pending.object = makeObjectData(element.transform, materialIndex,
                                    doubleSided, range.boundsCenter,
                                    range.boundsRadius);
    pending.firstIndex = range.firstIndex;
    pending.indexCount = range.indexCount;
    if (transparent) {
      if (doubleSided) {
        pending.bucket = DrawBucket::TransparentDoubleSided;
      } else if (windingFlipped) {
        pending.bucket = DrawBucket::TransparentWindingFlipped;
      } else {
        pending.bucket = DrawBucket::TransparentSingleSided;
      }
      transparentPendingDraws.push_back(pending);
    } else {
      if (doubleSided) {
        pending.bucket = DrawBucket::OpaqueDoubleSided;
      } else if (windingFlipped) {
        pending.bucket = DrawBucket::OpaqueWindingFlipped;
      } else {
        pending.bucket = DrawBucket::OpaqueSingleSided;
      }
      opaquePendingDraws.push_back(pending);
    }
  }

  if (opaquePendingDraws.empty() && transparentPendingDraws.empty()) {
    return;
  }

  std::ranges::sort(opaquePendingDraws, [](const PendingDraw &lhs,
                                           const PendingDraw &rhs) {
    if (lhs.bucket != rhs.bucket) {
      return bucketSortKey(lhs.bucket) < bucketSortKey(rhs.bucket);
    }
    if (lhs.firstIndex != rhs.firstIndex) {
      return lhs.firstIndex < rhs.firstIndex;
    }
    return lhs.indexCount < rhs.indexCount;
  });

  const size_t totalDraws =
      opaquePendingDraws.size() + transparentPendingDraws.size();
  objectData_.reserve(totalDraws);
  opaqueDrawCommands_.reserve(opaquePendingDraws.size());
  opaqueSingleSidedDrawCommands_.reserve(opaquePendingDraws.size());
  opaqueWindingFlippedDrawCommands_.reserve(opaquePendingDraws.size());
  opaqueDoubleSidedDrawCommands_.reserve(opaquePendingDraws.size());
  transparentDrawCommands_.reserve(transparentPendingDraws.size());
  transparentSingleSidedDrawCommands_.reserve(transparentPendingDraws.size());
  transparentWindingFlippedDrawCommands_.reserve(transparentPendingDraws.size());
  transparentDoubleSidedDrawCommands_.reserve(transparentPendingDraws.size());

  auto appendPendingDraw = [this](const PendingDraw &pending,
                                  bool allowMerge) {
    const uint32_t objectIndex = static_cast<uint32_t>(objectData_.size());
    objectData_.push_back(pending.object);

    auto appendBucketCommand = [&](std::vector<DrawCommand> &commands) {
      appendDrawCommand(commands, objectIndex, pending.firstIndex,
                        pending.indexCount, allowMerge);
    };
    auto appendAggregateCommand = [&](std::vector<DrawCommand> &commands) {
      appendDrawCommand(commands, objectIndex, pending.firstIndex,
                        pending.indexCount, false);
    };

    switch (pending.bucket) {
      case DrawBucket::OpaqueSingleSided:
        appendAggregateCommand(opaqueDrawCommands_);
        appendBucketCommand(opaqueSingleSidedDrawCommands_);
        break;
      case DrawBucket::OpaqueWindingFlipped:
        appendAggregateCommand(opaqueDrawCommands_);
        appendBucketCommand(opaqueWindingFlippedDrawCommands_);
        break;
      case DrawBucket::OpaqueDoubleSided:
        appendAggregateCommand(opaqueDrawCommands_);
        appendBucketCommand(opaqueDoubleSidedDrawCommands_);
        break;
      case DrawBucket::TransparentSingleSided:
        appendAggregateCommand(transparentDrawCommands_);
        appendBucketCommand(transparentSingleSidedDrawCommands_);
        break;
      case DrawBucket::TransparentWindingFlipped:
        appendAggregateCommand(transparentDrawCommands_);
        appendBucketCommand(transparentWindingFlippedDrawCommands_);
        break;
      case DrawBucket::TransparentDoubleSided:
        appendAggregateCommand(transparentDrawCommands_);
        appendBucketCommand(transparentDoubleSidedDrawCommands_);
        break;
    }
  };

  for (const PendingDraw &pending : opaquePendingDraws) {
    appendPendingDraw(pending, true);
  }
  for (const PendingDraw &pending : transparentPendingDraws) {
    appendPendingDraw(pending, false);
  }

  if (objectData_.empty()) {
    return;
  }
  sceneManager.uploadMaterialResources();
  uploadObjects();
}

void BimManager::loadGltfFallback(
    const std::filesystem::path &path, float importScale,
    container::scene::SceneManager &sceneManager) {
  const auto model = container::geometry::Model::LoadFromGltf(
      container::util::pathToUtf8(path));
  if (model.vertices().empty() || model.indices().empty() ||
      model.primitiveRanges().empty()) {
    throw std::runtime_error(modelLoadErrorPrefix("glTF", path));
  }
  uploadGeometry(model.vertices(), model.indices());

  const glm::mat4 transform = importScaleTransform(importScale);
  const uint32_t materialIndex = sceneManager.defaultMaterialIndex();
  objectData_.push_back(
      makeObjectData(transform, materialIndex, false, glm::vec3(0.0f), 0.0f));

  opaqueDrawCommands_.reserve(model.primitiveRanges().size());
  opaqueSingleSidedDrawCommands_.reserve(model.primitiveRanges().size());
  opaqueDoubleSidedDrawCommands_.reserve(model.primitiveRanges().size());
  for (const auto &primitive : model.primitiveRanges()) {
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
  vertices_.clear();
  indices_.clear();
  if (vertices.empty() || indices.empty()) {
    return;
  }

  vertices_.assign(vertices.begin(), vertices.end());
  indices_.assign(indices.begin(), indices.end());

  const VkDeviceSize vertexBufferSize = static_cast<VkDeviceSize>(
      sizeof(container::geometry::Vertex) * vertices.size());
  const VkDeviceSize indexBufferSize =
      static_cast<VkDeviceSize>(sizeof(uint32_t) * indices.size());

  vertexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte *>(vertices.data()),
       static_cast<size_t>(vertexBufferSize)},
      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  indexBuffer_ = allocationManager_.uploadBuffer(
      {reinterpret_cast<const std::byte *>(indices.data()),
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

  SceneController::writeToBuffer(
      allocationManager_, objectBuffer_, objectData_.data(),
      sizeof(container::gpu::ObjectData) * objectData_.size());
  ++objectDataRevision_;
}

} // namespace container::renderer
