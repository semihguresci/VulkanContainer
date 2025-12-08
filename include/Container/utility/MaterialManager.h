#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace utility::material {

struct TextureResource {
  std::string name;
  VkImage image{VK_NULL_HANDLE};
  VkImageView imageView{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
};

struct Material {
  glm::vec4 baseColor{1.0f};
  glm::vec3 emissiveColor{0.0f};
  float metallicFactor{1.0f};
  float roughnessFactor{1.0f};
  uint32_t baseColorTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t normalTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t occlusionTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t emissiveTextureIndex{std::numeric_limits<uint32_t>::max()};
  uint32_t metallicRoughnessTextureIndex{std::numeric_limits<uint32_t>::max()};
};

class TextureManager {
 public:
  uint32_t registerTexture(const TextureResource& resource);
  [[nodiscard]] std::optional<uint32_t> findTextureIndex(
      const std::string& name) const;
  [[nodiscard]] const TextureResource* getTexture(uint32_t index) const;
  [[nodiscard]] size_t textureCount() const { return textures_.size(); }

 private:
  std::vector<TextureResource> textures_{};
  std::unordered_map<std::string, uint32_t> textureNameToIndex_{};
};

class MaterialManager {
 public:
  uint32_t createMaterial(const Material& material);
  bool updateMaterial(uint32_t index, const Material& material);
  [[nodiscard]] const Material* getMaterial(uint32_t index) const;
  [[nodiscard]] size_t materialCount() const { return materials_.size(); }

 private:
  std::vector<Material> materials_{};
};

}  // namespace utility::material

