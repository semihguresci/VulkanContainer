#pragma once

#include "Container/renderer/core/TechniqueDebugModel.h"
#include "Container/scene/SceneProvider.h"

#include <string>
#include <vector>

namespace container::renderer {

[[nodiscard]] inline std::string sceneProviderKindName(
    container::scene::SceneProviderKind kind) {
  switch (kind) {
    case container::scene::SceneProviderKind::Mesh:
      return "mesh";
    case container::scene::SceneProviderKind::Bim:
      return "bim";
    case container::scene::SceneProviderKind::GaussianSplatting:
      return "gaussian-splatting";
    case container::scene::SceneProviderKind::RadianceField:
      return "radiance-field";
  }
  return "unknown";
}

[[nodiscard]] inline SceneDebugModel buildSceneDebugModel(
    const std::vector<container::scene::SceneProviderSnapshot>& providers) {
  SceneDebugModel model{};
  model.providers.reserve(providers.size());
  for (const container::scene::SceneProviderSnapshot& provider : providers) {
    model.providers.push_back(SceneProviderDebugState{
        .providerId = provider.id.value,
        .displayName = provider.displayName,
        .kind = sceneProviderKindName(provider.kind),
        .elementCount = provider.elementCount,
    });
  }
  return model;
}

}  // namespace container::renderer
