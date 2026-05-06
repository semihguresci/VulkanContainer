#pragma once

#include "Container/renderer/core/RenderGraph.h"

namespace container::renderer {

struct DeferredRasterHiZPassPlanInputs {
  bool gpuCullManagerReady{false};
  bool frameReady{false};
  bool depthSamplingViewReady{false};
  bool depthSamplerReady{false};
  bool depthStencilImageReady{false};
};

struct DeferredRasterHiZPassPlan {
  bool active{false};
  RenderPassReadiness readiness{};
};

[[nodiscard]] DeferredRasterHiZPassPlan
buildDeferredRasterHiZPassPlan(const DeferredRasterHiZPassPlanInputs &inputs);

} // namespace container::renderer
