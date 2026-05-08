#include "Container/renderer/shadow/ShadowCascadePreparationPlanner.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::ShadowCascadePreparationPlanInputs;
using container::renderer::buildShadowCascadePreparationPlan;

ShadowCascadePreparationPlanInputs readyInputs() {
  ShadowCascadePreparationPlanInputs inputs{};
  inputs.shadowAtlasVisible = true;
  inputs.cascades[0].shadowPassActive = true;
  inputs.cascades[0].shadowPassRecordable = true;
  return inputs;
}

} // namespace

TEST(ShadowCascadePreparationPlannerTests,
     HiddenShadowAtlasSuppressesPreparation) {
  auto inputs = readyInputs();
  inputs.shadowAtlasVisible = false;
  inputs.hasSceneWindingFlippedDraws = true;

  EXPECT_FALSE(buildShadowCascadePreparationPlan(inputs).prepareDrawCommands);
}

TEST(ShadowCascadePreparationPlannerTests,
     InactiveOrUnrecordableCascadesAreIgnored) {
  auto inactive = readyInputs();
  inactive.cascades[0].shadowPassActive = false;
  inactive.hasSceneWindingFlippedDraws = true;

  auto unrecordable = readyInputs();
  unrecordable.cascades[0].shadowPassRecordable = false;
  unrecordable.hasBimShadowGeometry = true;

  EXPECT_FALSE(
      buildShadowCascadePreparationPlan(inactive).prepareDrawCommands);
  EXPECT_FALSE(
      buildShadowCascadePreparationPlan(unrecordable).prepareDrawCommands);
}

TEST(ShadowCascadePreparationPlannerTests,
     WindingFlippedOrDoubleSidedSceneDrawsNeedPreparation) {
  auto windingFlipped = readyInputs();
  windingFlipped.hasSceneWindingFlippedDraws = true;

  auto doubleSided = readyInputs();
  doubleSided.hasSceneDoubleSidedDraws = true;

  EXPECT_TRUE(
      buildShadowCascadePreparationPlan(windingFlipped).prepareDrawCommands);
  EXPECT_TRUE(
      buildShadowCascadePreparationPlan(doubleSided).prepareDrawCommands);
}

TEST(ShadowCascadePreparationPlannerTests,
     SingleSidedSceneDrawsNeedPreparationOnlyWithoutGpuCull) {
  auto cpuFallback = readyInputs();
  cpuFallback.hasSceneSingleSidedDraws = true;

  auto gpuCull = readyInputs();
  gpuCull.hasSceneSingleSidedDraws = true;
  gpuCull.cascades[0].sceneSingleSidedUsesGpuCull = true;

  EXPECT_TRUE(
      buildShadowCascadePreparationPlan(cpuFallback).prepareDrawCommands);
  EXPECT_FALSE(buildShadowCascadePreparationPlan(gpuCull).prepareDrawCommands);
}

TEST(ShadowCascadePreparationPlannerTests,
     BimShadowGeometryNeedsPreparationForRecordableCascade) {
  auto inputs = readyInputs();
  inputs.hasBimShadowGeometry = true;
  inputs.cascades[0].sceneSingleSidedUsesGpuCull = true;

  EXPECT_TRUE(buildShadowCascadePreparationPlan(inputs).prepareDrawCommands);
}
