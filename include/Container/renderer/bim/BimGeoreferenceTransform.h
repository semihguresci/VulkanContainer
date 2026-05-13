#pragma once

#include <string>

#include <glm/vec3.hpp>

namespace container::renderer {

struct BimGeoreferenceMetadata {
  bool hasMetersPerUnit{false};
  double metersPerUnit{1.0};
  bool hasEffectiveImportScale{false};
  double effectiveImportScale{1.0};
  bool hasProjectOrigin{false};
  glm::dvec3 projectOrigin{0.0};
  bool hasSourceUpAxis{false};
  std::string sourceUpAxis{};
  bool hasRebaseOffset{false};
  glm::dvec3 rebaseOffset{0.0};
  bool hasSurveyOffset{false};
  glm::dvec3 surveyOffset{0.0};
  std::string crsAuthority{};
  std::string crsCode{};
  std::string crsName{};
  std::string mapConversionLabel{};
};

struct BimCoordinateReadout {
  glm::dvec3 rendererWorld{0.0};
  glm::dvec3 projectCoordinates{0.0};
  glm::dvec3 surveyCoordinates{0.0};
  std::string crsLabel{};
  bool hasSurveyCoordinates{false};
};

struct BimOriginRebaseOptions {
  double largeCoordinateThreshold{100000.0};
  double largeSurveyOffsetThreshold{100000.0};
};

struct BimOriginRebaseRecommendation {
  bool recommended{false};
  bool largeProjectCoordinates{false};
  bool largeSurveyOffset{false};
  double largestProjectCoordinateMagnitude{0.0};
  double surveyOffsetMagnitude{0.0};
};

[[nodiscard]] BimCoordinateReadout buildBimCoordinateReadout(
    glm::dvec3 rendererWorld, const BimGeoreferenceMetadata& metadata);

[[nodiscard]] BimOriginRebaseRecommendation recommendBimOriginRebase(
    glm::dvec3 rendererWorld, const BimGeoreferenceMetadata& metadata,
    const BimOriginRebaseOptions& options = {});

} // namespace container::renderer
