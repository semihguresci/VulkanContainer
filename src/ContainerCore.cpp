#include <Container/ContainerCore.h>

namespace Container {

constexpr auto ContainerCore::version() noexcept -> Version {
  return {1, 0, 0};
}
}  // namespace Container