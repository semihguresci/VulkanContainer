#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <limits>
#include <print>
#include <stdexcept>
#include <utility>

#include "Container/geometry/GltfModelLoader.h"
#include "Container/geometry/Mesh.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/VulkanDevice.h"

#include <glm/gtc/quaternion.hpp>

namespace container::scene {

namespace {

glm::mat4 nodeLocalTransform(const tinygltf::Node& node) {
  if (node.matrix.size() == 16) {
    glm::mat4 transform(1.0f);
    for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
        transform[column][row] =
            static_cast<float>(node.matrix[column * 4 + row]);
      }
    }
    return transform;
  }

  glm::mat4 transform(1.0f);

  if (node.translation.size() == 3) {
    transform = glm::translate(
        transform,
        glm::vec3(static_cast<float>(node.translation[0]),
                  static_cast<float>(node.translation[1]),
                  static_cast<float>(node.translation[2])));
  }

  if (node.rotation.size() == 4) {
    const glm::quat rotation(
        static_cast<float>(node.rotation[3]),
        static_cast<float>(node.rotation[0]),
        static_cast<float>(node.rotation[1]),
        static_cast<float>(node.rotation[2]));
    transform *= glm::mat4_cast(rotation);
  }

  if (node.scale.size() == 3) {
    transform = glm::scale(
        transform,
        glm::vec3(static_cast<float>(node.scale[0]),
                  static_cast<float>(node.scale[1]),
                  static_cast<float>(node.scale[2])));
  }

  return transform;
}

bool isDefaultSceneRequest(std::string_view path) {
  return path == container::app::kDefaultSceneModelToken;
}

bool isProceduralSphereEntry(std::string_view entry) {
  return entry == "__procedural_uv_sphere__";
}

glm::mat4 makeDefaultSceneTransform(const glm::vec3& translation, float uniformScale) {
  glm::mat4 transform = glm::translate(glm::mat4(1.0f), translation);
  return glm::scale(transform, glm::vec3(uniformScale));
}

glm::mat4 composeNodeWorldTransform(const tinygltf::Model& model, int nodeIndex) {
  glm::mat4 worldTransform(1.0f);
  if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
    return worldTransform;
  }

  std::vector<int> parentByNode(model.nodes.size(), -1);
  for (size_t parentIndex = 0; parentIndex < model.nodes.size(); ++parentIndex) {
    for (int childIndex : model.nodes[parentIndex].children) {
      if (childIndex >= 0 && childIndex < static_cast<int>(model.nodes.size())) {
        parentByNode[childIndex] = static_cast<int>(parentIndex);
      }
    }
  }

  std::vector<int> chain;
  for (int current = nodeIndex; current >= 0; current = parentByNode[current]) {
    chain.push_back(current);
  }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    worldTransform *= nodeLocalTransform(model.nodes[*it]);
  }
  return worldTransform;
}

container::geometry::Mesh transformMesh(const container::geometry::Mesh& mesh,
                                        const glm::mat4& transform,
                                        int32_t materialIndex) {
  std::vector<container::geometry::Vertex> vertices = mesh.vertices();
  std::vector<uint32_t> indices = mesh.indices();
  const glm::mat3 modelMatrix = glm::mat3(transform);
  const glm::mat3 normalMatrix = glm::transpose(glm::inverse(modelMatrix));
  const bool windingFlipped = glm::determinant(modelMatrix) < 0.0f;

  for (auto& vertex : vertices) {
    vertex.position = glm::vec3(transform * glm::vec4(vertex.position, 1.0f));

    glm::vec3 worldNormal = normalMatrix * vertex.normal;
    if (glm::dot(worldNormal, worldNormal) > 1e-8f) {
      vertex.normal = glm::normalize(worldNormal);
    }

    glm::vec3 worldTangent = modelMatrix * glm::vec3(vertex.tangent);
    worldTangent -= vertex.normal * glm::dot(vertex.normal, worldTangent);
    if (glm::dot(worldTangent, worldTangent) > 1e-8f) {
      vertex.tangent =
          glm::vec4(glm::normalize(worldTangent),
                    windingFlipped ? -vertex.tangent.w : vertex.tangent.w);
    }
  }

  if (windingFlipped) {
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
      std::swap(indices[i + 1], indices[i + 2]);
    }
  }

  return container::geometry::Mesh(std::move(vertices), std::move(indices),
                                   materialIndex, mesh.disableBackfaceCulling());
}

void appendModelMeshes(const container::geometry::Model& model,
                       const glm::mat4& transform,
                       int32_t materialIndex,
                       std::vector<container::geometry::Mesh>& mergedMeshes) {
  for (const auto& mesh : model.meshes()) {
    const int32_t resolvedMaterialIndex =
        materialIndex >= 0 ? materialIndex : mesh.materialIndex();
    mergedMeshes.push_back(transformMesh(mesh, transform, resolvedMaterialIndex));
  }
}

}  // namespace

