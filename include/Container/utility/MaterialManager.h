#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <string>
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
  uint32_t baseColorTextureIndex{std::numeric_limits<uint32_t>::max()};
};

class TextureManager {
 public:
  uint32_t registerTexture(const TextureResource& resource);
  [[nodiscard]] const TextureResource* getTexture(uint32_t index) const;
  [[nodiscard]] size_t textureCount() const { return textures_.size(); }

 private:
  std::vector<TextureResource> textures_{};
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

