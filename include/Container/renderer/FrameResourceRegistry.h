#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/RenderTechnique.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace container::renderer {

struct TechniqueResourceKey {
  RenderTechniqueId technique{RenderTechniqueId::DeferredRaster};
  std::string name{};

  [[nodiscard]] bool operator==(const TechniqueResourceKey& other) const {
    return technique == other.technique && name == other.name;
  }
};

enum class FrameResourceKind {
  Image,
  Buffer,
  External,
};

enum class FrameResourceLifetime {
  PerFrame,
  Persistent,
  Imported,
};

struct FrameImageDesc {
  VkFormat format{VK_FORMAT_UNDEFINED};
  VkExtent3D extent{0, 0, 1};
  VkImageUsageFlags usage{0};
};

struct FrameBufferDesc {
  VkDeviceSize size{0};
  VkBufferUsageFlags usage{0};
};

struct FrameResourceDesc {
  TechniqueResourceKey key{};
  FrameResourceKind kind{FrameResourceKind::External};
  FrameResourceLifetime lifetime{FrameResourceLifetime::PerFrame};
  FrameImageDesc image{};
  FrameBufferDesc buffer{};
};

class FrameResourceRegistry {
 public:
  void registerResource(FrameResourceDesc desc);
  void registerImage(RenderTechniqueId technique, std::string name,
                     FrameImageDesc desc,
                     FrameResourceLifetime lifetime =
                         FrameResourceLifetime::PerFrame);
  void registerBuffer(RenderTechniqueId technique, std::string name,
                      FrameBufferDesc desc,
                      FrameResourceLifetime lifetime =
                          FrameResourceLifetime::PerFrame);
  void registerExternal(RenderTechniqueId technique, std::string name);

  [[nodiscard]] const FrameResourceDesc* find(
      const TechniqueResourceKey& key) const;
  [[nodiscard]] bool contains(const TechniqueResourceKey& key) const {
    return find(key) != nullptr;
  }
  [[nodiscard]] std::vector<const FrameResourceDesc*> resourcesForTechnique(
      RenderTechniqueId technique) const;
  [[nodiscard]] std::size_t size() const { return resources_.size(); }

  void clearTechnique(RenderTechniqueId technique);
  void clear();

 private:
  [[nodiscard]] std::size_t findIndex(const TechniqueResourceKey& key) const;

  std::vector<FrameResourceDesc> resources_{};
};

}  // namespace container::renderer
