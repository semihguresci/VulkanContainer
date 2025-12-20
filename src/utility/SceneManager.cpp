#include <Container/utility/SceneManager.h>
#include <Container/utility/SceneData.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>

namespace utility::scene {

SceneManager::SceneManager(utility::memory::AllocationManager& allocationManager,
                           utility::pipeline::PipelineManager& pipelineManager,
                           std::shared_ptr<utility::vulkan::VulkanDevice> deviceWrapper,
                           const app::AppConfig& config)
    : allocationManager_(&allocationManager),
      pipelineManager_(&pipelineManager),
      deviceWrapper_(std::move(deviceWrapper)),
      config_(config) {}

SceneManager::~SceneManager() {
  resetLoadedAssets();
  if (descriptorPool_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(deviceWrapper_->device(), descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
  }
  if (descriptorSetLayout_ != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(deviceWrapper_->device(), descriptorSetLayout_, nullptr);
    descriptorSetLayout_ = VK_NULL_HANDLE;
  }
  if (baseColorSampler_ != VK_NULL_HANDLE) {
    vkDestroySampler(deviceWrapper_->device(), baseColorSampler_, nullptr);
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

bool SceneManager::reloadModel(const std::string& path,
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
  } catch (const std::exception& exc) {
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

glm::vec4 SceneManager::resolveMaterialColor(uint32_t materialIndex) const {
  if (const auto* material = materialManager_.getMaterial(materialIndex)) {
    return material->baseColor;
  }
  return materialBaseColor_;
}

glm::vec4 SceneManager::resolveMaterialEmissive(uint32_t materialIndex) const {
  if (const auto* material = materialManager_.getMaterial(materialIndex)) {
    return glm::vec4(material->emissiveColor, 1.0f);
  }
  return glm::vec4(0.0f);
}

glm::vec2 SceneManager::resolveMaterialMetallicRoughnessFactors(
    uint32_t materialIndex) const {
  if (const auto* material = materialManager_.getMaterial(materialIndex)) {
    return glm::vec2(material->metallicFactor, material->roughnessFactor);
  }
  return glm::vec2(1.0f, 1.0f);
}

uint32_t SceneManager::resolveMaterialTextureIndex(uint32_t materialIndex) const {
  if (const auto* material = materialManager_.getMaterial(materialIndex)) {
    return material->baseColorTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialNormalTexture(uint32_t materialIndex) const {
  if (const auto* material = materialManager_.getMaterial(materialIndex)) {
    return material->normalTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialOcclusionTexture(uint32_t materialIndex) const {
  if (const auto* material = materialManager_.getMaterial(materialIndex)) {
    return material->occlusionTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialEmissiveTexture(uint32_t materialIndex) const {
  if (const auto* material = materialManager_.getMaterial(materialIndex)) {
    return material->emissiveTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

uint32_t SceneManager::resolveMaterialMetallicRoughnessTexture(
    uint32_t materialIndex) const {
  if (const auto* material = materialManager_.getMaterial(materialIndex)) {
    return material->metallicRoughnessTextureIndex;
  }
  return std::numeric_limits<uint32_t>::max();
}

void SceneManager::createDescriptorSetLayout() {
  vk::DescriptorSetLayoutBinding cameraBinding{
      .binding = 0,
      .descriptorType = vk::DescriptorType::eUniformBuffer,
      .descriptorCount = 1,
      .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
  };

  vk::DescriptorSetLayoutBinding objectBinding{
      .binding = 1,
      .descriptorType = vk::DescriptorType::eStorageBuffer,
      .descriptorCount = 1,
      .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
  };

  vk::DescriptorSetLayoutBinding samplerBinding{
      .binding = 2,
      .descriptorType = vk::DescriptorType::eSampler,
      .descriptorCount = 1,
      .stageFlags = vk::ShaderStageFlagBits::eFragment,
  };

  vk::DescriptorSetLayoutBinding textureBinding{
      .binding = 3,
      .descriptorType = vk::DescriptorType::eSampledImage,
      .descriptorCount = static_cast<uint32_t>(config_.maxSceneObjects),
      .stageFlags = vk::ShaderStageFlagBits::eFragment,
  };

  std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {
      cameraBinding, objectBinding, samplerBinding, textureBinding};

  std::array<vk::DescriptorBindingFlags, 4> bindingFlags = {
      0u,
      0u,
      0u,
      vk::DescriptorBindingFlagBits::ePartiallyBound |
          vk::DescriptorBindingFlagBits::eVariableDescriptorCount};

  std::vector<VkDescriptorSetLayoutBinding> rawBindings;
  rawBindings.reserve(bindings.size());
  std::transform(bindings.begin(), bindings.end(), std::back_inserter(rawBindings),
                 [](const vk::DescriptorSetLayoutBinding& binding) {
                   return static_cast<VkDescriptorSetLayoutBinding>(binding);
                 });

  std::vector<VkDescriptorBindingFlags> rawBindingFlags;
  rawBindingFlags.reserve(bindingFlags.size());
  std::transform(bindingFlags.begin(), bindingFlags.end(),
                 std::back_inserter(rawBindingFlags),
                 [](vk::DescriptorBindingFlags flags) {
                   return static_cast<VkDescriptorBindingFlags>(flags);
                 });

  descriptorSetLayout_ =
      pipelineManager_->createDescriptorSetLayout(rawBindings, rawBindingFlags, 0);
}

void SceneManager::createSampler() {
  VkPhysicalDeviceProperties properties{};
  vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(), &properties);

  vk::SamplerCreateInfo samplerInfo{
      .magFilter = vk::Filter::eLinear,
      .minFilter = vk::Filter::eLinear,
      .mipmapMode = vk::SamplerMipmapMode::eLinear,
      .addressModeU = vk::SamplerAddressMode::eRepeat,
      .addressModeV = vk::SamplerAddressMode::eRepeat,
      .addressModeW = vk::SamplerAddressMode::eRepeat,
      .mipLodBias = 0.0f,
      .anisotropyEnable = vk::True,
      .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
      .compareEnable = vk::False,
      .compareOp = vk::CompareOp::eAlways,
      .minLod = 0.0f,
      .maxLod = 0.0f,
      .borderColor = vk::BorderColor::eIntOpaqueBlack,
      .unnormalizedCoordinates = vk::False,
  };

  vk::Device device{deviceWrapper_->device()};
  if (device.createSampler(&samplerInfo, nullptr, &baseColorSampler_) != vk::Result::eSuccess) {
    throw std::runtime_error("failed to create texture sampler!");
  }
}

void SceneManager::loadMaterialXMaterial() {
  utility::material::Material material{};
  try {
    auto document = materialXBridge_.loadDocument("materials/base.mtlx");
    material.baseColor = materialXBridge_.extractBaseColor(document);
  } catch (const std::exception& exc) {
    std::cerr << "MaterialX load failed: " << exc.what() << std::endl;
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
      auto gltfResult = geometry::gltf::LoadModelWithSource(config_.modelPath);
      gltfModel_ = std::move(gltfResult.gltfModel);
      model_ = std::move(gltfResult.model);

      const auto baseDir = std::filesystem::path(config_.modelPath).parent_path();
      auto imageToTexture = materialXBridge_.loadTexturesForGltf(
          gltfModel_, baseDir, textureManager_,
          [this](const std::string& path) { return allocationManager_->createTextureFromFile(path); });
      materialXBridge_.loadMaterialsForGltf(gltfModel_, imageToTexture, materialManager_,
                                            defaultMaterialIndex_);
    } catch (const std::exception& exc) {
      std::cerr << "glTF load failed: " << exc.what()
                << "; falling back to cube model." << std::endl;
    }
  }

  if (model_.empty()) {
    model_ = geometry::Model::MakeCube();
  }

  vertices_ = model_.vertices();
  indices_ = model_.indices();
  indexType_ = VK_INDEX_TYPE_UINT32;
}

void SceneManager::allocateDescriptorSet() {
  const uint32_t textureDescriptorCount = std::min<uint32_t>(
      config_.maxSceneObjects,
      std::max<uint32_t>(1u, static_cast<uint32_t>(textureManager_.textureCount())));

  VkDescriptorPoolSize uniformPool{};
  uniformPool.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uniformPool.descriptorCount = config_.maxSceneObjects + 1;

  VkDescriptorPoolSize storagePool{};
  storagePool.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  storagePool.descriptorCount = 1;

  VkDescriptorPoolSize sampledImagePool{};
  sampledImagePool.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  sampledImagePool.descriptorCount =
      std::max<uint32_t>(config_.maxSceneObjects,
                         static_cast<uint32_t>(textureManager_.textureCount()));

  VkDescriptorPoolSize samplerPool{};
  samplerPool.type = VK_DESCRIPTOR_TYPE_SAMPLER;
  samplerPool.descriptorCount = 1;

  descriptorPool_ = pipelineManager_->createDescriptorPool(
      std::vector<VkDescriptorPoolSize>{uniformPool, storagePool, sampledImagePool,
                                        samplerPool},
      1, 0);

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool_;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &descriptorSetLayout_;
  uint32_t descriptorCount = textureDescriptorCount;
  VkDescriptorSetVariableDescriptorCountAllocateInfo countInfo{};
  countInfo.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
  countInfo.descriptorSetCount = 1;
  countInfo.pDescriptorCounts = &descriptorCount;
  allocInfo.pNext = &countInfo;

  if (vkAllocateDescriptorSets(deviceWrapper_->device(), &allocInfo, &descriptorSet_) !=
      VK_SUCCESS) {
    throw std::runtime_error("failed to allocate descriptor set!");
  }
}

void SceneManager::writeDescriptorSetContents(
    const utility::memory::AllocatedBuffer& cameraBuffer,
    const utility::memory::AllocatedBuffer& objectBuffer) {
  if (descriptorSet_ == VK_NULL_HANDLE) return;

  VkDescriptorBufferInfo cameraBufferInfo{};
  cameraBufferInfo.buffer = cameraBuffer.buffer;
  cameraBufferInfo.offset = 0;
  cameraBufferInfo.range = sizeof(CameraData);

  VkDescriptorBufferInfo objectBufferInfo{};
  objectBufferInfo.buffer = objectBuffer.buffer;
  objectBufferInfo.offset = 0;
  objectBufferInfo.range = sizeof(ObjectData) * config_.maxSceneObjects;

  std::vector<VkWriteDescriptorSet> descriptorWrites{};

  VkWriteDescriptorSet cameraWrite{};
  cameraWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  cameraWrite.dstSet = descriptorSet_;
  cameraWrite.dstBinding = 0;
  cameraWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  cameraWrite.descriptorCount = 1;
  cameraWrite.pBufferInfo = &cameraBufferInfo;
  descriptorWrites.push_back(cameraWrite);

  VkWriteDescriptorSet objectWrite{};
  objectWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  objectWrite.dstSet = descriptorSet_;
  objectWrite.dstBinding = 1;
  objectWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  objectWrite.descriptorCount = 1;
  objectWrite.pBufferInfo = &objectBufferInfo;
  descriptorWrites.push_back(objectWrite);

  const uint32_t textureDescriptorCount = std::min<uint32_t>(
      config_.maxSceneObjects,
      std::max<uint32_t>(1u, static_cast<uint32_t>(textureManager_.textureCount())));

  std::vector<VkDescriptorImageInfo> textureInfos{};
  const size_t textureCount = textureManager_.textureCount();
  if (textureCount > 0) {
    textureInfos.reserve(std::min<size_t>(textureCount, textureDescriptorCount));
    const size_t maxTextures = std::min<size_t>(textureDescriptorCount, textureCount);
    for (size_t i = 0; i < maxTextures; ++i) {
      const auto* tex = textureManager_.getTexture(static_cast<uint32_t>(i));
      if (tex == nullptr) continue;
      VkDescriptorImageInfo info{};
      info.sampler = VK_NULL_HANDLE;
      info.imageView = tex->imageView;
      info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      textureInfos.push_back(info);
    }

    if (!textureInfos.empty()) {
      VkWriteDescriptorSet textureWrite{};
      textureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      textureWrite.dstSet = descriptorSet_;
      textureWrite.dstBinding = 3;
      textureWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      textureWrite.descriptorCount =
          static_cast<uint32_t>(textureInfos.size());
      textureWrite.pImageInfo = textureInfos.data();
      descriptorWrites.push_back(textureWrite);
    }
  }

  VkDescriptorImageInfo samplerInfo{};
  samplerInfo.sampler = baseColorSampler_;

  VkWriteDescriptorSet samplerWrite{};
  samplerWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  samplerWrite.dstSet = descriptorSet_;
  samplerWrite.dstBinding = 2;
  samplerWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  samplerWrite.descriptorCount = 1;
  samplerWrite.pImageInfo = &samplerInfo;
  descriptorWrites.push_back(samplerWrite);

  vkUpdateDescriptorSets(deviceWrapper_->device(),
                         static_cast<uint32_t>(descriptorWrites.size()),
                         descriptorWrites.data(), 0, nullptr);
}

void SceneManager::resetLoadedAssets() {
  allocationManager_->resetTextureAllocations();
  materialManager_ = utility::material::MaterialManager{};
  textureManager_ = utility::material::TextureManager{};
  defaultMaterialIndex_ = std::numeric_limits<uint32_t>::max();
  materialBaseColor_ = glm::vec4(1.0f);
}

}  // namespace utility::scene
