#pragma once

#include "Container/common/CommonMath.h"

#include <string>
#include <vector>

namespace container::renderer {

struct BimDrawingExportLine {
  glm::vec3 a{0.0f};
  glm::vec3 b{0.0f};
  glm::vec3 color{0.0f};
  float lineWidthMm{0.18f};
  std::string layer{};
};

struct BimDrawingExportRequest {
  std::string title{};
  std::string viewName{};
  float paperWidthMm{297.0f};
  float paperHeightMm{210.0f};
  float modelUnitsPerPaperMm{50.0f};
  std::vector<BimDrawingExportLine> lines{};
};

[[nodiscard]] std::string
ExportBimDrawingSvg(const BimDrawingExportRequest &request);

} // namespace container::renderer
