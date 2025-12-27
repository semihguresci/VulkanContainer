#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "Container/utility/SceneData.h"
#include "Container/utility/SceneManager.h"

namespace utility::scene {

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

void SceneManager::initialize(const std::string& initialModelPath) {
  createDescriptorSetLayout();
  createSampler();
  loadMaterialXMaterial();

  config_.modelPath = initialModelPath;
  loadGltfAssets();
  allocateDescriptorSet();
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

  bindings[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 static_cast<uint32_t>(config_.maxSceneObjects),
                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

  std::array<VkDescriptorBindingFlags, 4> bindingFlags{
      0, 0, 0,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};

  descriptorSetLayout_ =
      pipelineManager_->createDescriptorSetLayout(bindings, bindingFlags, 0);
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

      materialXBridge_.loadMaterialsForGltf(
          gltfModel_, imageToTexture, materialManager_, defaultMaterialIndex_);
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
  indexType_ = VK_INDEX_TYPE_UINT32;
}

/* ---------- Descriptor sets ---------- */

void SceneManager::allocateDescriptorSet() {
  const uint32_t textureDescriptorCount = std::min<uint32_t>(
      config_.maxSceneObjects,
      std::max<uint32_t>(
          1u, static_cast<uint32_t>(textureManager_.textureCount())));

  std::vector<VkDescriptorPoolSize> poolSizes = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, config_.maxSceneObjects + 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
       std::max<uint32_t>(
           config_.maxSceneObjects,
           static_cast<uint32_t>(textureManager_.textureCount()))},
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
      objectBuffer.buffer, 0, sizeof(ObjectData) * config_.maxSceneObjects};

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
  const size_t maxTextures =
      std::min<size_t>(texCount, config_.maxSceneObjects);

  for (size_t i = 0; i < maxTextures; ++i) {
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
  materialBaseColor_ = glm::vec4(1.0f);
}

}  // namespace utility::scene
