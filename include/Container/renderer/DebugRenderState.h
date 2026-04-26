#pragma once

namespace container::renderer {

// Runtime debug rendering toggles.
// Passed into FrameRecordParams each frame.
struct DebugRenderState {
  bool directionalOnly{false};
  bool visualizePointLightStencil{false};
  bool freezeCulling{false};

  // Key-down trackers for edge-triggered toggle logic.
  bool directionalOnlyKeyDown{false};
  bool visualizePointLightStencilKeyDown{false};
  bool freezeCullingKeyDown{false};
};

}  // namespace container::renderer