SceneManager::SceneManager(
    container::gpu::AllocationManager& allocationManager,
    container::gpu::PipelineManager& pipelineManager,
    std::shared_ptr<container::gpu::VulkanDevice> deviceWrapper,
    const container::app::AppConfig& config)
    : allocationManager_(&allocationManager),
      pipelineManager_(&pipelineManager),
      deviceWrapper_(std::move(deviceWrapper)),
      config_(config) {}

SceneManager::~SceneManager() {
  resetLoadedAssets();

  descriptorSets_.clear();
  descriptorPool_ = VK_NULL_HANDLE;
  descriptorSetLayout_ = VK_NULL_HANDLE;

  VkDevice device = deviceWrapper_->device();
  if (baseColorSampler_ != VK_NULL_HANDLE) {
    vkDestroySampler(device, baseColorSampler_, nullptr);
    baseColorSampler_ = VK_NULL_HANDLE;
  }
}

uint32_t SceneManager::queryTextureDescriptorCapacity() const {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(), &properties);

  return std::max(
      1u, std::min(properties.limits.maxPerStageDescriptorSampledImages,
                   properties.limits.maxDescriptorSetSampledImages));
}

uint32_t SceneManager::resolveLoadedMaterialIndex(int32_t materialIndex) const {
  if (materialIndex < 0 ||
      materialIndex >= static_cast<int32_t>(gltfModel_.materials.size())) {
    return defaultMaterialIndex_;
  }

  return gltfMaterialBaseIndex_ + static_cast<uint32_t>(materialIndex);
}

void SceneManager::initialize(const std::string& initialModelPath,
                              uint32_t descriptorSetCount) {
  textureDescriptorCapacity_ = queryTextureDescriptorCapacity();
  createDescriptorSetLayout();
  createSampler();
  loadMaterialXMaterial();

  config_.modelPath = initialModelPath;
  loadGltfAssets();
  allocateDescriptorSets(descriptorSetCount);
}

bool SceneManager::isDefaultTestSceneActive() const {
  return isDefaultSceneRequest(config_.modelPath);
}

void SceneManager::populateSceneGraph(SceneGraph& sceneGraph) const {
  sceneGraph = SceneGraph{};

  if (primitiveRanges().empty()) {
    return;
  }

  if (gltfModel_.nodes.empty()) {
    const uint32_t rootNode = sceneGraph.createNode(
        glm::mat4(1.0f), defaultMaterialIndex_, false);
    for (size_t primitiveIndex = 0; primitiveIndex < primitiveRanges().size();
         ++primitiveIndex) {
      const auto& primitive = primitiveRanges()[primitiveIndex];
      const uint32_t primitiveNode = sceneGraph.createNode(
          glm::mat4(1.0f), resolveLoadedMaterialIndex(primitive.materialIndex),
          true, static_cast<uint32_t>(primitiveIndex));
      sceneGraph.setParent(primitiveNode, rootNode);
    }
    sceneGraph.updateWorldTransforms();
    return;
  }

  std::vector<uint32_t> meshPrimitiveBase(gltfModel_.meshes.size(), 0);
  uint32_t primitiveOffset = 0;
  for (size_t meshIndex = 0; meshIndex < gltfModel_.meshes.size(); ++meshIndex) {
    meshPrimitiveBase[meshIndex] = primitiveOffset;
    primitiveOffset +=
        static_cast<uint32_t>(gltfModel_.meshes[meshIndex].primitives.size());
  }

  std::function<void(int, std::optional<uint32_t>)> appendNode =
      [&](int gltfNodeIndex, std::optional<uint32_t> parentNode) {
        if (gltfNodeIndex < 0 ||
            gltfNodeIndex >= static_cast<int>(gltfModel_.nodes.size())) {
          return;
        }

        const auto& gltfNode = gltfModel_.nodes[gltfNodeIndex];
        const uint32_t graphNode = sceneGraph.createNode(
            nodeLocalTransform(gltfNode), defaultMaterialIndex_, false);
        sceneGraph.setParent(graphNode, parentNode);

        if (gltfNode.mesh >= 0 &&
            gltfNode.mesh < static_cast<int>(gltfModel_.meshes.size())) {
          const auto& mesh = gltfModel_.meshes[gltfNode.mesh];
          const uint32_t basePrimitiveIndex = meshPrimitiveBase[gltfNode.mesh];

          for (size_t primitiveOffsetInMesh = 0;
               primitiveOffsetInMesh < mesh.primitives.size();
               ++primitiveOffsetInMesh) {
            const uint32_t primitiveIndex =
                basePrimitiveIndex + static_cast<uint32_t>(primitiveOffsetInMesh);
            if (primitiveIndex >= primitiveRanges().size()) {
              continue;
            }

            const auto& primitive = primitiveRanges()[primitiveIndex];
            const uint32_t primitiveNode = sceneGraph.createNode(
                glm::mat4(1.0f),
                resolveLoadedMaterialIndex(primitive.materialIndex), true,
                primitiveIndex);
            sceneGraph.setParent(primitiveNode, graphNode);
          }
        }

        for (int childIndex : gltfNode.children) {
          appendNode(childIndex, graphNode);
        }
      };

  if (!gltfModel_.scenes.empty()) {
    int sceneIndex = gltfModel_.defaultScene;
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(gltfModel_.scenes.size())) {
      sceneIndex = 0;
    }

    for (int rootNodeIndex : gltfModel_.scenes[sceneIndex].nodes) {
      appendNode(rootNodeIndex, std::nullopt);
    }
  } else {
    std::vector<bool> hasParent(gltfModel_.nodes.size(), false);
    for (const auto& node : gltfModel_.nodes) {
      for (int childIndex : node.children) {
        if (childIndex >= 0 &&
            childIndex < static_cast<int>(hasParent.size())) {
          hasParent[childIndex] = true;
        }
      }
    }

    for (size_t nodeIndex = 0; nodeIndex < gltfModel_.nodes.size(); ++nodeIndex) {
      if (!hasParent[nodeIndex]) {
        appendNode(static_cast<int>(nodeIndex), std::nullopt);
      }
    }
  }

  sceneGraph.updateWorldTransforms();
}

