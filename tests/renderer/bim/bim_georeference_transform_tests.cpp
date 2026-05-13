#include "Container/renderer/bim/BimGeoreferenceTransform.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::BimGeoreferenceMetadata;
using container::renderer::BimOriginRebaseOptions;
using container::renderer::buildBimCoordinateReadout;
using container::renderer::recommendBimOriginRebase;

TEST(BimGeoreferenceTransformTests,
     ConvertsWorldToProjectCoordinatesWithUnits) {
  BimGeoreferenceMetadata metadata{};
  metadata.hasMetersPerUnit = true;
  metadata.metersPerUnit = 0.001;
  metadata.crsName = "GDA2020 / MGA zone 56";
  metadata.crsAuthority = "EPSG";
  metadata.crsCode = "7856";
  metadata.mapConversionLabel = "Site grid";

  const auto readout =
      buildBimCoordinateReadout(glm::dvec3{2.0, -3.5, 0.125}, metadata);

  EXPECT_DOUBLE_EQ(readout.rendererWorld.x, 2.0);
  EXPECT_DOUBLE_EQ(readout.rendererWorld.y, -3.5);
  EXPECT_DOUBLE_EQ(readout.rendererWorld.z, 0.125);
  EXPECT_NEAR(readout.projectCoordinates.x, 2000.0, 1.0e-9);
  EXPECT_NEAR(readout.projectCoordinates.y, -3500.0, 1.0e-9);
  EXPECT_NEAR(readout.projectCoordinates.z, 125.0, 1.0e-9);
  EXPECT_FALSE(readout.hasSurveyCoordinates);
  EXPECT_EQ(readout.crsLabel, "GDA2020 / MGA zone 56 (EPSG:7856) - Site grid");
}

TEST(BimGeoreferenceTransformTests, AppliesSurveyOffsetWhenMetadataExists) {
  BimGeoreferenceMetadata metadata{};
  metadata.hasEffectiveImportScale = true;
  metadata.effectiveImportScale = 0.5;
  metadata.hasProjectOrigin = true;
  metadata.projectOrigin = glm::dvec3{1000.0, 2000.0, 10.0};
  metadata.hasSurveyOffset = true;
  metadata.surveyOffset = glm::dvec3{500000.0, 4000000.0, 100.0};

  const auto readout =
      buildBimCoordinateReadout(glm::dvec3{10.0, 20.0, 2.0}, metadata);

  EXPECT_NEAR(readout.projectCoordinates.x, 1020.0, 1.0e-9);
  EXPECT_NEAR(readout.projectCoordinates.y, 2040.0, 1.0e-9);
  EXPECT_NEAR(readout.projectCoordinates.z, 14.0, 1.0e-9);
  ASSERT_TRUE(readout.hasSurveyCoordinates);
  EXPECT_NEAR(readout.surveyCoordinates.x, 501020.0, 1.0e-9);
  EXPECT_NEAR(readout.surveyCoordinates.y, 4002040.0, 1.0e-9);
  EXPECT_NEAR(readout.surveyCoordinates.z, 114.0, 1.0e-9);
}

TEST(BimGeoreferenceTransformTests,
     AppliesSourceUpAxisBeforeScaleAndProjectOffsets) {
  BimGeoreferenceMetadata metadata{};
  metadata.hasEffectiveImportScale = true;
  metadata.effectiveImportScale = 2.0;
  metadata.hasSourceUpAxis = true;
  metadata.sourceUpAxis = "Z";
  metadata.hasRebaseOffset = true;
  metadata.rebaseOffset = glm::dvec3{1000.0, 2000.0, 3000.0};

  const auto readout =
      buildBimCoordinateReadout(glm::dvec3{4.0, 8.0, -6.0}, metadata);

  EXPECT_NEAR(readout.projectCoordinates.x, 1002.0, 1.0e-9);
  EXPECT_NEAR(readout.projectCoordinates.y, 2003.0, 1.0e-9);
  EXPECT_NEAR(readout.projectCoordinates.z, 3004.0, 1.0e-9);
}

TEST(BimGeoreferenceTransformTests,
     RecommendsOriginRebaseForLargeCoordinates) {
  BimGeoreferenceMetadata metadata{};
  metadata.hasProjectOrigin = true;
  metadata.projectOrigin = glm::dvec3{700000.0, 4500000.0, 0.0};
  metadata.hasSurveyOffset = true;
  metadata.surveyOffset = glm::dvec3{250000.0, 0.0, 0.0};

  const auto recommendation = recommendBimOriginRebase(
      glm::dvec3{25.0, 50.0, 0.0},
      metadata,
      BimOriginRebaseOptions{.largeCoordinateThreshold = 100000.0,
                             .largeSurveyOffsetThreshold = 100000.0});

  EXPECT_TRUE(recommendation.recommended);
  EXPECT_TRUE(recommendation.largeProjectCoordinates);
  EXPECT_TRUE(recommendation.largeSurveyOffset);
  EXPECT_NEAR(recommendation.largestProjectCoordinateMagnitude, 4500050.0,
              1.0e-9);
  EXPECT_NEAR(recommendation.surveyOffsetMagnitude, 250000.0, 1.0e-9);
}

} // namespace
