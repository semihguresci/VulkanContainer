#pragma once

#include "Container/geometry/DotBimLoader.h"

#include <filesystem>
#include <string_view>

namespace container::geometry::usd {

[[nodiscard]] dotbim::Model LoadFromFile(const std::filesystem::path& path,
                                         float importScale = 1.0f);
[[nodiscard]] dotbim::Model LoadFromText(std::string_view usdText,
                                         float importScale = 1.0f);

}  // namespace container::geometry::usd
