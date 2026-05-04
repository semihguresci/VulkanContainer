#pragma once

#include "Container/renderer/RenderTechnique.h"

namespace container::renderer {

class DeferredRasterTechnique final : public RenderTechnique {
 public:
  [[nodiscard]] RenderTechniqueId id() const override {
    return RenderTechniqueId::DeferredRaster;
  }
  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] std::string_view displayName() const override;
  [[nodiscard]] RenderTechniqueAvailability availability(
      const RenderSystemContext& context) const override;
  [[nodiscard]] TechniqueDebugModel debugModel() const override;

  void buildFrameGraph(RenderSystemContext& context) override;
};

}  // namespace container::renderer