bool SceneManager::reloadModel(
    const std::string& path,
    std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
    const container::gpu::AllocatedBuffer& objectBuffer) {
  const std::string previousModelPath = config_.modelPath;
  auto resetForLoad = [this]() {
    resetLoadedAssets();
    loadMaterialXMaterial();
    vertices_.clear();
    indices_.clear();
    gltfModel_ = tinygltf::Model{};
    model_ = container::geometry::Model{};
  };

  resetForLoad();
  config_.modelPath = path;

  try {
    loadGltfAssets();
    updateDescriptorSets(cameraBuffers, objectBuffer);
    return true;
  } catch (...) {
    try {
      resetForLoad();
      config_.modelPath = previousModelPath;
      loadGltfAssets();
      updateDescriptorSets(cameraBuffers, objectBuffer);
    } catch (const std::exception& e) {
      std::println(stderr, "failed to restore previous model '{}': {}",
                   previousModelPath, e.what());
      resetForLoad();
    }
    return false;
  }
}

void SceneManager::updateDescriptorSets(
    std::span<const container::gpu::AllocatedBuffer> cameraBuffers,
    const container::gpu::AllocatedBuffer& objectBuffer) {
  if (cameraBuffers.empty() || descriptorSets_.empty()) return;

  if (descriptorSets_.size() != cameraBuffers.size()) {
    allocateDescriptorSets(static_cast<uint32_t>(cameraBuffers.size()));
  }

  const size_t descriptorCount = std::min(descriptorSets_.size(), cameraBuffers.size());
  for (size_t i = 0; i < descriptorCount; ++i) {
    writeDescriptorSetContents(descriptorSets_[i], cameraBuffers[i], objectBuffer);
  }
}

/* ---------- Material resolution ---------- */

glm::vec4 SceneManager::resolveMaterialColor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->baseColor;
  }
  return materialBaseColor_;
}

glm::vec4 SceneManager::resolveMaterialEmissive(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return glm::vec4(m->emissiveColor, 1.0f);
  }
  return glm::vec4(0.0f);
}

glm::vec2 SceneManager::resolveMaterialMetallicRoughnessFactors(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return {m->metallicFactor, m->roughnessFactor};
  }
  return {1.0f, 1.0f};
}

uint32_t SceneManager::resolveMaterialTextureIndex(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->baseColorTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialNormalTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->normalTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

float SceneManager::resolveMaterialNormalTextureScale(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->normalTextureScale;
  }
  return 1.0f;
}

uint32_t SceneManager::resolveMaterialOcclusionTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->occlusionTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

float SceneManager::resolveMaterialOcclusionStrength(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->occlusionStrength;
  }
  return 1.0f;
}

uint32_t SceneManager::resolveMaterialEmissiveTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->emissiveTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialMetallicRoughnessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->metallicRoughnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialRoughnessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->roughnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialMetalnessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->metalnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialSpecularTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->specularTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialSpecularColorTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->specularColorTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialHeightTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->heightTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialOpacityTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->opacityTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialTransmissionTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->transmissionTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialClearcoatTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialClearcoatRoughnessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatRoughnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialClearcoatNormalTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatNormalTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialThicknessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->thicknessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialSheenColorTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->sheenColorTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialSheenRoughnessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->sheenRoughnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialIridescenceTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialIridescenceThicknessTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceThicknessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

