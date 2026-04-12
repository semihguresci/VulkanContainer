#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "tiny_gltf.h"

#include "Container/app/AppConfig.h"
#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonMath.h"
#include "Container/geometry/Model.h"
#include "Container/utility/MaterialManager.h"
#include "Container/utility/MaterialXIntegration.h"
#include "Container/utility/TextureManager.h"

namespace container::gpu {
class AllocationManager;
class PipelineManager;
class VulkanDevice;
struct AllocatedBuffer;
}  // namespace container::gpu

namespace container::scene {
class SceneGraph;
}  // namespace container::scene

namespace container::scene {

struct ModelBounds {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  glm::vec3 center{0.0f};
  glm::vec3 size{0.0f};
  float radius{0.0f};
  bool valid{false};
};

class SceneManager {
 public:
  SceneManager(container::gpu::AllocationManager& allocationManager,
               container::gpu::PipelineManager& pipelineManager,
               std::shared_ptr<container::gpu::VulkanDevice> deviceWrapper,
               const container::app::AppConfig& config);
  ~SceneManager();

  SceneManager(const SceneManager&) = delete;
  SceneManager& operator=(const SceneManager&) = delete;

  void initialize(const std::string& initialModelPath);

  bool reloadModel(const std::string& path,
                   const container::gpu::AllocatedBuffer& cameraBuffer,
                   const container::gpu::AllocatedBuffer& objectBuffer);

  void updateDescriptorSet(
      const container::gpu::AllocatedBuffer& cameraBuffer,
      const container::gpu::AllocatedBuffer& objectBuffer);

  VkDescriptorSetLayout descriptorSetLayout() const {
    return descriptorSetLayout_;
  }
  VkDescriptorSet descriptorSet() const { return descriptorSet_; }

  const std::vector<container::geometry::Vertex>& vertices() const { return vertices_; }
  const std::vector<uint32_t>& indices() const { return indices_; }
  const std::vector<container::geometry::PrimitiveRange>& primitiveRanges() const {
    return model_.primitiveRanges();
  }
  const container::geometry::Model& model() const { return model_; }
  VkIndexType indexType() const { return indexType_; }
  uint32_t defaultMaterialIndex() const { return defaultMaterialIndex_; }
  const ModelBounds& modelBounds() const { return modelBounds_; }
  void populateSceneGraph(SceneGraph& sceneGraph) const;

  glm::vec4 resolveMaterialColor(uint32_t materialIndex) const;
  glm::vec4 resolveMaterialEmissive(uint32_t materialIndex) const;
  glm::vec2 resolveMaterialMetallicRoughnessFactors(
      uint32_t materialIndex) const;
  uint32_t resolveMaterialTextureIndex(uint32_t materialIndex) const;
  uint32_t resolveMaterialNormalTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialOcclusionTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialEmissiveTexture(uint32_t materialIndex) const;
  uint32_t resolveMaterialMetallicRoughnessTexture(
      uint32_t materialIndex) const;
  float resolveMaterialAlphaCutoff(uint32_t materialIndex) const;
  bool isMaterialTransparent(uint32_t materialIndex) const;
  bool isMaterialAlphaMasked(uint32_t materialIndex) const;
  bool isMaterialDoubleSided(uint32_t materialIndex) const;

 private:
  uint32_t queryTextureDescriptorCapacity() const;
  uint32_t resolveLoadedMaterialIndex(int32_t materialIndex) const;
  void createDescriptorSetLayout();
  void createSampler();
  void loadMaterialXMaterial();
  void loadGltfAssets();
  void updateModelBounds();
  void allocateDescriptorSet();
  void writeDescriptorSetContents(
      const container::gpu::AllocatedBuffer& cameraBuffer,
      const container::gpu::AllocatedBuffer& objectBuffer);
  void resetLoadedAssets();

  container::gpu::AllocationManager* allocationManager_{nullptr};
  container::gpu::PipelineManager* pipelineManager_{nullptr};
  std::shared_ptr<container::gpu::VulkanDevice> deviceWrapper_{};
  container::app::AppConfig config_{};

  container::material::SlangMaterialXBridge materialXBridge_{};
  container::material::TextureManager textureManager_{};
  container::material::MaterialManager materialManager_{};

  container::geometry::Model model_{};
  tinygltf::Model gltfModel_{};
  std::vector<container::geometry::Vertex> vertices_{};
  std::vector<uint32_t> indices_{};
  ModelBounds modelBounds_{};

  VkIndexType indexType_{VK_INDEX_TYPE_UINT32};

  glm::vec4 materialBaseColor_{1.0f};
  uint32_t defaultMaterialIndex_{std::numeric_limits<uint32_t>::max()};
  uint32_t gltfMaterialBaseIndex_{0};

  VkSampler baseColorSampler_{VK_NULL_HANDLE};
  VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
  VkDescriptorPool descriptorPool_{VK_NULL_HANDLE};
  VkDescriptorSet descriptorSet_{VK_NULL_HANDLE};
  uint32_t textureDescriptorCapacity_{1};
};

}  // namespace container::scene
