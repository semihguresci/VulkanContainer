#include "Container/renderer/resources/FrameResourceRegistry.h"

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

void FrameResourceRegistry::registerFramebuffer(
    RenderTechniqueId technique, std::string name, FrameFramebufferDesc desc,
    FrameResourceLifetime lifetime) {
  FrameResourceDesc resource{};
  resource.key = {.technique = technique, .name = std::move(name)};
  resource.kind = FrameResourceKind::Framebuffer;
  resource.lifetime = lifetime;
  resource.framebuffer = desc;
  registerResource(std::move(resource));
}

void FrameResourceRegistry::registerDescriptorSet(
    RenderTechniqueId technique, std::string name,
    FrameResourceLifetime lifetime) {
  FrameResourceDesc resource{};
  resource.key = {.technique = technique, .name = std::move(name)};
  resource.kind = FrameResourceKind::DescriptorSet;
  resource.lifetime = lifetime;
  registerResource(std::move(resource));
}

void FrameResourceRegistry::registerSampler(RenderTechniqueId technique,
                                            std::string name,
                                            FrameSamplerDesc desc,
                                            FrameResourceLifetime lifetime) {
  FrameResourceDesc resource{};
  resource.key = {.technique = technique, .name = std::move(name)};
  resource.kind = FrameResourceKind::Sampler;
  resource.lifetime = lifetime;
  resource.sampler = desc;
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

FrameResourceHandle FrameResourceRegistry::bindImage(
    RenderTechniqueId technique, std::string name, uint32_t frameIndex,
    FrameImageBinding image) {
  TechniqueResourceKey key{.technique = technique, .name = std::move(name)};
  validateKey(key);

  const std::size_t existing = findBindingIndex(key, frameIndex);
  if (existing != bindings_.size()) {
    FrameResourceBinding& binding = bindings_[existing];
    binding.kind = FrameResourceKind::Image;
    binding.image = image;
    binding.buffer = {};
    binding.framebuffer = {};
    binding.descriptor = {};
    binding.sampler = {};
    return binding.handle;
  }

  FrameResourceBinding binding{};
  binding.handle = FrameResourceHandle{nextBindingHandleId_++};
  binding.key = std::move(key);
  binding.frameIndex = frameIndex;
  binding.kind = FrameResourceKind::Image;
  binding.image = image;
  bindings_.push_back(std::move(binding));
  return bindings_.back().handle;
}

FrameResourceHandle FrameResourceRegistry::bindBuffer(
    RenderTechniqueId technique, std::string name, uint32_t frameIndex,
    FrameBufferBinding buffer) {
  TechniqueResourceKey key{.technique = technique, .name = std::move(name)};
  validateKey(key);

  const std::size_t existing = findBindingIndex(key, frameIndex);
  if (existing != bindings_.size()) {
    FrameResourceBinding& binding = bindings_[existing];
    binding.kind = FrameResourceKind::Buffer;
    binding.image = {};
    binding.buffer = buffer;
    binding.framebuffer = {};
    binding.descriptor = {};
    binding.sampler = {};
    return binding.handle;
  }

  FrameResourceBinding binding{};
  binding.handle = FrameResourceHandle{nextBindingHandleId_++};
  binding.key = std::move(key);
  binding.frameIndex = frameIndex;
  binding.kind = FrameResourceKind::Buffer;
  binding.buffer = buffer;
  bindings_.push_back(std::move(binding));
  return bindings_.back().handle;
}

FrameResourceHandle FrameResourceRegistry::bindFramebuffer(
    RenderTechniqueId technique, std::string name, uint32_t frameIndex,
    FrameFramebufferBinding framebuffer) {
  TechniqueResourceKey key{.technique = technique, .name = std::move(name)};
  validateKey(key);

  const std::size_t existing = findBindingIndex(key, frameIndex);
  if (existing != bindings_.size()) {
    FrameResourceBinding& binding = bindings_[existing];
    binding.kind = FrameResourceKind::Framebuffer;
    binding.image = {};
    binding.buffer = {};
    binding.framebuffer = framebuffer;
    binding.descriptor = {};
    binding.sampler = {};
    return binding.handle;
  }

  FrameResourceBinding binding{};
  binding.handle = FrameResourceHandle{nextBindingHandleId_++};
  binding.key = std::move(key);
  binding.frameIndex = frameIndex;
  binding.kind = FrameResourceKind::Framebuffer;
  binding.framebuffer = framebuffer;
  bindings_.push_back(std::move(binding));
  return bindings_.back().handle;
}

FrameResourceHandle FrameResourceRegistry::bindDescriptorSet(
    RenderTechniqueId technique, std::string name, uint32_t frameIndex,
    FrameDescriptorBinding descriptor) {
  TechniqueResourceKey key{.technique = technique, .name = std::move(name)};
  validateKey(key);

  const std::size_t existing = findBindingIndex(key, frameIndex);
  if (existing != bindings_.size()) {
    FrameResourceBinding& binding = bindings_[existing];
    binding.kind = FrameResourceKind::DescriptorSet;
    binding.image = {};
    binding.buffer = {};
    binding.framebuffer = {};
    binding.descriptor = descriptor;
    binding.sampler = {};
    return binding.handle;
  }

  FrameResourceBinding binding{};
  binding.handle = FrameResourceHandle{nextBindingHandleId_++};
  binding.key = std::move(key);
  binding.frameIndex = frameIndex;
  binding.kind = FrameResourceKind::DescriptorSet;
  binding.descriptor = descriptor;
  bindings_.push_back(std::move(binding));
  return bindings_.back().handle;
}

FrameResourceHandle FrameResourceRegistry::bindSampler(
    RenderTechniqueId technique, std::string name, uint32_t frameIndex,
    FrameSamplerBinding sampler) {
  TechniqueResourceKey key{.technique = technique, .name = std::move(name)};
  validateKey(key);

  const std::size_t existing = findBindingIndex(key, frameIndex);
  if (existing != bindings_.size()) {
    FrameResourceBinding& binding = bindings_[existing];
    binding.kind = FrameResourceKind::Sampler;
    binding.image = {};
    binding.buffer = {};
    binding.framebuffer = {};
    binding.descriptor = {};
    binding.sampler = sampler;
    return binding.handle;
  }

  FrameResourceBinding binding{};
  binding.handle = FrameResourceHandle{nextBindingHandleId_++};
  binding.key = std::move(key);
  binding.frameIndex = frameIndex;
  binding.kind = FrameResourceKind::Sampler;
  binding.sampler = sampler;
  bindings_.push_back(std::move(binding));
  return bindings_.back().handle;
}

const FrameResourceBinding* FrameResourceRegistry::findBinding(
    FrameResourceHandle handle) const {
  if (!handle.valid()) {
    return nullptr;
  }
  const auto it = std::find_if(
      bindings_.begin(), bindings_.end(),
      [handle](const FrameResourceBinding& binding) {
        return binding.handle == handle;
      });
  return it == bindings_.end() ? nullptr : &*it;
}

const FrameResourceBinding* FrameResourceRegistry::findBinding(
    const TechniqueResourceKey& key, uint32_t frameIndex) const {
  const std::size_t index = findBindingIndex(key, frameIndex);
  return index == bindings_.size() ? nullptr : &bindings_[index];
}

std::vector<const FrameResourceBinding*>
FrameResourceRegistry::bindingsForTechnique(RenderTechniqueId technique) const {
  std::vector<const FrameResourceBinding*> matches;
  for (const FrameResourceBinding& binding : bindings_) {
    if (binding.key.technique == technique) {
      matches.push_back(&binding);
    }
  }
  return matches;
}

std::vector<const FrameResourceBinding*> FrameResourceRegistry::bindingsForFrame(
    RenderTechniqueId technique, uint32_t frameIndex) const {
  std::vector<const FrameResourceBinding*> matches;
  for (const FrameResourceBinding& binding : bindings_) {
    if (binding.key.technique == technique &&
        binding.frameIndex == frameIndex) {
      matches.push_back(&binding);
    }
  }
  return matches;
}

void FrameResourceRegistry::clearTechnique(RenderTechniqueId technique) {
  std::erase_if(resources_, [technique](const FrameResourceDesc& resource) {
    return resource.key.technique == technique;
  });
  std::erase_if(bindings_, [technique](const FrameResourceBinding& binding) {
    return binding.key.technique == technique;
  });
}

void FrameResourceRegistry::clearBindings() { bindings_.clear(); }

void FrameResourceRegistry::clear() {
  resources_.clear();
  bindings_.clear();
}

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

std::size_t FrameResourceRegistry::findBindingIndex(
    const TechniqueResourceKey& key, uint32_t frameIndex) const {
  const auto it = std::find_if(
      bindings_.begin(), bindings_.end(),
      [&key, frameIndex](const FrameResourceBinding& binding) {
        return binding.key == key && binding.frameIndex == frameIndex;
      });
  if (it == bindings_.end()) {
    return bindings_.size();
  }
  return static_cast<std::size_t>(std::distance(bindings_.begin(), it));
}

}  // namespace container::renderer