float SceneManager::resolveMaterialOpacityFactor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->opacityFactor;
  }
  return 1.0f;
}

float SceneManager::resolveMaterialSpecularFactor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->specularFactor;
  }
  return 1.0f;
}

glm::vec4 SceneManager::resolveMaterialSpecularColorFactor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return glm::vec4(m->specularColorFactor, 0.0f);
  }
  return glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
}

float SceneManager::resolveMaterialHeightScale(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->heightScale;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialHeightOffset(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->heightOffset;
  }
  return -0.5f;
}

float SceneManager::resolveMaterialTransmissionFactor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->transmissionFactor;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialEmissiveStrength(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->emissiveStrength;
  }
  return 1.0f;
}

float SceneManager::resolveMaterialIor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->ior;
  }
  return 1.5f;
}

float SceneManager::resolveMaterialDispersion(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->dispersion;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialClearcoatFactor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatFactor;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialClearcoatRoughnessFactor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatRoughnessFactor;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialClearcoatNormalTextureScale(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->clearcoatNormalTextureScale;
  }
  return 1.0f;
}

float SceneManager::resolveMaterialThicknessFactor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->thicknessFactor;
  }
  return 0.0f;
}

glm::vec4 SceneManager::resolveMaterialAttenuationColor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return glm::vec4(m->attenuationColor, 0.0f);
  }
  return glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
}

float SceneManager::resolveMaterialAttenuationDistance(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->attenuationDistance;
  }
  return std::numeric_limits<float>::infinity();
}

glm::vec4 SceneManager::resolveMaterialSheenColorFactor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return glm::vec4(m->sheenColorFactor, 0.0f);
  }
  return glm::vec4(0.0f);
}

float SceneManager::resolveMaterialSheenRoughnessFactor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->sheenRoughnessFactor;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialIridescenceFactor(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceFactor;
  }
  return 0.0f;
}

float SceneManager::resolveMaterialIridescenceIor(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceIor;
  }
  return 1.3f;
}

float SceneManager::resolveMaterialIridescenceThicknessMinimum(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceThicknessMinimum;
  }
  return 100.0f;
}

float SceneManager::resolveMaterialIridescenceThicknessMaximum(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->iridescenceThicknessMaximum;
  }
  return 400.0f;
}

float SceneManager::resolveMaterialAlphaCutoff(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->alphaCutoff;
  }
  return 0.5f;
}

bool SceneManager::isMaterialTransparent(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->alphaMode == container::material::AlphaMode::Blend;
  }
  return false;
}

bool SceneManager::isMaterialAlphaMasked(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->alphaMode == container::material::AlphaMode::Mask;
  }
  return false;
}

bool SceneManager::isMaterialDoubleSided(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->doubleSided;
  }
  return false;
}

bool SceneManager::usesMaterialSpecularGlossiness(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->specularGlossinessWorkflow;
  }
  return false;
}

bool SceneManager::isMaterialUnlit(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->unlit;
  }
  return false;
}

/* ---------- Vulkan setup ---------- */

void SceneManager::createDescriptorSetLayout() {
  std::array<VkDescriptorSetLayoutBinding, 4> bindings{};

  bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                     VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureDescriptorCapacity_,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  std::array<VkDescriptorBindingFlags, 4> bindingFlags{
      0, 0, 0,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};

  descriptorSetLayout_ = pipelineManager_->createDescriptorSetLayout(
      std::vector<VkDescriptorSetLayoutBinding>(bindings.begin(),
                                                bindings.end()),
      std::vector<VkDescriptorBindingFlags>(bindingFlags.begin(),
                                           bindingFlags.end()),
      0);
}

void SceneManager::createSampler() {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(), &properties);

  VkSamplerCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = VK_FILTER_LINEAR;
  info.minFilter = VK_FILTER_LINEAR;
  info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  info.anisotropyEnable = VK_TRUE;
  info.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
  info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

  if (vkCreateSampler(deviceWrapper_->device(), &info, nullptr,
                      &baseColorSampler_) != VK_SUCCESS) {
    throw std::runtime_error("failed to create scene texture sampler");
  }
}

/* ---------- Assets ---------- */

