#pragma once

namespace container::renderer {

// Runtime debug rendering toggles.
// Passed into FrameRecordParams each frame.
struct DebugRenderState {
  bool directionalOnly{false};
  bool visualizePointLightStencil{false};

  // Key-down trackers for edge-triggered toggle logic.
  bool directionalOnlyKeyDown{false};
  bool visualizePointLightStencilKeyDown{false};
};

}  // namespace container::renderer
