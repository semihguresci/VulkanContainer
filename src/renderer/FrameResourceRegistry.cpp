#include "Container/renderer/FrameResourceRegistry.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace container::renderer {

namespace {

void validateKey(const TechniqueResourceKey& key) {
  if (key.name.empty()) {
    throw std::invalid_argument("frame resource name must not be empty");
  }
}

}  // namespace

void FrameResourceRegistry::registerResource(FrameResourceDesc desc) {
  validateKey(desc.key);
  if (contains(desc.key)) {
    throw std::invalid_argument("frame resource is already registered");
  }
  resources_.push_back(std::move(desc));
}

void FrameResourceRegistry::registerImage(RenderTechniqueId technique,
                                          std::string name,
                                          FrameImageDesc desc,
                                          FrameResourceLifetime lifetime) {
  FrameResourceDesc resource{};
  resource.key = {.technique = technique, .name = std::move(name)};
  resource.kind = FrameResourceKind::Image;
  resource.lifetime = lifetime;
  resource.image = desc;
  registerResource(std::move(resource));
}

void FrameResourceRegistry::registerBuffer(RenderTechniqueId technique,
                                           std::string name,
                                           FrameBufferDesc desc,
                                           FrameResourceLifetime lifetime) {
  FrameResourceDesc resource{};
  resource.key = {.technique = technique, .name = std::move(name)};
  resource.kind = FrameResourceKind::Buffer;
  resource.lifetime = lifetime;
  resource.buffer = desc;
  registerResource(std::move(resource));
}

void FrameResourceRegistry::registerExternal(RenderTechniqueId technique,
                                             std::string name) {
  FrameResourceDesc resource{};
  resource.key = {.technique = technique, .name = std::move(name)};
  resource.kind = FrameResourceKind::External;
  resource.lifetime = FrameResourceLifetime::Imported;
  registerResource(std::move(resource));
}

const FrameResourceDesc* FrameResourceRegistry::find(
    const TechniqueResourceKey& key) const {
  const std::size_t index = findIndex(key);
  return index == resources_.size() ? nullptr : &resources_[index];
}

std::vector<const FrameResourceDesc*>
FrameResourceRegistry::resourcesForTechnique(RenderTechniqueId technique) const {
  std::vector<const FrameResourceDesc*> matches;
  for (const FrameResourceDesc& resource : resources_) {
    if (resource.key.technique == technique) {
      matches.push_back(&resource);
    }
  }
  return matches;
}

void FrameResourceRegistry::clearTechnique(RenderTechniqueId technique) {
  std::erase_if(resources_, [technique](const FrameResourceDesc& resource) {
    return resource.key.technique == technique;
  });
}

void FrameResourceRegistry::clear() { resources_.clear(); }

std::size_t FrameResourceRegistry::findIndex(
    const TechniqueResourceKey& key) const {
  const auto it =
      std::find_if(resources_.begin(), resources_.end(),
                   [&key](const FrameResourceDesc& resource) {
                     return resource.key == key;
                   });
  if (it == resources_.end()) {
    return resources_.size();
  }
  return static_cast<std::size_t>(std::distance(resources_.begin(), it));
}

}  // namespace container::renderer
