#pragma once

#include "Container/common/CommonMath.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace container::renderer {

class BimFloorPlanOverlayData {
 public:
  std::vector<DrawCommand> drawCommands{};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  glm::vec3 boundsCenter{0.0f};
  float boundsRadius{0.0f};

  [[nodiscard]] bool valid() const {
    return indexCount >= 2u && !drawCommands.empty();
  }
};

}  // namespace container::renderer
