#pragma once

#include "Container/utility/SceneData.h"

#include <array>

namespace container::renderer {

struct ShadowCascadePreparationCascadeInputs {
  bool shadowPassActive{false};
  bool shadowPassRecordable{false};
  bool sceneSingleSidedUsesGpuCull{false};
};

struct ShadowCascadePreparationPlanInputs {
  bool shadowAtlasVisible{false};
  bool hasSceneSingleSidedDraws{false};
  bool hasSceneWindingFlippedDraws{false};
  bool hasSceneDoubleSidedDraws{false};
  bool hasBimShadowGeometry{false};
  std::array<ShadowCascadePreparationCascadeInputs,
             container::gpu::kShadowCascadeCount>
      cascades{};
};

struct ShadowCascadePreparationPlan {
  bool prepareDrawCommands{false};
};

[[nodiscard]] ShadowCascadePreparationPlan
buildShadowCascadePreparationPlan(
    const ShadowCascadePreparationPlanInputs &inputs);

} // namespace container::renderer
