#include "Container/renderer/bim/BimDrawingExport.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>

namespace container::renderer {
namespace {

[[nodiscard]] std::string EscapeXml(std::string_view value) {
  std::string escaped{};
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
    case '&':
      escaped += "&amp;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    case '"':
      escaped += "&quot;";
      break;
    case '\'':
      escaped += "&apos;";
      break;
    default:
      escaped.push_back(ch);
      break;
    }
  }
  return escaped;
}

[[nodiscard]] float ProjectX(float modelX,
                             const BimDrawingExportRequest &request) {
  const double center = static_cast<double>(request.paperWidthMm) * 0.5;
  const double offset =
      static_cast<double>(std::isfinite(modelX) ? modelX : 0.0f) /
      static_cast<double>(request.modelUnitsPerPaperMm);
  const double projected = center + offset;
  if (!std::isfinite(projected) ||
      projected > static_cast<double>(std::numeric_limits<float>::max()) ||
      projected < static_cast<double>(std::numeric_limits<float>::lowest())) {
    return static_cast<float>(center);
  }
  return static_cast<float>(projected);
}

[[nodiscard]] float ProjectY(float modelZ,
                             const BimDrawingExportRequest &request) {
  const double center = static_cast<double>(request.paperHeightMm) * 0.5;
  const double offset =
      static_cast<double>(std::isfinite(modelZ) ? modelZ : 0.0f) /
      static_cast<double>(request.modelUnitsPerPaperMm);
  const double projected = center - offset;
  if (!std::isfinite(projected) ||
      projected > static_cast<double>(std::numeric_limits<float>::max()) ||
      projected < static_cast<double>(std::numeric_limits<float>::lowest())) {
    return static_cast<float>(center);
  }
  return static_cast<float>(projected);
}

[[nodiscard]] int ColorByte(float component) {
  if (!std::isfinite(component)) {
    return 0;
  }
  const float clamped = std::clamp(component, 0.0f, 1.0f);
  return static_cast<int>(std::lround(clamped * 255.0f));
}

} // namespace

std::string ExportBimDrawingSvg(const BimDrawingExportRequest &request) {
  if (request.paperWidthMm <= 0.0f || request.paperHeightMm <= 0.0f ||
      request.modelUnitsPerPaperMm <= 0.0f ||
      !std::isfinite(request.paperWidthMm) ||
      !std::isfinite(request.paperHeightMm) ||
      !std::isfinite(request.modelUnitsPerPaperMm)) {
    return {};
  }

  std::ostringstream svg{};
  svg << std::fixed << std::setprecision(3);
  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\""
      << request.paperWidthMm << "mm\" height=\"" << request.paperHeightMm
      << "mm\" viewBox=\"0 0 " << request.paperWidthMm << ' '
      << request.paperHeightMm << "\">\n";
  svg << "  <title>" << EscapeXml(request.title) << "</title>\n";
  svg << "  <desc>View: " << EscapeXml(request.viewName)
      << "; model units per paper mm: " << request.modelUnitsPerPaperMm
      << "</desc>\n";

  for (const BimDrawingExportLine &line : request.lines) {
    const float lineWidthMm =
        std::max(std::isfinite(line.lineWidthMm) ? line.lineWidthMm : 0.0f,
                 0.05f);
    svg << "  <line x1=\"" << ProjectX(line.a.x, request) << "\" y1=\""
        << ProjectY(line.a.z, request) << "\" x2=\""
        << ProjectX(line.b.x, request) << "\" y2=\""
        << ProjectY(line.b.z, request) << "\" stroke=\"rgb("
        << ColorByte(line.color.r) << ',' << ColorByte(line.color.g) << ','
        << ColorByte(line.color.b) << ")\" stroke-width=\"" << lineWidthMm
        << "mm\" fill=\"none\" data-layer=\"" << EscapeXml(line.layer)
        << "\" />\n";
  }

  svg << "</svg>\n";
  return svg.str();
}

} // namespace container::renderer
