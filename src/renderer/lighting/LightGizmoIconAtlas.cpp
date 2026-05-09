#include "Container/renderer/lighting/LightGizmoIconAtlas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <glm/glm.hpp>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace container::renderer {

namespace {

struct SvgViewBox {
  float minX{0.0f};
  float minY{0.0f};
  float width{64.0f};
  float height{64.0f};
};

struct Rgba8 {
  uint8_t r{255u};
  uint8_t g{255u};
  uint8_t b{255u};
  uint8_t a{255u};
};

[[nodiscard]] std::string toString(std::string_view value) {
  return std::string(value.begin(), value.end());
}

[[nodiscard]] std::string trimAscii(std::string_view value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](char c) {
                      return c == ' ' || c == '\t' || c == '\r' || c == '\n';
                    }).base();
  return first < last ? std::string(first, last) : std::string{};
}

[[nodiscard]] std::string attributeValue(std::string_view tag,
                                         std::string_view name) {
  const std::string key = toString(name) + "=";
  const size_t keyPos = tag.find(key);
  if (keyPos == std::string_view::npos) {
    return {};
  }
  const size_t valueStart = keyPos + key.size();
  if (valueStart >= tag.size()) {
    return {};
  }
  const char quote = tag[valueStart];
  if (quote != '"' && quote != '\'') {
    return {};
  }
  const size_t end = tag.find(quote, valueStart + 1u);
  if (end == std::string_view::npos) {
    return {};
  }
  return toString(tag.substr(valueStart + 1u, end - valueStart - 1u));
}

[[nodiscard]] float floatAttribute(std::string_view tag, std::string_view name,
                                   float fallback) {
  const std::string value = attributeValue(tag, name);
  if (value.empty()) {
    return fallback;
  }
  try {
    return std::stof(value);
  } catch (...) {
    return fallback;
  }
}

[[nodiscard]] std::vector<float> parseFloatList(std::string_view text) {
  std::string normalized = toString(text);
  for (char &c : normalized) {
    if (c == ',') {
      c = ' ';
    }
  }

  std::istringstream stream(normalized);
  std::vector<float> values;
  float value = 0.0f;
  while (stream >> value) {
    values.push_back(value);
  }
  return values;
}

[[nodiscard]] SvgViewBox parseViewBox(std::string_view svg) {
  const size_t svgStart = svg.find("<svg");
  if (svgStart == std::string_view::npos) {
    return {};
  }
  const size_t svgEnd = svg.find('>', svgStart);
  if (svgEnd == std::string_view::npos) {
    return {};
  }
  const std::string viewBox =
      attributeValue(svg.substr(svgStart, svgEnd - svgStart + 1u), "viewBox");
  const std::vector<float> values = parseFloatList(viewBox);
  if (values.size() != 4u || values[2] <= 0.0f || values[3] <= 0.0f) {
    return {};
  }
  return {.minX = values[0],
          .minY = values[1],
          .width = values[2],
          .height = values[3]};
}

[[nodiscard]] uint8_t hexByte(std::string_view text) {
  if (text.size() != 2u) {
    return 255u;
  }
  const auto nibble = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') {
      return static_cast<uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
      return static_cast<uint8_t>(10 + c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
      return static_cast<uint8_t>(10 + c - 'A');
    }
    return 15u;
  };
  return static_cast<uint8_t>((nibble(text[0]) << 4u) | nibble(text[1]));
}

[[nodiscard]] Rgba8 colorAttribute(std::string_view tag,
                                   std::string_view name, Rgba8 fallback) {
  std::string value = trimAscii(attributeValue(tag, name));
  if (value.empty()) {
    return fallback;
  }
  if (value == "none") {
    return {.r = fallback.r, .g = fallback.g, .b = fallback.b, .a = 0u};
  }
  if (value.size() == 7u && value[0] == '#') {
    fallback.r = hexByte(std::string_view(value).substr(1u, 2u));
    fallback.g = hexByte(std::string_view(value).substr(3u, 2u));
    fallback.b = hexByte(std::string_view(value).substr(5u, 2u));
    fallback.a = 255u;
  }
  return fallback;
}

[[nodiscard]] Rgba8 withOpacity(Rgba8 color, float opacity) {
  const float clamped = std::clamp(opacity, 0.0f, 1.0f);
  color.a = static_cast<uint8_t>(
      std::round(static_cast<float>(color.a) * clamped));
  return color;
}

