#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Container/utility/TextureResource.h"

namespace utility::material {

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
