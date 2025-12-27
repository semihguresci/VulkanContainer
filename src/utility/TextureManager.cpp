#include <Container/utility/TextureManager.h>
#include <Container/utility/TextureResource.h>


namespace utility::material {

uint32_t TextureManager::registerTexture(const TextureResource& resource) {
  if (!resource.name.empty()) {
    if (const auto existing = textureNameToIndex_.find(resource.name);
        existing != textureNameToIndex_.end()) {
      return existing->second;
    }
  }

  textures_.push_back(resource);
  const auto index = static_cast<uint32_t>(textures_.size() - 1);
  if (!resource.name.empty()) {
    textureNameToIndex_.emplace(resource.name, index);
  }
  return index;
}

std::optional<uint32_t> TextureManager::findTextureIndex(
    const std::string& name) const {
  if (name.empty()) return std::nullopt;
  if (const auto it = textureNameToIndex_.find(name);
      it != textureNameToIndex_.end()) {
    return it->second;
  }
  return std::nullopt;
}

const TextureResource* TextureManager::getTexture(uint32_t index) const {
  if (index >= textures_.size()) return nullptr;
  return &textures_[index];
}

}  // namespace utility::material