void SceneManager::loadMaterialXMaterial() {
  container::material::Material material{};

  try {
    auto doc = materialXBridge_.loadDocument("materials/base.mtlx");
    material.baseColor = materialXBridge_.extractBaseColor(doc);
    material.opacityFactor =
        materialXBridge_.extractFloatInput(doc, "opacity", material.opacityFactor);
    material.specularFactor =
        materialXBridge_.extractFloatInput(doc, "specular", material.specularFactor);
    material.heightScale =
        materialXBridge_.extractFloatInput(doc, "height", material.heightScale);
    material.transmissionFactor = materialXBridge_.extractFloatInput(
        doc, "refraction",
        materialXBridge_.extractFloatInput(
            doc, "transmission", material.transmissionFactor));
  } catch (const std::exception& e) {
    std::println(stderr, "MaterialX load failed: {}", e.what());
    material.baseColor = glm::vec4(1.0f);
  }

  material.emissiveColor = glm::vec3(0.0f);
  material.metallicFactor = 1.0f;
  material.roughnessFactor = 1.0f;

  materialBaseColor_ = material.baseColor;

  if (defaultMaterialIndex_ == std::numeric_limits<uint32_t>::max()) {
    defaultMaterialIndex_ = materialManager_.createMaterial(material);
  } else {
    materialManager_.updateMaterial(defaultMaterialIndex_, material);
  }
}

void SceneManager::loadGltfAssets() {
  model_ = container::geometry::Model{};
  gltfModel_ = tinygltf::Model{};

   if (isDefaultSceneRequest(config_.modelPath)) {
    loadDefaultTestSceneAssets();
    vertices_ = model_.vertices();
    indices_ = model_.indices();
    updateModelBounds();
    indexType_ = VK_INDEX_TYPE_UINT32;
    return;
  }

  if (!config_.modelPath.empty()) {
    try {
      std::filesystem::path resolvedPath(config_.modelPath);
      if (resolvedPath.is_relative() && !std::filesystem::exists(resolvedPath)) {
        resolvedPath = resolveSceneAssetPath(config_.modelPath);
      }
      auto result = container::geometry::gltf::LoadModelWithSource(resolvedPath.string());
      gltfModel_ = std::move(result.gltfModel);
      model_ = std::move(result.model);

      const auto baseDir = resolvedPath.parent_path();

      auto imageToTexture = materialXBridge_.loadTexturesForGltf(
          gltfModel_, baseDir, textureManager_,
          [this](const std::string& path, bool isSrgb) {
            return allocationManager_->createTextureFromFile(
                path, isSrgb ? VK_FORMAT_R8G8B8A8_SRGB
                             : VK_FORMAT_R8G8B8A8_UNORM);
          });

      const uint32_t fallbackMaterialIndex = defaultMaterialIndex_;
      gltfMaterialBaseIndex_ =
          static_cast<uint32_t>(materialManager_.materialCount());
      materialXBridge_.loadMaterialsForGltf(
          gltfModel_, imageToTexture, materialManager_, defaultMaterialIndex_);
      defaultMaterialIndex_ = fallbackMaterialIndex;
    } catch (const std::exception& e) {
      std::println(stderr, "glTF load failed: {}", e.what());
      throw;
    }
  }

  vertices_ = model_.vertices();
  indices_ = model_.indices();
  updateModelBounds();
  indexType_ = VK_INDEX_TYPE_UINT32;
}

void SceneManager::loadDefaultTestSceneAssets() {
  std::vector<container::geometry::Mesh> mergedMeshes;
  mergedMeshes.reserve(container::app::kDefaultSceneModelRelativePaths.size());

  const std::array<glm::mat4, 3> transforms = {{
      makeDefaultSceneTransform(glm::vec3(-2.4f, 0.0f, 0.0f), 1.3f),
      makeDefaultSceneTransform(glm::vec3(0.0f, 0.0f, 0.0f), 1.0f),
      makeDefaultSceneTransform(glm::vec3(2.4f, 0.0f, 0.0f), 1.25f),
  }};

  container::material::Material sphereMaterial{};
  sphereMaterial.baseColor = glm::vec4(0.90f, 0.92f, 1.0f, 1.0f);
  sphereMaterial.metallicFactor = 0.0f;
  sphereMaterial.roughnessFactor = 0.45f;
  const int32_t proceduralSphereMaterialIndex = static_cast<int32_t>(
      materialManager_.createMaterial(sphereMaterial));

  for (size_t i = 0; i < container::app::kDefaultSceneModelRelativePaths.size(); ++i) {
    const auto entry = container::app::kDefaultSceneModelRelativePaths[i];
    if (isProceduralSphereEntry(entry)) {
      appendModelMeshes(container::geometry::Model::MakeSphere(), transforms[i],
                        proceduralSphereMaterialIndex, mergedMeshes);
      continue;
    }

    const std::filesystem::path assetPath =
        resolveSceneAssetPath(entry);
    appendSceneAsset(assetPath, transforms[i], mergedMeshes);
  }

  if (mergedMeshes.empty()) {
    throw std::runtime_error("failed to build default test scene from triangle/cube/sphere assets");
  }

  model_ = container::geometry::Model::FromMeshes(std::move(mergedMeshes));
  gltfModel_ = tinygltf::Model{};
}

