#pragma once

#include "Container/utility/FrameSyncManager.h"

#include <cstddef>
#include <string_view>

namespace container::renderer {

enum class FrameConcurrencyMode {
  CurrentFrameFence,
  SerializedGpuResources,
};

class FrameConcurrencyPolicy {
 public:
  static constexpr FrameConcurrencyPolicy currentFrameFence() {
    return {FrameConcurrencyMode::CurrentFrameFence, "per-frame resources"};
  }

  static constexpr FrameConcurrencyPolicy serializedGpuResources(
      std::string_view reason) {
    return {FrameConcurrencyMode::SerializedGpuResources, reason};
  }

  void waitBeforeAcquire(container::gpu::FrameSyncManager& syncManager,
                         size_t currentFrame) const {
    if (mode_ == FrameConcurrencyMode::SerializedGpuResources) {
      syncManager.waitForAllFrames();
      return;
    }
    syncManager.waitForFrame(currentFrame);
  }

  [[nodiscard]] FrameConcurrencyMode mode() const { return mode_; }
  [[nodiscard]] std::string_view reason() const { return reason_; }

 private:
  constexpr FrameConcurrencyPolicy(FrameConcurrencyMode mode,
                                   std::string_view reason)
      : mode_(mode), reason_(reason) {}

  FrameConcurrencyMode mode_{FrameConcurrencyMode::CurrentFrameFence};
  std::string_view reason_{};
};

}  // namespace container::renderer
