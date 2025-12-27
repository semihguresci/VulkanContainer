#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"

namespace utility::material {
struct TextureResource;
struct Material;
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

}  // namespace utility::material