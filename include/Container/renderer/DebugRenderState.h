#pragma once

namespace container::renderer {

// Runtime debug rendering toggles.
// Passed into FrameRecordParams each frame.
struct DebugRenderState {
  bool directionalOnly{false};
  bool visualizePointLightStencil{false};
  bool freezeCulling{false};
};

}  // namespace container::renderer
