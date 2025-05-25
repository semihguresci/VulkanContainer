#ifndef CONTAINER_CORE_H
#define CONTAINER_CORE_H

namespace Container {
struct Version {
  int major;
  int minor;
  int patch;
};

class ContainerCore {
 public:
  ContainerCore() = default;
  virtual ~ContainerCore() = default;
  ContainerCore(const ContainerCore&) = delete;
  ContainerCore& operator=(const ContainerCore&) = delete;
  ContainerCore(ContainerCore&&) = delete;
  ContainerCore& operator=(ContainerCore&&) = delete;

  constexpr auto version() noexcept -> Version;
};
}  // namespace Container

#endif