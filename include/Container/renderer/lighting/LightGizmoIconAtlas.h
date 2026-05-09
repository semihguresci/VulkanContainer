#pragma once

#include "Container/renderer/lighting/EditableLight.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace container::renderer {

inline constexpr uint32_t kLightGizmoIconSize = 64u;
inline constexpr uint32_t kLightGizmoIconLayerCount = 4u;

struct RasterizedLightGizmoIcon {
  uint32_t width{0u};
  uint32_t height{0u};
  std::vector<std::byte> rgba{};
};

[[nodiscard]] uint32_t lightGizmoIconLayerForType(EditableLightType type);

[[nodiscard]] std::array<std::filesystem::path, kLightGizmoIconLayerCount>
lightGizmoIconAssetPaths(const std::filesystem::path &assetRoot);

[[nodiscard]] RasterizedLightGizmoIcon
rasterizeLightGizmoSvg(std::string_view svg,
                       uint32_t size = kLightGizmoIconSize);

[[nodiscard]] std::vector<std::byte> buildLightGizmoIconAtlasRgba(
    std::span<const RasterizedLightGizmoIcon, kLightGizmoIconLayerCount> icons);

[[nodiscard]] std::vector<std::byte>
loadLightGizmoIconAtlasRgba(const std::filesystem::path &assetRoot,
                            uint32_t size = kLightGizmoIconSize);

} // namespace container::renderer
