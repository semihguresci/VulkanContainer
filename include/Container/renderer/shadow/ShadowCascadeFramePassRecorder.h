#pragma once

#include "Container/renderer/core/RenderGraph.h"
#include "Container/renderer/shadow/ShadowCascadeDrawPlanner.h"
#include "Container/renderer/shadow/ShadowPassRecorder.h"

#include <functional>

namespace container::renderer {

struct FrameRecordParams;

struct ShadowCascadeFramePassContext {
  bool shadowAtlasVisible{false};
  std::function<bool(RenderPassId)> isPassActive{};
};

class ShadowCascadeFramePassRecorder {
public:
  void prepareFrame(const FrameRecordParams &params,
                    const ShadowCascadeFramePassContext &context) const;

  [[nodiscard]] bool canRecordCascade(const FrameRecordParams &params,
                                      uint32_t cascadeIndex) const;

  void recordCascadePass(VkCommandBuffer cmd, const FrameRecordParams &params,
                         const ShadowCascadeFramePassContext &context,
                         uint32_t cascadeIndex) const;

private:
  [[nodiscard]] bool
  shouldPrepareDrawCommands(const FrameRecordParams &params,
                            const ShadowCascadeFramePassContext &context) const;

  [[nodiscard]] bool
  useGpuCullForCascade(const FrameRecordParams &params,
                       const ShadowCascadeFramePassContext &context,
                       uint32_t cascadeIndex) const;

  [[nodiscard]] size_t
  cpuCommandCount(const FrameRecordParams &params,
                  const ShadowCascadeFramePassContext &context,
                  uint32_t cascadeIndex) const;

  [[nodiscard]] bool shouldUseSecondaryCommandBuffer(
      const FrameRecordParams &params,
      const ShadowCascadeFramePassContext &context,
      uint32_t cascadeIndex) const;

  [[nodiscard]] ShadowCascadeDrawPlannerInputs
  drawPlannerInputs(const FrameRecordParams &params,
                    const ShadowCascadeFramePassContext &context) const;

  [[nodiscard]] ShadowCascadePassRecordInputs cascadePassRecordInputs(
      const FrameRecordParams &params,
      const ShadowCascadeFramePassContext &context,
      uint32_t cascadeIndex) const;

  [[nodiscard]] ShadowCascadeSecondaryPassRecordInputs
  secondaryPassRecordInputs(const FrameRecordParams &params,
                            const ShadowCascadeFramePassContext &context) const;

  void prepareDrawCommands(const FrameRecordParams &params,
                           const ShadowCascadeFramePassContext &context) const;

  void clearDrawCommandCache() const;

  mutable ShadowCascadeDrawPlan drawPlanCache_{};
  mutable bool drawCommandCacheValid_{false};
};

} // namespace container::renderer
