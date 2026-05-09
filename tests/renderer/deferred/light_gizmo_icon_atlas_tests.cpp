#include "Container/renderer/lighting/LightGizmoIconAtlas.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using container::renderer::EditableLightType;
using container::renderer::RasterizedLightGizmoIcon;
using container::renderer::buildLightGizmoIconAtlasRgba;
using container::renderer::kLightGizmoIconLayerCount;
using container::renderer::lightGizmoIconAssetPaths;
using container::renderer::lightGizmoIconLayerForType;
using container::renderer::rasterizeLightGizmoSvg;

[[nodiscard]] uint8_t alphaAt(const RasterizedLightGizmoIcon &icon, uint32_t x,
                              uint32_t y) {
  const size_t offset =
      (static_cast<size_t>(y) * icon.width + static_cast<size_t>(x)) * 4u + 3u;
  return std::to_integer<uint8_t>(icon.rgba.at(offset));
}

} // namespace

TEST(LightGizmoIconAtlasTests, MapsEditableLightTypesToStableSvgLayers) {
  EXPECT_EQ(lightGizmoIconLayerForType(EditableLightType::Directional), 0u);
  EXPECT_EQ(lightGizmoIconLayerForType(EditableLightType::Point), 1u);
  EXPECT_EQ(lightGizmoIconLayerForType(EditableLightType::Spot), 2u);
  EXPECT_EQ(lightGizmoIconLayerForType(EditableLightType::Area), 3u);
  EXPECT_EQ(kLightGizmoIconLayerCount, 4u);
}

TEST(LightGizmoIconAtlasTests, BuildsExpectedSvgAssetPaths) {
  const std::filesystem::path root = "materials/gizmos/lights";
  const auto paths = lightGizmoIconAssetPaths(root);

  ASSERT_EQ(paths.size(), kLightGizmoIconLayerCount);
  EXPECT_EQ(paths[0].filename().string(), "directional.svg");
  EXPECT_EQ(paths[1].filename().string(), "point.svg");
  EXPECT_EQ(paths[2].filename().string(), "spot.svg");
  EXPECT_EQ(paths[3].filename().string(), "area.svg");
  EXPECT_EQ(paths[2].parent_path(), root);
}

TEST(LightGizmoIconAtlasTests, RasterizesSimpleCircleSvgIntoRgbaIcon) {
  const std::string svg =
      R"(<svg viewBox="0 0 16 16"><circle cx="8" cy="8" r="4" fill="#ffffff"/></svg>)";

  const RasterizedLightGizmoIcon icon = rasterizeLightGizmoSvg(svg, 16u);

  EXPECT_EQ(icon.width, 16u);
  EXPECT_EQ(icon.height, 16u);
  EXPECT_EQ(icon.rgba.size(), 16u * 16u * 4u);
  EXPECT_GT(alphaAt(icon, 8u, 8u), 200u);
  EXPECT_EQ(alphaAt(icon, 0u, 0u), 0u);
}

TEST(LightGizmoIconAtlasTests, BuildsLayerOrderedRgbaAtlas) {
  std::array<RasterizedLightGizmoIcon, kLightGizmoIconLayerCount> icons{};
  for (uint32_t layer = 0u; layer < kLightGizmoIconLayerCount; ++layer) {
    icons[layer].width = 2u;
    icons[layer].height = 2u;
    icons[layer].rgba.assign(2u * 2u * 4u, std::byte{0});
    icons[layer].rgba[3] = static_cast<std::byte>(10u + layer);
  }

  const std::vector<std::byte> atlas = buildLightGizmoIconAtlasRgba(icons);

  ASSERT_EQ(atlas.size(), kLightGizmoIconLayerCount * 2u * 2u * 4u);
  for (uint32_t layer = 0u; layer < kLightGizmoIconLayerCount; ++layer) {
    const size_t offset = static_cast<size_t>(layer) * 2u * 2u * 4u + 3u;
    EXPECT_EQ(std::to_integer<uint8_t>(atlas[offset]), 10u + layer);
  }
}
