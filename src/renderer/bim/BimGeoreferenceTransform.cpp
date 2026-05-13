#include "Container/renderer/bim/BimGeoreferenceTransform.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace container::renderer {
namespace {

[[nodiscard]] bool finiteVec3(const glm::dvec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

[[nodiscard]] bool finitePositive(double value) {
  return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] double worldUnitsPerProjectUnit(
    const BimGeoreferenceMetadata& metadata) {
  if (metadata.hasEffectiveImportScale &&
      finitePositive(metadata.effectiveImportScale)) {
    return metadata.effectiveImportScale;
  }
  if (metadata.hasMetersPerUnit && finitePositive(metadata.metersPerUnit)) {
    return metadata.metersPerUnit;
  }
  return 1.0;
}

[[nodiscard]] double maxAbsComponent(const glm::dvec3& value) {
  return std::max({std::abs(value.x), std::abs(value.y), std::abs(value.z)});
}

[[nodiscard]] bool exceedsThreshold(double value, double threshold) {
  return std::isfinite(value) && std::isfinite(threshold) && threshold > 0.0 &&
         value > threshold;
}

[[nodiscard]] std::string normalizedAxisName(std::string axis) {
  for (char& c : axis) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return axis;
}

[[nodiscard]] glm::dvec3 sourceAxisCoordinates(
    const glm::dvec3& rendererWorld, const BimGeoreferenceMetadata& metadata) {
  if (!metadata.hasSourceUpAxis ||
      normalizedAxisName(metadata.sourceUpAxis) != "Z") {
    return rendererWorld;
  }
  return {rendererWorld.x, -rendererWorld.z, rendererWorld.y};
}

[[nodiscard]] std::string crsAuthorityCode(
    const BimGeoreferenceMetadata& metadata) {
  if (!metadata.crsAuthority.empty() && !metadata.crsCode.empty()) {
    return metadata.crsAuthority + ":" + metadata.crsCode;
  }
  if (!metadata.crsAuthority.empty()) {
    return metadata.crsAuthority;
  }
  return metadata.crsCode;
}

[[nodiscard]] std::string crsLabel(const BimGeoreferenceMetadata& metadata) {
  const std::string authorityCode = crsAuthorityCode(metadata);
  std::string label = metadata.crsName;
  if (!authorityCode.empty()) {
    if (label.empty()) {
      label = authorityCode;
    } else {
      label += " (" + authorityCode + ")";
    }
  }
  if (!metadata.mapConversionLabel.empty()) {
    if (!label.empty()) {
      label += " - ";
    }
    label += metadata.mapConversionLabel;
  }
  return label;
}

} // namespace

BimCoordinateReadout buildBimCoordinateReadout(
    glm::dvec3 rendererWorld, const BimGeoreferenceMetadata& metadata) {
  BimCoordinateReadout readout{};
  readout.rendererWorld = rendererWorld;
  readout.projectCoordinates =
      sourceAxisCoordinates(rendererWorld, metadata) /
      worldUnitsPerProjectUnit(metadata);

  if (metadata.hasProjectOrigin && finiteVec3(metadata.projectOrigin)) {
    readout.projectCoordinates += metadata.projectOrigin;
  }
  if (metadata.hasRebaseOffset && finiteVec3(metadata.rebaseOffset)) {
    readout.projectCoordinates += metadata.rebaseOffset;
  }

  readout.surveyCoordinates = readout.projectCoordinates;
  if (metadata.hasSurveyOffset && finiteVec3(metadata.surveyOffset)) {
    readout.surveyCoordinates += metadata.surveyOffset;
    readout.hasSurveyCoordinates = true;
  }
  readout.crsLabel = crsLabel(metadata);
  return readout;
}

BimOriginRebaseRecommendation recommendBimOriginRebase(
    glm::dvec3 rendererWorld, const BimGeoreferenceMetadata& metadata,
    const BimOriginRebaseOptions& options) {
  const BimCoordinateReadout readout =
      buildBimCoordinateReadout(rendererWorld, metadata);

  BimOriginRebaseRecommendation recommendation{};
  recommendation.largestProjectCoordinateMagnitude =
      maxAbsComponent(readout.projectCoordinates);
  if (metadata.hasSurveyOffset && finiteVec3(metadata.surveyOffset)) {
    recommendation.surveyOffsetMagnitude = maxAbsComponent(metadata.surveyOffset);
  }
  recommendation.largeProjectCoordinates = exceedsThreshold(
      recommendation.largestProjectCoordinateMagnitude,
      options.largeCoordinateThreshold);
  recommendation.largeSurveyOffset = exceedsThreshold(
      recommendation.surveyOffsetMagnitude, options.largeSurveyOffsetThreshold);
  recommendation.recommended = recommendation.largeProjectCoordinates ||
                               recommendation.largeSurveyOffset;
  return recommendation;
}

} // namespace container::renderer
