#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "Container/common/CommonMath.h"
#include "Container/geometry/GltfModelLoader.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/SceneManager.h"

#include <glm/gtc/quaternion.hpp>

namespace utility::scene {

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
    return common::math::toLeftHandedTransform(transform);
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

  return common::math::toLeftHandedTransform(transform);
}

}  // namespace

SceneManager::SceneManager(
    utility::memory::AllocationManager& allocationManager,
    utility::pipeline::PipelineManager& pipelineManager,
    std::shared_ptr<utility::vulkan::VulkanDevice> deviceWrapper,
    const app::AppConfig& config)
    : allocationManager_(&allocationManager),
      pipelineManager_(&pipelineManager),
      deviceWrapper_(std::move(deviceWrapper)),
      config_(config) {}

SceneManager::~SceneManager() {
  resetLoadedAssets();

  VkDevice device = deviceWrapper_->device();

  if (descriptorPool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
  }
  if (descriptorSetLayout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
    descriptorSetLayout_ = VK_NULL_HANDLE;
  }
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

void SceneManager::initialize(const std::string& initialModelPath) {
  textureDescriptorCapacity_ = queryTextureDescriptorCapacity();
  createDescriptorSetLayout();
  createSampler();
  loadMaterialXMaterial();

  config_.modelPath = initialModelPath;
  loadGltfAssets();
  allocateDescriptorSet();
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
    const utility::memory::AllocatedBuffer& cameraBuffer,
    const utility::memory::AllocatedBuffer& objectBuffer) {
  resetLoadedAssets();
  loadMaterialXMaterial();

  config_.modelPath = path;
  vertices_.clear();
  indices_.clear();
  gltfModel_ = tinygltf::Model{};
  model_ = geometry::Model{};

  try {
    loadGltfAssets();
    writeDescriptorSetContents(cameraBuffer, objectBuffer);
    return true;
  } catch (...) {
    materialManager_ = utility::material::MaterialManager{};
    textureManager_ = utility::material::TextureManager{};
    materialBaseColor_ = glm::vec4(1.0f);
    defaultMaterialIndex_ = std::numeric_limits<uint32_t>::max();
    loadMaterialXMaterial();
    return false;
  }
}

void SceneManager::updateDescriptorSet(
    const utility::memory::AllocatedBuffer& cameraBuffer,
    const utility::memory::AllocatedBuffer& objectBuffer) {
  writeDescriptorSetContents(cameraBuffer, objectBuffer);
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

uint32_t SceneManager::resolveMaterialOcclusionTexture(
    uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->occlusionTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
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

float SceneManager::resolveMaterialAlphaCutoff(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->alphaCutoff;
  }
  return 0.5f;
}

bool SceneManager::isMaterialTransparent(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->alphaMode == utility::material::AlphaMode::Blend;
  }
  return false;
}

bool SceneManager::isMaterialAlphaMasked(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->alphaMode == utility::material::AlphaMode::Mask;
  }
  return false;
}

bool SceneManager::isMaterialDoubleSided(uint32_t materialIndex) const {
  if (const auto* m = materialManager_.getMaterial(materialIndex)) {
    return m->doubleSided;
  }
  return false;
}

/* ---------- Vulkan setup ---------- */

void SceneManager::createDescriptorSetLayout() {
  std::array<VkDescriptorSetLayoutBinding, 4> bindings{};

  bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                 nullptr};

  bindings[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureDescriptorCapacity_,
                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

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

  vkCreateSampler(deviceWrapper_->device(), &info, nullptr, &baseColorSampler_);
}

/* ---------- Assets ---------- */

