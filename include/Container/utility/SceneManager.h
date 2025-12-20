#pragma once

#include <Container/app/AppConfig.h>
#include <Container/geometry/Model.h>
#include <Container/utility/AllocationManager.h>
#include <Container/utility/MaterialManager.h>
#include <Container/utility/MaterialXIntegration.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SceneData.h>
#include <Container/utility/VulkanDevice.h>

#include <glm/glm.hpp>

#include <limits>
#include <string>
#include <vector>

namespace utility::scene {

class SceneManager {
 public:
  SceneManager(utility::memory::AllocationManager& allocationManager,
               utility::pipeline::PipelineManager& pipelineManager,
               std::shared_ptr<utility::vulkan::VulkanDevice> deviceWrapper,
               const app::AppConfig& config);
  ~SceneManager();

  SceneManager(const SceneManager&) = delete;
  SceneManager& operator=(const SceneManager&) = delete;

  void initialize(const std::string& initialModelPath);
  bool reloadModel(const std::string& path,
                   const utility::memory::AllocatedBuffer& cameraBuffer,
                   const utility::memory::AllocatedBuffer& objectBuffer);

  void updateDescriptorSet(const utility::memory::AllocatedBuffer& cameraBuffer,
                           const utility::memory::AllocatedBuffer& objectBuffer);

  VkDescriptorSetLayout descriptorSetLayout() const { return descriptorSetLayout_; }
  VkDescriptorSet descriptorSet() const { return descriptorSet_; }

  const std::vector<geometry::Vertex>& vertices() const { return vertices_; }
  const std::vector<uint32_t>& indices() const { return indices_; }
  VkIndexType indexType() const { return indexType_; }
  uint32_t defaultMaterialIndex() const { return defaultMaterialIndex_; }

  glm::vec4 resolveMaterialColor(uint32_t materialIndex) const;
  glm::vec4 resolveMaterialEmissive(uint32_t materialIndex) const;
  glm::vec2 resolveMaterialMetallicRoughnessFactors(uint32_t materialIndex) const;
  uint32_t resolveMaterialTextureIndex(uint32_t materialIndex) const;
  uint32_t resolveMaterialNormalTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialOcclusionTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialEmissiveTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialMetallicRoughnessTexture(uint32_t materialIndex) const;

 private:
  void createDescriptorSetLayout();
  void createSampler();
  void loadMaterialXMaterial();
  void loadGltfAssets();
  void allocateDescriptorSet();
  void writeDescriptorSetContents(const utility::memory::AllocatedBuffer& cameraBuffer,
                                  const utility::memory::AllocatedBuffer& objectBuffer);
  void resetLoadedAssets();

  utility::memory::AllocationManager* allocationManager_{nullptr};
  utility::pipeline::PipelineManager* pipelineManager_{nullptr};
  std::shared_ptr<utility::vulkan::VulkanDevice> deviceWrapper_{};
  app::AppConfig config_{};

  utility::materialx::SlangMaterialXBridge materialXBridge_{};
  utility::material::TextureManager textureManager_{};
  utility::material::MaterialManager materialManager_{};

  geometry::Model model_{};
  tinygltf::Model gltfModel_{};
  std::vector<geometry::Vertex> vertices_{};
  std::vector<uint32_t> indices_{};
  VkIndexType indexType_{VK_INDEX_TYPE_UINT32};

  glm::vec4 materialBaseColor_{1.0f};
  uint32_t defaultMaterialIndex_{std::numeric_limits<uint32_t>::max()};

  VkSampler baseColorSampler_{VK_NULL_HANDLE};
  VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
};

}  // namespace utility::scene
