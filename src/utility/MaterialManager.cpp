#include <Container/utility/MaterialManager.h>

namespace utility::material {

uint32_t TextureManager::registerTexture(const TextureResource& resource) {
  textures_.push_back(resource);
  return static_cast<uint32_t>(textures_.size() - 1);
}

const TextureResource* TextureManager::getTexture(uint32_t index) const {
  if (index >= textures_.size()) return nullptr;
  return &textures_[index];
}

uint32_t MaterialManager::createMaterial(const Material& material) {
  materials_.push_back(material);
  return static_cast<uint32_t>(materials_.size() - 1);
}

bool MaterialManager::updateMaterial(uint32_t index, const Material& material) {
  if (index >= materials_.size()) return false;
  materials_[index] = material;
  return true;
}

const Material* MaterialManager::getMaterial(uint32_t index) const {
  if (index >= materials_.size()) return nullptr;
  return &materials_[index];
}

}  // namespace utility::material