void blendPixel(RasterizedLightGizmoIcon &icon, uint32_t x, uint32_t y,
                Rgba8 src) {
  if (src.a == 0u) {
    return;
  }

  const size_t offset =
      (static_cast<size_t>(y) * icon.width + static_cast<size_t>(x)) * 4u;
  const float srcA = static_cast<float>(src.a) / 255.0f;
  const float dstA =
      static_cast<float>(std::to_integer<uint8_t>(icon.rgba[offset + 3u])) /
      255.0f;
  const float outA = srcA + dstA * (1.0f - srcA);
  if (outA <= 0.0f) {
    return;
  }

  const auto dstChannel = [&](size_t channel) {
    return static_cast<float>(
               std::to_integer<uint8_t>(icon.rgba[offset + channel])) /
           255.0f;
  };
  const std::array<float, 3> srcRgb = {
      static_cast<float>(src.r) / 255.0f,
      static_cast<float>(src.g) / 255.0f,
      static_cast<float>(src.b) / 255.0f,
  };
  for (size_t channel = 0u; channel < 3u; ++channel) {
    const float dst = dstChannel(channel);
    const float out =
        (srcRgb[channel] * srcA + dst * dstA * (1.0f - srcA)) / outA;
    icon.rgba[offset + channel] = static_cast<std::byte>(static_cast<uint8_t>(
        std::round(std::clamp(out, 0.0f, 1.0f) * 255.0f)));
  }
  icon.rgba[offset + 3u] = static_cast<std::byte>(static_cast<uint8_t>(
      std::round(std::clamp(outA, 0.0f, 1.0f) * 255.0f)));
}

[[nodiscard]] float distanceToSegment(glm::vec2 point, glm::vec2 a,
                                      glm::vec2 b) {
  const glm::vec2 segment = b - a;
  const float len2 = glm::dot(segment, segment);
  if (len2 <= std::numeric_limits<float>::epsilon()) {
    return glm::length(point - a);
  }
  const float t = std::clamp(glm::dot(point - a, segment) / len2, 0.0f, 1.0f);
  return glm::length(point - (a + segment * t));
}

[[nodiscard]] bool pointInPolygon(glm::vec2 point,
                                  std::span<const glm::vec2> points) {
  bool inside = false;
  for (size_t i = 0u, j = points.size() - 1u; i < points.size(); j = i++) {
    const glm::vec2 a = points[i];
    const glm::vec2 b = points[j];
    const bool crosses =
        ((a.y > point.y) != (b.y > point.y)) &&
        (point.x < (b.x - a.x) * (point.y - a.y) /
                           ((b.y - a.y) + 1.0e-6f) +
                       a.x);
    if (crosses) {
      inside = !inside;
    }
  }
  return inside;
}

template <typename Predicate>
void drawPredicate(RasterizedLightGizmoIcon &icon, const SvgViewBox &viewBox,
                   Rgba8 color, Predicate predicate) {
  for (uint32_t y = 0u; y < icon.height; ++y) {
    const float svgY = viewBox.minY +
                       (static_cast<float>(y) + 0.5f) /
                           static_cast<float>(icon.height) * viewBox.height;
    for (uint32_t x = 0u; x < icon.width; ++x) {
      const float svgX = viewBox.minX +
                         (static_cast<float>(x) + 0.5f) /
                             static_cast<float>(icon.width) * viewBox.width;
      if (predicate(glm::vec2(svgX, svgY))) {
        blendPixel(icon, x, y, color);
      }
    }
  }
}

void drawCircle(RasterizedLightGizmoIcon &icon, const SvgViewBox &viewBox,
                std::string_view tag) {
  const glm::vec2 center{floatAttribute(tag, "cx", 0.0f),
                         floatAttribute(tag, "cy", 0.0f)};
  const float radius = std::max(0.0f, floatAttribute(tag, "r", 0.0f));
  const float opacity = floatAttribute(tag, "opacity", 1.0f);
  const Rgba8 fill = withOpacity(
      colorAttribute(tag, "fill", {.r = 255u, .g = 255u, .b = 255u, .a = 255u}),
      floatAttribute(tag, "fill-opacity", opacity));
  const Rgba8 stroke = withOpacity(
      colorAttribute(tag, "stroke",
                     {.r = 255u, .g = 255u, .b = 255u, .a = 0u}),
      floatAttribute(tag, "stroke-opacity", opacity));
  const float strokeWidth =
      std::max(0.0f, floatAttribute(tag, "stroke-width", 0.0f));

  drawPredicate(icon, viewBox, fill, [&](glm::vec2 point) {
    return glm::length(point - center) <= radius;
  });
  if (stroke.a != 0u && strokeWidth > 0.0f) {
    drawPredicate(icon, viewBox, stroke, [&](glm::vec2 point) {
      return std::abs(glm::length(point - center) - radius) <=
             strokeWidth * 0.5f;
    });
  }
}

