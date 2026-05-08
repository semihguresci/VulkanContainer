#pragma once

#include "Container/renderer/core/RenderGraph.h"

#include <cstdint>

namespace container::renderer {

enum class DeferredRasterFrustumCullFreezeAction : uint8_t {
  None,
  Freeze,
  Unfreeze,
};

struct DeferredRasterFrustumCullPassPlanInputs {
  bool gpuCullManagerReady{false};
  bool sceneSingleSidedDrawsAvailable{false};
  bool cameraBufferReady{false};
  bool objectBufferReady{false};
  bool debugFreezeCulling{false};
  bool cullingFrozen{false};
  uint32_t sourceDrawCount{0u};
};

struct DeferredRasterFrustumCullPassPlan {
  bool active{false};
  RenderPassReadiness readiness{};
  uint32_t drawCount{0u};
  bool updateObjectDescriptor{false};
  DeferredRasterFrustumCullFreezeAction freezeAction{
      DeferredRasterFrustumCullFreezeAction::None};
};

[[nodiscard]] DeferredRasterFrustumCullPassPlan
buildDeferredRasterFrustumCullPassPlan(
    const DeferredRasterFrustumCullPassPlanInputs &inputs);

} // namespace container::renderer