void SceneManager::appendSceneAsset(
    const std::filesystem::path& assetPath,
    const glm::mat4& transform,
    std::vector<container::geometry::Mesh>& mergedMeshes) {
  auto result = container::geometry::gltf::LoadModelWithSource(assetPath.string());

  const auto imageToTexture = materialXBridge_.loadTexturesForGltf(
      result.gltfModel, assetPath.parent_path(), textureManager_,
      [this](const std::string& path, bool isSrgb) {
        return allocationManager_->createTextureFromFile(
            path, isSrgb ? VK_FORMAT_R8G8B8A8_SRGB
                         : VK_FORMAT_R8G8B8A8_UNORM);
      });

  const uint32_t fallbackMaterialIndex = defaultMaterialIndex_;
  const uint32_t materialBaseIndex =
      static_cast<uint32_t>(materialManager_.materialCount());
  materialXBridge_.loadMaterialsForGltf(
      result.gltfModel, imageToTexture, materialManager_, defaultMaterialIndex_);
  defaultMaterialIndex_ = fallbackMaterialIndex;

  std::vector<uint32_t> primitiveBaseByMesh(result.gltfModel.meshes.size(), 0);
  uint32_t primitiveBase = 0;
  for (size_t meshIndex = 0; meshIndex < result.gltfModel.meshes.size(); ++meshIndex) {
    primitiveBaseByMesh[meshIndex] = primitiveBase;
    primitiveBase +=
        static_cast<uint32_t>(result.gltfModel.meshes[meshIndex].primitives.size());
  }

  uint32_t meshCursor = 0;
  for (size_t nodeIndex = 0; nodeIndex < result.gltfModel.nodes.size(); ++nodeIndex) {
    const auto& node = result.gltfModel.nodes[nodeIndex];
    if (node.mesh < 0 ||
        node.mesh >= static_cast<int>(result.gltfModel.meshes.size())) {
      continue;
    }

    const glm::mat4 nodeTransform =
        transform * composeNodeWorldTransform(result.gltfModel, static_cast<int>(nodeIndex));
    const auto& meshDef = result.gltfModel.meshes[node.mesh];
    for (size_t primitiveIndex = 0; primitiveIndex < meshDef.primitives.size(); ++primitiveIndex) {
      if (meshCursor >= result.model.meshes().size()) {
        break;
      }
      const auto& mesh = result.model.meshes()[meshCursor++];
      const uint32_t primitiveGlobalIndex =
          primitiveBaseByMesh[node.mesh] + static_cast<uint32_t>(primitiveIndex);
      int32_t sourceMaterialIndex = mesh.materialIndex();
      if (primitiveGlobalIndex < result.model.primitiveRanges().size()) {
        sourceMaterialIndex = result.model.primitiveRanges()[primitiveGlobalIndex].materialIndex;
      }
      const int32_t materialIndex = sourceMaterialIndex >= 0
          ? static_cast<int32_t>(materialBaseIndex) + sourceMaterialIndex
          : -1;
      mergedMeshes.push_back(transformMesh(mesh, nodeTransform, materialIndex));
    }
  }

  while (meshCursor < result.model.meshes().size()) {
    const auto& mesh = result.model.meshes()[meshCursor++];
    const int32_t materialIndex = mesh.materialIndex() >= 0
        ? static_cast<int32_t>(materialBaseIndex) + mesh.materialIndex()
        : -1;
    mergedMeshes.push_back(transformMesh(mesh, transform, materialIndex));
  }
}

std::filesystem::path SceneManager::resolveSceneAssetPath(
    std::string_view relativePath) const {
  const std::filesystem::path exeRelative =
      container::util::executableDirectory() / std::filesystem::path(relativePath);
  if (std::filesystem::exists(exeRelative)) {
    return exeRelative;
  }

  const std::filesystem::path workspaceRelative = std::filesystem::path(relativePath);
  if (std::filesystem::exists(workspaceRelative)) {
    return workspaceRelative;
  }

  return exeRelative;
}