void SceneManager::loadMaterialXMaterial() {
  utility::material::Material material{};

  try {
    auto doc = materialXBridge_.loadDocument("materials/base.mtlx");
    material.baseColor = materialXBridge_.extractBaseColor(doc);
  } catch (const std::exception& e) {
    std::cerr << "MaterialX load failed: " << e.what() << std::endl;
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
  model_ = geometry::Model::MakeCube();
  gltfModel_ = tinygltf::Model{};

  if (!config_.modelPath.empty()) {
    try {
      auto result = geometry::gltf::LoadModelWithSource(config_.modelPath);
      gltfModel_ = std::move(result.gltfModel);
      model_ = std::move(result.model);

      const auto baseDir =
          std::filesystem::path(config_.modelPath).parent_path();

      auto imageToTexture = materialXBridge_.loadTexturesForGltf(
          gltfModel_, baseDir, textureManager_,
          [this](const std::string& path) {
            return allocationManager_->createTextureFromFile(path);
          });

      const uint32_t fallbackMaterialIndex = defaultMaterialIndex_;
      gltfMaterialBaseIndex_ =
          static_cast<uint32_t>(materialManager_.materialCount());
      materialXBridge_.loadMaterialsForGltf(
          gltfModel_, imageToTexture, materialManager_, defaultMaterialIndex_);
      defaultMaterialIndex_ = fallbackMaterialIndex;
    } catch (const std::exception& e) {
      std::cerr << "glTF load failed: " << e.what()
                << "; falling back to cube.\n";
    }
  }

  if (model_.empty()) {
    model_ = geometry::Model::MakeCube();
  }

  vertices_ = model_.vertices();
  indices_ = model_.indices();
  updateModelBounds();
  indexType_ = VK_INDEX_TYPE_UINT32;
}

void SceneManager::updateModelBounds() {
  modelBounds_ = ModelBounds{};
  if (vertices_.empty()) {
    return;
  }

  glm::vec3 minBounds = vertices_.front().position;
  glm::vec3 maxBounds = vertices_.front().position;

  for (const auto& vertex : vertices_) {
    minBounds = glm::min(minBounds, vertex.position);
    maxBounds = glm::max(maxBounds, vertex.position);
  }

  const glm::vec3 center = 0.5f * (minBounds + maxBounds);
  float radius = 0.0f;
  for (const auto& vertex : vertices_) {
    radius = std::max(radius, glm::length(vertex.position - center));
  }

  modelBounds_.min = minBounds;
  modelBounds_.max = maxBounds;
  modelBounds_.center = center;
  modelBounds_.size = maxBounds - minBounds;
  modelBounds_.radius = radius;
  modelBounds_.valid = true;
}

/* ---------- Descriptor sets ---------- */

void SceneManager::allocateDescriptorSet() {
  const uint32_t textureDescriptorCount =
      std::max<uint32_t>(1u, static_cast<uint32_t>(textureManager_.textureCount()));

  if (textureDescriptorCount > textureDescriptorCapacity_) {
    throw std::runtime_error("Model requires more sampled-image descriptors than "
                             "the device supports");
  }

  std::vector<VkDescriptorPoolSize> poolSizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textureDescriptorCount},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
  };

  descriptorPool_ = pipelineManager_->createDescriptorPool(poolSizes, 1, 0);

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool_;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &descriptorSetLayout_;

  VkDescriptorSetVariableDescriptorCountAllocateInfo countInfo{};
  countInfo.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
  countInfo.descriptorSetCount = 1;
  countInfo.pDescriptorCounts = &textureDescriptorCount;
  allocInfo.pNext = &countInfo;

  vkAllocateDescriptorSets(deviceWrapper_->device(), &allocInfo,
                           &descriptorSet_);
}

void SceneManager::writeDescriptorSetContents(
    const utility::memory::AllocatedBuffer& cameraBuffer,
    const utility::memory::AllocatedBuffer& objectBuffer) {
  if (descriptorSet_ == VK_NULL_HANDLE) return;

  VkDescriptorBufferInfo cameraInfo{cameraBuffer.buffer, 0, sizeof(CameraData)};
  VkDescriptorBufferInfo objectInfo{
      objectBuffer.buffer, 0, objectBuffer.allocation_info.size};

  std::vector<VkWriteDescriptorSet> writes;

  VkWriteDescriptorSet cameraWrite{};
  cameraWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  cameraWrite.dstSet = descriptorSet_;
  cameraWrite.dstBinding = 0;
  cameraWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  cameraWrite.descriptorCount = 1;
  cameraWrite.pBufferInfo = &cameraInfo;
  writes.push_back(cameraWrite);

  VkWriteDescriptorSet objectWrite{};
  objectWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  objectWrite.dstSet = descriptorSet_;
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
    imageWrite.dstSet = descriptorSet_;
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
  samplerWrite.dstSet = descriptorSet_;
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
  materialManager_ = utility::material::MaterialManager{};
  textureManager_ = utility::material::TextureManager{};
  defaultMaterialIndex_ = std::numeric_limits<uint32_t>::max();
  gltfMaterialBaseIndex_ = 0;
  materialBaseColor_ = glm::vec4(1.0f);
  modelBounds_ = ModelBounds{};
}

}  // namespace utility::scene
