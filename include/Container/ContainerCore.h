#pragma once

namespace container {
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

  constexpr auto version() const noexcept -> Version;
};
}  // namespace container