void drawRect(RasterizedLightGizmoIcon &icon, const SvgViewBox &viewBox,
              std::string_view tag) {
  const float x = floatAttribute(tag, "x", 0.0f);
  const float y = floatAttribute(tag, "y", 0.0f);
  const float width = std::max(0.0f, floatAttribute(tag, "width", 0.0f));
  const float height = std::max(0.0f, floatAttribute(tag, "height", 0.0f));
  const float opacity = floatAttribute(tag, "opacity", 1.0f);
  const Rgba8 fill = withOpacity(
      colorAttribute(tag, "fill", {.r = 255u, .g = 255u, .b = 255u, .a = 255u}),
      floatAttribute(tag, "fill-opacity", opacity));
  const Rgba8 stroke = withOpacity(
      colorAttribute(tag, "stroke",
                     {.r = 255u, .g = 255u, .b = 255u, .a = 0u}),
      floatAttribute(tag, "stroke-opacity", opacity));
  const float strokeWidth =
      std::max(0.0f, floatAttribute(tag, "stroke-width", 0.0f));

  drawPredicate(icon, viewBox, fill, [&](glm::vec2 point) {
    return point.x >= x && point.x <= x + width && point.y >= y &&
           point.y <= y + height;
  });
  if (stroke.a != 0u && strokeWidth > 0.0f) {
    drawPredicate(icon, viewBox, stroke, [&](glm::vec2 point) {
      const bool inOuter = point.x >= x - strokeWidth * 0.5f &&
                           point.x <= x + width + strokeWidth * 0.5f &&
                           point.y >= y - strokeWidth * 0.5f &&
                           point.y <= y + height + strokeWidth * 0.5f;
      const bool inInner = point.x >= x + strokeWidth * 0.5f &&
                           point.x <= x + width - strokeWidth * 0.5f &&
                           point.y >= y + strokeWidth * 0.5f &&
                           point.y <= y + height - strokeWidth * 0.5f;
      return inOuter && !inInner;
    });
  }
}

void drawLine(RasterizedLightGizmoIcon &icon, const SvgViewBox &viewBox,
              std::string_view tag) {
  const glm::vec2 a{floatAttribute(tag, "x1", 0.0f),
                    floatAttribute(tag, "y1", 0.0f)};
  const glm::vec2 b{floatAttribute(tag, "x2", 0.0f),
                    floatAttribute(tag, "y2", 0.0f)};
  const float opacity = floatAttribute(tag, "opacity", 1.0f);
  const Rgba8 stroke = withOpacity(
      colorAttribute(tag, "stroke",
                     {.r = 255u, .g = 255u, .b = 255u, .a = 255u}),
      floatAttribute(tag, "stroke-opacity", opacity));
  const float strokeWidth =
      std::max(1.0f, floatAttribute(tag, "stroke-width", 1.0f));

  drawPredicate(icon, viewBox, stroke, [&](glm::vec2 point) {
    return distanceToSegment(point, a, b) <= strokeWidth * 0.5f;
  });
}

void drawPolygon(RasterizedLightGizmoIcon &icon, const SvgViewBox &viewBox,
                 std::string_view tag) {
  const std::vector<float> values =
      parseFloatList(attributeValue(tag, "points"));
  if (values.size() < 6u || (values.size() % 2u) != 0u) {
    return;
  }

  std::vector<glm::vec2> points;
  points.reserve(values.size() / 2u);
  for (size_t i = 0u; i < values.size(); i += 2u) {
    points.emplace_back(values[i], values[i + 1u]);
  }

  const float opacity = floatAttribute(tag, "opacity", 1.0f);
  const Rgba8 fill = withOpacity(
      colorAttribute(tag, "fill", {.r = 255u, .g = 255u, .b = 255u, .a = 255u}),
      floatAttribute(tag, "fill-opacity", opacity));
  drawPredicate(icon, viewBox, fill, [&](glm::vec2 point) {
    return pointInPolygon(point,
                          std::span<const glm::vec2>(points.data(),
                                                     points.size()));
  });
}