void SceneManager::updateModelBounds() {
  modelBounds_ = ModelBounds{};
  if (vertices_.empty()) {
    return;
  }

  auto forEachRenderedPoint =
      [this](const std::function<void(const glm::vec3&)>& visit) {
        auto emitPrimitive = [&](uint32_t primitiveIndex,
                                 const glm::mat4& transform) {
          if (primitiveIndex >= model_.primitiveRanges().size()) {
            return;
          }

          const auto& primitive = model_.primitiveRanges()[primitiveIndex];
          const uint32_t endIndex = primitive.firstIndex + primitive.indexCount;
          for (uint32_t i = primitive.firstIndex;
               i < endIndex && i < indices_.size(); ++i) {
            const uint32_t vertexIndex = indices_[i];
            if (vertexIndex >= vertices_.size()) {
              continue;
            }
            visit(glm::vec3(transform *
                            glm::vec4(vertices_[vertexIndex].position, 1.0f)));
          }
        };

        if (!gltfModel_.nodes.empty() && !gltfModel_.meshes.empty()) {
          std::vector<uint32_t> meshPrimitiveBase(gltfModel_.meshes.size(), 0);
          uint32_t primitiveOffset = 0;
          for (size_t meshIndex = 0; meshIndex < gltfModel_.meshes.size();
               ++meshIndex) {
            meshPrimitiveBase[meshIndex] = primitiveOffset;
            primitiveOffset += static_cast<uint32_t>(
                gltfModel_.meshes[meshIndex].primitives.size());
          }

          std::function<void(int, const glm::mat4&)> traverseNode =
              [&](int gltfNodeIndex, const glm::mat4& parentTransform) {
                if (gltfNodeIndex < 0 ||
                    gltfNodeIndex >= static_cast<int>(gltfModel_.nodes.size())) {
                  return;
                }

                const auto& node = gltfModel_.nodes[gltfNodeIndex];
                const glm::mat4 worldTransform =
                    parentTransform * nodeLocalTransform(node);

                if (node.mesh >= 0 &&
                    node.mesh < static_cast<int>(gltfModel_.meshes.size())) {
                  const auto& mesh = gltfModel_.meshes[node.mesh];
                  const uint32_t basePrimitiveIndex =
                      meshPrimitiveBase[node.mesh];
                  for (size_t primitiveInMesh = 0;
                       primitiveInMesh < mesh.primitives.size();
                       ++primitiveInMesh) {
                    emitPrimitive(
                        basePrimitiveIndex +
                            static_cast<uint32_t>(primitiveInMesh),
                        worldTransform);
                  }
                }

                for (int childIndex : node.children) {
                  traverseNode(childIndex, worldTransform);
                }
              };

          if (!gltfModel_.scenes.empty()) {
            int sceneIndex = gltfModel_.defaultScene;
            if (sceneIndex < 0 ||
                sceneIndex >= static_cast<int>(gltfModel_.scenes.size())) {
              sceneIndex = 0;
            }

            for (int rootNodeIndex : gltfModel_.scenes[sceneIndex].nodes) {
              traverseNode(rootNodeIndex, glm::mat4(1.0f));
            }
            return;
          }

          std::vector<bool> hasParent(gltfModel_.nodes.size(), false);
          for (const auto& node : gltfModel_.nodes) {
            for (int childIndex : node.children) {
              if (childIndex >= 0 &&
                  childIndex < static_cast<int>(hasParent.size())) {
                hasParent[childIndex] = true;
              }
            }
          }

          for (size_t nodeIndex = 0; nodeIndex < gltfModel_.nodes.size();
               ++nodeIndex) {
            if (!hasParent[nodeIndex]) {
              traverseNode(static_cast<int>(nodeIndex), glm::mat4(1.0f));
            }
          }
          return;
        }

        for (const auto& vertex : vertices_) {
          visit(vertex.position);
        }
      };

  bool hasPoint = false;
  glm::vec3 minBounds(std::numeric_limits<float>::max());
  glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
  forEachRenderedPoint([&](const glm::vec3& point) {
    hasPoint = true;
    minBounds = glm::min(minBounds, point);
    maxBounds = glm::max(maxBounds, point);
  });

  if (!hasPoint) {
    return;
  }

  const glm::vec3 center = 0.5f * (minBounds + maxBounds);
  float radius = 0.0f;
  forEachRenderedPoint([&](const glm::vec3& point) {
    radius = std::max(radius, glm::length(point - center));
  });

  modelBounds_.min = minBounds;
  modelBounds_.max = maxBounds;
  modelBounds_.center = center;
  modelBounds_.size = maxBounds - minBounds;
  modelBounds_.radius = radius;
  modelBounds_.valid = true;
}

/* ---------- Descriptor sets ---------- */

