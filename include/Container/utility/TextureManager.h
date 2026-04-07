#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Container/utility/TextureResource.h"

namespace utility::material {

/// Transparent hash for heterogeneous lookup on std::unordered_map<std::string, ...>
struct StringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
};

class TextureManager {
 public:
  uint32_t registerTexture(const TextureResource& resource);
  [[nodiscard]] std::optional<uint32_t> findTextureIndex(
      std::string_view name) const;
  [[nodiscard]] const TextureResource* getTexture(uint32_t index) const;
  [[nodiscard]] size_t textureCount() const noexcept { return textures_.size(); }

 private:
  std::vector<TextureResource> textures_{};
  std::unordered_map<std::string, uint32_t, StringHash, std::equal_to<>>
      textureNameToIndex_{};
};

}  // namespace utility::material
