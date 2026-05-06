#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace container::renderer {

enum class BimSemanticColorMode : uint32_t {
  Off = 0,
  Type = 1,
  Storey = 2,
  Material = 3,
  FireRating = 4,
  LoadBearing = 5,
  Status = 6,
};

inline constexpr std::array<BimSemanticColorMode, 7> kBimSemanticColorModes{{
    BimSemanticColorMode::Off,
    BimSemanticColorMode::Type,
    BimSemanticColorMode::Storey,
    BimSemanticColorMode::Material,
    BimSemanticColorMode::FireRating,
    BimSemanticColorMode::LoadBearing,
    BimSemanticColorMode::Status,
}};

[[nodiscard]] constexpr std::string_view bimSemanticColorModeLabel(
    BimSemanticColorMode mode) {
  switch (mode) {
    case BimSemanticColorMode::Off:
      return "Original materials";
    case BimSemanticColorMode::Type:
      return "IFC class / type";
    case BimSemanticColorMode::Storey:
      return "Storey";
    case BimSemanticColorMode::Material:
      return "Material";
    case BimSemanticColorMode::FireRating:
      return "Fire rating";
    case BimSemanticColorMode::LoadBearing:
      return "Load-bearing";
    case BimSemanticColorMode::Status:
      return "Status";
  }
  return "Original materials";
}

}  // namespace container::renderer