void SceneManager::allocateDescriptorSets(uint32_t descriptorSetCount) {
  const uint32_t textureDescriptorCount =
      std::max<uint32_t>(1u, static_cast<uint32_t>(textureManager_.textureCount()));
  const uint32_t setCount = std::max(1u, descriptorSetCount);

  if (descriptorPool_ != VK_NULL_HANDLE) {
    pipelineManager_->destroyDescriptorPool(descriptorPool_);
  }
  descriptorSets_.clear();

  if (textureDescriptorCount > textureDescriptorCapacity_) {
    throw std::runtime_error("Model requires more sampled-image descriptors than "
                             "the device supports");
  }

  std::vector<VkDescriptorPoolSize> poolSizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setCount},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setCount},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
       textureDescriptorCapacity_ * setCount},
      {VK_DESCRIPTOR_TYPE_SAMPLER, setCount},
  };

  descriptorPool_ = pipelineManager_->createDescriptorPool(poolSizes, setCount, 0);

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool_;
  allocInfo.descriptorSetCount = setCount;

  std::vector<VkDescriptorSetLayout> layouts(setCount, descriptorSetLayout_);
  allocInfo.pSetLayouts = layouts.data();

  VkDescriptorSetVariableDescriptorCountAllocateInfo countInfo{};
  countInfo.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
  countInfo.descriptorSetCount = setCount;
  std::vector<uint32_t> descriptorCounts(setCount, textureDescriptorCapacity_);
  countInfo.pDescriptorCounts = descriptorCounts.data();
  allocInfo.pNext = &countInfo;

  descriptorSets_.assign(setCount, VK_NULL_HANDLE);
  if (vkAllocateDescriptorSets(deviceWrapper_->device(), &allocInfo,
                               descriptorSets_.data()) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate scene descriptor sets");
  }
}

void SceneManager::writeDescriptorSetContents(
    VkDescriptorSet descriptorSet,
    const container::gpu::AllocatedBuffer& cameraBuffer,
    const container::gpu::AllocatedBuffer& objectBuffer) {
  if (descriptorSet == VK_NULL_HANDLE) return;

  VkDescriptorBufferInfo cameraInfo{cameraBuffer.buffer, 0, sizeof(container::gpu::CameraData)};
  VkDescriptorBufferInfo objectInfo{
      objectBuffer.buffer, 0, objectBuffer.allocation_info.size};

  std::vector<VkWriteDescriptorSet> writes;

  VkWriteDescriptorSet cameraWrite{};
  cameraWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  cameraWrite.dstSet = descriptorSet;
  cameraWrite.dstBinding = 0;
  cameraWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  cameraWrite.descriptorCount = 1;
  cameraWrite.pBufferInfo = &cameraInfo;
  writes.push_back(cameraWrite);

  VkWriteDescriptorSet objectWrite{};
  objectWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  objectWrite.dstSet = descriptorSet;
  objectWrite.dstBinding = 1;
  objectWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  objectWrite.descriptorCount = 1;
  objectWrite.pBufferInfo = &objectInfo;
  writes.push_back(objectWrite);

  std::vector<VkDescriptorImageInfo> imageInfos;
  const size_t texCount = textureManager_.textureCount();
  if (texCount > textureDescriptorCapacity_) {
    throw std::runtime_error("Model requires more sampled-image descriptors than "
                             "the device supports");
  }

  for (size_t i = 0; i < texCount; ++i) {
    const auto* tex = textureManager_.getTexture(static_cast<uint32_t>(i));
    if (!tex) continue;
    imageInfos.push_back({VK_NULL_HANDLE, tex->imageView,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
  }

  if (!imageInfos.empty()) {
    VkWriteDescriptorSet imageWrite{};
    imageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    imageWrite.dstSet = descriptorSet;
    imageWrite.dstBinding = 3;
    imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    imageWrite.descriptorCount = static_cast<uint32_t>(imageInfos.size());
    imageWrite.pImageInfo = imageInfos.data();
    writes.push_back(imageWrite);
  }

  VkDescriptorImageInfo samplerInfo{baseColorSampler_, VK_NULL_HANDLE,
                                    VK_IMAGE_LAYOUT_UNDEFINED};

  VkWriteDescriptorSet samplerWrite{};
  samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  samplerWrite.dstSet = descriptorSet;
  samplerWrite.dstBinding = 2;
  samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  samplerWrite.descriptorCount = 1;
  samplerWrite.pImageInfo = &samplerInfo;
  writes.push_back(samplerWrite);

  vkUpdateDescriptorSets(deviceWrapper_->device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
}

void SceneManager::resetLoadedAssets() {
  allocationManager_->resetTextureAllocations();
  materialManager_ = container::material::MaterialManager{};
  textureManager_ = container::material::TextureManager{};
  defaultMaterialIndex_ = std::numeric_limits<uint32_t>::max();
  gltfMaterialBaseIndex_ = 0;
  materialBaseColor_ = glm::vec4(1.0f);
  modelBounds_ = ModelBounds{};
}

}  // namespace container::scene