[[nodiscard]] bool startsWith(std::string_view value, std::string_view prefix) {
  return value.substr(0u, prefix.size()) == prefix;
}

[[nodiscard]] std::string readTextFile(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("failed to open light gizmo SVG: " +
                             path.string());
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

} // namespace

uint32_t lightGizmoIconLayerForType(EditableLightType type) {
  switch (type) {
  case EditableLightType::Directional:
    return 0u;
  case EditableLightType::Point:
    return 1u;
  case EditableLightType::Spot:
    return 2u;
  case EditableLightType::Area:
    return 3u;
  }
  return 1u;
}

std::array<std::filesystem::path, kLightGizmoIconLayerCount>
lightGizmoIconAssetPaths(const std::filesystem::path &assetRoot) {
  return {
      assetRoot / "directional.svg",
      assetRoot / "point.svg",
      assetRoot / "spot.svg",
      assetRoot / "area.svg",
  };
}

RasterizedLightGizmoIcon rasterizeLightGizmoSvg(std::string_view svg,
                                                uint32_t size) {
  if (size == 0u) {
    throw std::runtime_error("light gizmo SVG raster size is zero");
  }

  RasterizedLightGizmoIcon icon{};
  icon.width = size;
  icon.height = size;
  icon.rgba.assign(static_cast<size_t>(size) * size * 4u, std::byte{0});

  const SvgViewBox viewBox = parseViewBox(svg);
  size_t cursor = 0u;
  while (true) {
    const size_t open = svg.find('<', cursor);
    if (open == std::string_view::npos) {
      break;
    }
    const size_t close = svg.find('>', open);
    if (close == std::string_view::npos) {
      break;
    }
    cursor = close + 1u;
    std::string tag = trimAscii(svg.substr(open + 1u, close - open - 1u));
    if (tag.empty() || tag[0] == '/' || tag[0] == '!' || tag[0] == '?') {
      continue;
    }

    if (startsWith(tag, "circle")) {
      drawCircle(icon, viewBox, tag);
    } else if (startsWith(tag, "rect")) {
      drawRect(icon, viewBox, tag);
    } else if (startsWith(tag, "line")) {
      drawLine(icon, viewBox, tag);
    } else if (startsWith(tag, "polygon")) {
      drawPolygon(icon, viewBox, tag);
    }
  }

  return icon;
}

std::vector<std::byte> buildLightGizmoIconAtlasRgba(
    std::span<const RasterizedLightGizmoIcon, kLightGizmoIconLayerCount> icons) {
  const uint32_t width = icons[0].width;
  const uint32_t height = icons[0].height;
  if (width == 0u || height == 0u) {
    throw std::runtime_error("light gizmo icon dimensions are invalid");
  }

  const size_t layerBytes = static_cast<size_t>(width) * height * 4u;
  std::vector<std::byte> atlas;
  atlas.reserve(layerBytes * kLightGizmoIconLayerCount);
  for (const RasterizedLightGizmoIcon &icon : icons) {
    if (icon.width != width || icon.height != height ||
        icon.rgba.size() < layerBytes) {
      throw std::runtime_error("light gizmo icon atlas layers are inconsistent");
    }
    atlas.insert(atlas.end(), icon.rgba.begin(),
                 icon.rgba.begin() + static_cast<std::ptrdiff_t>(layerBytes));
  }
  return atlas;
}

std::vector<std::byte> loadLightGizmoIconAtlasRgba(
    const std::filesystem::path &assetRoot, uint32_t size) {
  const auto paths = lightGizmoIconAssetPaths(assetRoot);
  std::array<RasterizedLightGizmoIcon, kLightGizmoIconLayerCount> icons{};
  for (uint32_t layer = 0u; layer < kLightGizmoIconLayerCount; ++layer) {
    icons[layer] = rasterizeLightGizmoSvg(readTextFile(paths[layer]), size);
  }
  return buildLightGizmoIconAtlasRgba(icons);
}

} // namespace container::renderer
