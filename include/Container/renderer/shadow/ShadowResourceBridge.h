#pragma once

#include "Container/renderer/core/FrameRecorder.h"

#include <string_view>

namespace container::renderer {

enum class ShadowDescriptorSetId {
  Scene,
  BimScene,
  Shadow,
};

[[nodiscard]] inline std::string_view shadowDescriptorSetKey(
    ShadowDescriptorSetId id) {
  switch (id) {
  case ShadowDescriptorSetId::Scene:
    return "scene-descriptor-set";
  case ShadowDescriptorSetId::BimScene:
    return "bim-scene-descriptor-set";
  case ShadowDescriptorSetId::Shadow:
    return "shadow-descriptor-set";
  }
  return {};
}

[[nodiscard]] inline const FrameDescriptorBinding *shadowDescriptorBinding(
    const FrameRecordParams &p, ShadowDescriptorSetId id) {
  return p.descriptorBinding(RenderTechniqueId::DeferredRaster,
                             shadowDescriptorSetKey(id));
}

[[nodiscard]] inline VkDescriptorSet shadowDescriptorSet(
    const FrameRecordParams &p, ShadowDescriptorSetId id) {
  return p.descriptorSet(RenderTechniqueId::DeferredRaster,
                         shadowDescriptorSetKey(id));
}

[[nodiscard]] inline bool shadowDescriptorSetReady(
    const FrameRecordParams &p, ShadowDescriptorSetId id) {
  return shadowDescriptorSet(p, id) != VK_NULL_HANDLE;
}

}  // namespace container::renderer
