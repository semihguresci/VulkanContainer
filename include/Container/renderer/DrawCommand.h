#pragma once

#include <cstdint>

namespace container::renderer {

struct DrawCommand {
  uint32_t objectIndex{0};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
  uint32_t instanceCount{1};
};

} // namespace container::renderer
