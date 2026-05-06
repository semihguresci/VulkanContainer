#pragma once

#include <cstddef>

namespace container::renderer {

inline constexpr size_t kMinShadowSecondaryCommandBufferCpuCommands = 512u;

struct ShadowSecondaryCommandBufferPlanInputs {
  bool usesGpuFilteredBimMeshShadowPath{false};
  bool secondaryCommandBuffersEnabled{false};
  bool shadowPassRecordable{false};
  bool secondaryCommandBufferAvailable{false};
  size_t cpuCommandCount{0u};
};

struct ShadowSecondaryCommandBufferPlan {
  bool useSecondaryCommandBuffer{false};
};

[[nodiscard]] ShadowSecondaryCommandBufferPlan
buildShadowSecondaryCommandBufferPlan(
    const ShadowSecondaryCommandBufferPlanInputs &inputs);

} // namespace container::renderer
