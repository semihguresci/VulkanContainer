#include "Container/renderer/BimManager.h"
#include "Container/renderer/bim/BimFrameDrawRoutingPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::BimDrawLists;
using container::renderer::BimFrameDrawRoutingInputs;
using container::renderer::BimFrameDrawSource;
using container::renderer::BimFrameGpuVisibilityInputs;
using container::renderer::BimFrameMeshDrawLists;
using container::renderer::BimGeometryDrawLists;
using container::renderer::DrawCommand;
using container::renderer::bimFrameGpuVisibilityAvailable;
using container::renderer::buildBimFrameDrawRoutingPlan;
using container::renderer::hasBimFrameGeometryDrawCommands;
using container::renderer::hasBimFrameMeshDrawCommands;

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t objectIndex) {
  return {DrawCommand{.objectIndex = objectIndex,
                      .firstIndex = objectIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

void addOpaque(BimGeometryDrawLists &draws, uint32_t objectIndex) {
  draws.opaqueDrawCommands = drawCommands(objectIndex);
}

void addTransparent(BimGeometryDrawLists &draws, uint32_t objectIndex) {
  draws.transparentDrawCommands = drawCommands(objectIndex);
}

void addMesh(BimDrawLists &draws, uint32_t objectIndex) {
  draws.opaqueDrawCommands = drawCommands(objectIndex);
}

[[nodiscard]] BimFrameMeshDrawLists meshDrawsFrom(BimDrawLists &draws) {
  return {.opaqueDrawCommands = &draws.opaqueDrawCommands,
          .opaqueSingleSidedDrawCommands = &draws.opaqueSingleSidedDrawCommands,
          .opaqueWindingFlippedDrawCommands =
              &draws.opaqueWindingFlippedDrawCommands,
          .opaqueDoubleSidedDrawCommands =
              &draws.opaqueDoubleSidedDrawCommands,
          .transparentDrawCommands = &draws.transparentDrawCommands,
          .transparentSingleSidedDrawCommands =
              &draws.transparentSingleSidedDrawCommands,
          .transparentWindingFlippedDrawCommands =
              &draws.transparentWindingFlippedDrawCommands,
          .transparentDoubleSidedDrawCommands =
              &draws.transparentDoubleSidedDrawCommands};
}

[[nodiscard]] BimFrameGpuVisibilityInputs gpuVisibilityReady() {
  return {.filterActive = true,
          .gpuResident = true,
          .computeReady = true,
          .objectCount = 8u,
          .visibilityMaskReady = true};
}

[[nodiscard]] BimFrameDrawRoutingInputs routingInputs(BimDrawLists &unfiltered) {
  return {.gpuVisibility = {},
          .pointCloudVisible = true,
          .curvesVisible = true,
          .unfilteredMeshDraws = meshDrawsFrom(unfiltered),
          .unfilteredPointDraws = &unfiltered.points,
          .unfilteredCurveDraws = &unfiltered.curves,
          .unfilteredNativePointDraws = &unfiltered.nativePoints,
          .unfilteredNativeCurveDraws = &unfiltered.nativeCurves};
}

TEST(BimFrameDrawRoutingPlannerTests, GpuVisibilityRequiresCompleteState) {
  EXPECT_FALSE(bimFrameGpuVisibilityAvailable({}));
  EXPECT_FALSE(bimFrameGpuVisibilityAvailable(
      {.filterActive = true,
       .gpuResident = true,
       .computeReady = true,
       .objectCount = 1u,
       .visibilityMaskReady = false}));
  EXPECT_FALSE(bimFrameGpuVisibilityAvailable(
      {.filterActive = true,
       .gpuResident = true,
       .computeReady = false,
       .objectCount = 1u,
       .visibilityMaskReady = true}));
  EXPECT_FALSE(bimFrameGpuVisibilityAvailable(
      {.filterActive = true,
       .gpuResident = true,
       .computeReady = true,
       .objectCount = 0u,
       .visibilityMaskReady = true}));
  EXPECT_TRUE(bimFrameGpuVisibilityAvailable(gpuVisibilityReady()));
}

TEST(BimFrameDrawRoutingPlannerTests,
     MeshesRequestCpuFilteringWhenGpuVisibilityIsUnavailable) {
  BimDrawLists unfiltered{};
  BimDrawLists filtered{};
  addMesh(unfiltered, 1u);
  addMesh(filtered, 2u);

  auto inputs = routingInputs(unfiltered);
  inputs.gpuVisibility.filterActive = true;

  auto plan = buildBimFrameDrawRoutingPlan(inputs);
  EXPECT_TRUE(plan.cpuFilteredDrawsRequired);
  EXPECT_EQ(plan.meshDrawSource, BimFrameDrawSource::Unfiltered);
  EXPECT_EQ(plan.meshDraws.opaqueDrawCommands, &unfiltered.opaqueDrawCommands);

  inputs.cpuFilteredDraws = &filtered;
  plan = buildBimFrameDrawRoutingPlan(inputs);

  EXPECT_TRUE(plan.cpuFilteredDrawsRequired);
  EXPECT_FALSE(plan.meshDrawsUseGpuVisibility);
  EXPECT_EQ(plan.meshDrawSource, BimFrameDrawSource::CpuFiltered);
  EXPECT_EQ(plan.meshDraws.opaqueDrawCommands, &filtered.opaqueDrawCommands);
}

TEST(BimFrameDrawRoutingPlannerTests,
     GpuVisibilityKeepsMeshSourceUnfilteredForMaskedGpuDraws) {
  BimDrawLists unfiltered{};
  BimDrawLists filtered{};
  addMesh(unfiltered, 3u);
  addMesh(filtered, 4u);

  auto inputs = routingInputs(unfiltered);
  inputs.gpuVisibility = gpuVisibilityReady();
  inputs.cpuFilteredDraws = &filtered;

  const auto plan = buildBimFrameDrawRoutingPlan(inputs);

  EXPECT_FALSE(plan.cpuFilteredDrawsRequired);
  EXPECT_TRUE(plan.meshDrawsUseGpuVisibility);
  EXPECT_TRUE(plan.transparentMeshDrawsUseGpuVisibility);
  EXPECT_EQ(plan.meshDrawSource, BimFrameDrawSource::Unfiltered);
  EXPECT_EQ(plan.meshDraws.opaqueDrawCommands, &unfiltered.opaqueDrawCommands);
}

TEST(BimFrameDrawRoutingPlannerTests,
     PlaceholderPointAndCurveDrawsUseCpuFilteredSourceWhenPresent) {
  BimDrawLists unfiltered{};
  BimDrawLists filtered{};
  addOpaque(unfiltered.points, 5u);
  addTransparent(unfiltered.curves, 6u);
  addOpaque(filtered.points, 7u);
  addTransparent(filtered.curves, 8u);

  auto inputs = routingInputs(unfiltered);
  inputs.gpuVisibility.filterActive = true;
  inputs.cpuFilteredDraws = &filtered;

  const auto plan = buildBimFrameDrawRoutingPlan(inputs);

  EXPECT_TRUE(plan.cpuFilteredDrawsRequired);
  EXPECT_EQ(plan.pointPlaceholderDrawSource, BimFrameDrawSource::CpuFiltered);
  EXPECT_EQ(plan.curvePlaceholderDrawSource, BimFrameDrawSource::CpuFiltered);
  EXPECT_EQ(plan.pointPlaceholderDraws, &filtered.points);
  EXPECT_EQ(plan.curvePlaceholderDraws, &filtered.curves);
}

TEST(BimFrameDrawRoutingPlannerTests,
     NativePrimitiveRoutingPrefersGpuThenCpuThenUnfiltered) {
  BimDrawLists unfiltered{};
  BimDrawLists filtered{};
  addOpaque(unfiltered.nativePoints, 9u);
  addOpaque(unfiltered.nativeCurves, 10u);
  addOpaque(filtered.nativePoints, 11u);
  addOpaque(filtered.nativeCurves, 12u);

  auto inputs = routingInputs(unfiltered);
  inputs.gpuVisibility = gpuVisibilityReady();
  inputs.cpuFilteredDraws = &filtered;

  auto plan = buildBimFrameDrawRoutingPlan(inputs);
  EXPECT_TRUE(plan.nativePrimitiveDrawsUseGpuVisibility);
  EXPECT_EQ(plan.nativePointDrawSource, BimFrameDrawSource::GpuFiltered);
  EXPECT_EQ(plan.nativeCurveDrawSource, BimFrameDrawSource::GpuFiltered);
  EXPECT_EQ(plan.nativePointDraws, &unfiltered.nativePoints);
  EXPECT_EQ(plan.nativeCurveDraws, &unfiltered.nativeCurves);
  EXPECT_TRUE(plan.pointPrimitivePassEnabled);
  EXPECT_TRUE(plan.curvePrimitivePassEnabled);

  inputs.gpuVisibility.computeReady = false;
  plan = buildBimFrameDrawRoutingPlan(inputs);
  EXPECT_EQ(plan.nativePointDrawSource, BimFrameDrawSource::CpuFiltered);
  EXPECT_EQ(plan.nativeCurveDrawSource, BimFrameDrawSource::CpuFiltered);
  EXPECT_EQ(plan.nativePointDraws, &filtered.nativePoints);
  EXPECT_EQ(plan.nativeCurveDraws, &filtered.nativeCurves);

  inputs.cpuFilteredDraws = nullptr;
  plan = buildBimFrameDrawRoutingPlan(inputs);
  EXPECT_EQ(plan.nativePointDrawSource, BimFrameDrawSource::Unfiltered);
  EXPECT_EQ(plan.nativeCurveDrawSource, BimFrameDrawSource::Unfiltered);
  EXPECT_EQ(plan.nativePointDraws, &unfiltered.nativePoints);
  EXPECT_EQ(plan.nativeCurveDraws, &unfiltered.nativeCurves);
}

TEST(BimFrameDrawRoutingPlannerTests,
     PrimitivePassEnablementFollowsChosenNativeSource) {
  BimDrawLists unfiltered{};
  BimDrawLists filtered{};
  addOpaque(unfiltered.nativePoints, 13u);

  auto inputs = routingInputs(unfiltered);
  inputs.gpuVisibility.filterActive = true;
  inputs.cpuFilteredDraws = &filtered;

  const auto plan = buildBimFrameDrawRoutingPlan(inputs);

  EXPECT_TRUE(plan.cpuFilteredDrawsRequired);
  EXPECT_EQ(plan.nativePointDrawSource, BimFrameDrawSource::CpuFiltered);
  EXPECT_EQ(plan.nativePointDraws, &filtered.nativePoints);
  EXPECT_FALSE(plan.pointPrimitivePassEnabled);
}

TEST(BimFrameDrawRoutingPlannerTests,
     HiddenPrimitiveLayersDoNotRequestFilteringOrDrawPasses) {
  BimDrawLists unfiltered{};
  addOpaque(unfiltered.points, 14u);
  addOpaque(unfiltered.nativePoints, 15u);
  addOpaque(unfiltered.curves, 16u);
  addOpaque(unfiltered.nativeCurves, 17u);

  auto inputs = routingInputs(unfiltered);
  inputs.gpuVisibility.filterActive = true;
  inputs.pointCloudVisible = false;
  inputs.curvesVisible = false;

  const auto plan = buildBimFrameDrawRoutingPlan(inputs);

  EXPECT_FALSE(plan.cpuFilteredDrawsRequired);
  EXPECT_EQ(plan.pointPlaceholderDraws, nullptr);
  EXPECT_EQ(plan.curvePlaceholderDraws, nullptr);
  EXPECT_EQ(plan.nativePointDraws, nullptr);
  EXPECT_EQ(plan.nativeCurveDraws, nullptr);
  EXPECT_FALSE(plan.pointPrimitivePassEnabled);
  EXPECT_FALSE(plan.curvePrimitivePassEnabled);
}

TEST(BimFrameDrawRoutingPlannerTests, DetectsGeometryAndMeshDrawCommands) {
  BimDrawLists draws{};
  EXPECT_FALSE(hasBimFrameGeometryDrawCommands(&draws.points));
  EXPECT_FALSE(hasBimFrameMeshDrawCommands(meshDrawsFrom(draws)));

  addTransparent(draws.points, 18u);
  addMesh(draws, 19u);

  EXPECT_TRUE(hasBimFrameGeometryDrawCommands(&draws.points));
  EXPECT_TRUE(hasBimFrameMeshDrawCommands(meshDrawsFrom(draws)));
  EXPECT_FALSE(hasBimFrameGeometryDrawCommands(nullptr));
}

} // namespace
