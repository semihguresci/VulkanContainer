#include "Container/renderer/deferred/DeferredRasterDebugOverlayPlanner.h"
#include "Container/renderer/scene/DrawCommand.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::DeferredDebugOverlayDrawLists;
using container::renderer::DeferredDebugOverlayInputs;
using container::renderer::DeferredDebugOverlayPipeline;
using container::renderer::DeferredDebugOverlayPipelineReadiness;
using container::renderer::DeferredDebugOverlaySource;
using container::renderer::DeferredDebugOverlaySourceInput;
using container::renderer::DrawCommand;
using container::renderer::
    kDeferredDebugOverlayNormalValidationBothSidesValid;
using container::renderer::
    kDeferredDebugOverlayNormalValidationInvertFaceClassification;
using container::renderer::buildDeferredDebugOverlayPlan;

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t firstIndex) {
  return {DrawCommand{.objectIndex = firstIndex,
                      .firstIndex = firstIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

[[nodiscard]] DeferredDebugOverlayPipelineReadiness readyPipelines() {
  return {.wireframeDepth = true,
          .wireframeNoDepth = true,
          .wireframeDepthFrontCull = true,
          .wireframeNoDepthFrontCull = true,
          .objectNormalDebug = true,
          .objectNormalDebugFrontCull = true,
          .objectNormalDebugNoCull = true,
          .geometryDebug = true,
          .normalValidation = true,
          .normalValidationFrontCull = true,
          .normalValidationNoCull = true,
          .surfaceNormalLine = true};
}

[[nodiscard]] DeferredDebugOverlayInputs readyInputs() {
  DeferredDebugOverlayInputs inputs{};
  inputs.pipelines = readyPipelines();
  inputs.bindlessPushConstantsReady = true;
  inputs.wireframePushConstantsReady = true;
  inputs.normalValidationPushConstantsReady = true;
  inputs.surfaceNormalPushConstantsReady = true;
  return inputs;
}

TEST(DeferredRasterDebugOverlayPlannerTests,
     WireframeFullWinsOverObjectNormalsAndPreservesSplitOrder) {
  const auto opaqueSingle = drawCommands(1u);
  const auto transparentSingle = drawCommands(2u);
  const auto opaqueWinding = drawCommands(3u);
  const auto transparentWinding = drawCommands(4u);
  const auto opaqueDouble = drawCommands(5u);
  const auto transparentDouble = drawCommands(6u);
  auto inputs = readyInputs();
  inputs.frameState.wireframeFullMode = true;
  inputs.frameState.objectSpaceNormalsEnabled = true;
  inputs.frameState.wireframeDepthTest = true;
  inputs.frameState.wireframeLineWidth = 2.5f;
  inputs.sources[0] = {
      .source = DeferredDebugOverlaySource::Scene,
      .geometryReady = true,
      .drawDiagnosticCube = true,
      .diagnosticCubeObjectIndex = 42u,
      .draws = {.opaqueSingleSidedDrawCommands = &opaqueSingle,
                .transparentSingleSidedDrawCommands = &transparentSingle,
                .opaqueWindingFlippedDrawCommands = &opaqueWinding,
                .transparentWindingFlippedDrawCommands = &transparentWinding,
                .opaqueDoubleSidedDrawCommands = &opaqueDouble,
                .transparentDoubleSidedDrawCommands = &transparentDouble}};
  inputs.sourceCount = 1u;

  const auto plan = buildDeferredDebugOverlayPlan(inputs);

  ASSERT_EQ(plan.wireframeFullSourceCount, 1u);
  const auto &sourcePlan = plan.wireframeFullSources[0];
  EXPECT_EQ(sourcePlan.source, DeferredDebugOverlaySource::Scene);
  EXPECT_TRUE(sourcePlan.drawDiagnosticCube);
  EXPECT_EQ(sourcePlan.diagnosticCubePipeline,
            DeferredDebugOverlayPipeline::WireframeDepth);
  EXPECT_EQ(sourcePlan.diagnosticCubeObjectIndex, 42u);
  ASSERT_EQ(sourcePlan.routeCount, 6u);
  EXPECT_EQ(sourcePlan.routes[0].commands, &opaqueSingle);
  EXPECT_EQ(sourcePlan.routes[0].pipeline,
            DeferredDebugOverlayPipeline::WireframeDepth);
  EXPECT_FLOAT_EQ(sourcePlan.routes[0].drawLineWidth, 2.5f);
  EXPECT_EQ(sourcePlan.routes[1].commands, &transparentSingle);
  EXPECT_EQ(sourcePlan.routes[1].pipeline,
            DeferredDebugOverlayPipeline::WireframeDepth);
  EXPECT_EQ(sourcePlan.routes[2].commands, &opaqueWinding);
  EXPECT_EQ(sourcePlan.routes[2].pipeline,
            DeferredDebugOverlayPipeline::WireframeDepthFrontCull);
  EXPECT_EQ(sourcePlan.routes[3].commands, &transparentWinding);
  EXPECT_EQ(sourcePlan.routes[3].pipeline,
            DeferredDebugOverlayPipeline::WireframeDepthFrontCull);
  EXPECT_EQ(sourcePlan.routes[4].commands, &opaqueDouble);
  EXPECT_EQ(sourcePlan.routes[5].commands, &transparentDouble);
  EXPECT_EQ(plan.objectNormalSourceCount, 0u);
}

TEST(DeferredRasterDebugOverlayPlannerTests,
     WireframeOverlayUsesDepthModeAndFrontCullFallback) {
  const auto winding = drawCommands(7u);
  auto inputs = readyInputs();
  inputs.frameState.wireframeOverlayMode = true;
  inputs.frameState.wireframeDepthTest = false;
  inputs.pipelines.wireframeNoDepthFrontCull = false;
  inputs.sources[0] = {
      .source = DeferredDebugOverlaySource::Scene,
      .geometryReady = true,
      .draws = {.opaqueWindingFlippedDrawCommands = &winding}};
  inputs.sourceCount = 1u;

  const auto plan = buildDeferredDebugOverlayPlan(inputs);

  ASSERT_EQ(plan.wireframeOverlaySourceCount, 1u);
  ASSERT_EQ(plan.wireframeOverlaySources[0].routeCount, 1u);
  EXPECT_EQ(plan.wireframeOverlaySources[0].routes[0].commands, &winding);
  EXPECT_EQ(plan.wireframeOverlaySources[0].routes[0].pipeline,
            DeferredDebugOverlayPipeline::WireframeNoDepth);
  EXPECT_FALSE(plan.wireframeOverlaySources[0].drawDiagnosticCube);
}

TEST(DeferredRasterDebugOverlayPlannerTests,
     ObjectNormalsRouteSingleWindingDoubleCullFamilies) {
  const auto single = drawCommands(8u);
  const auto winding = drawCommands(9u);
  const auto doubleSided = drawCommands(10u);
  auto inputs = readyInputs();
  inputs.frameState.objectSpaceNormalsEnabled = true;
  inputs.sources[0] = {
      .source = DeferredDebugOverlaySource::Scene,
      .geometryReady = true,
      .drawDiagnosticCube = true,
      .diagnosticCubeObjectIndex = 24u,
      .draws = {.opaqueSingleSidedDrawCommands = &single,
                .opaqueWindingFlippedDrawCommands = &winding,
                .opaqueDoubleSidedDrawCommands = &doubleSided}};
  inputs.sourceCount = 1u;

  const auto plan = buildDeferredDebugOverlayPlan(inputs);

  ASSERT_EQ(plan.objectNormalSourceCount, 1u);
  const auto &sourcePlan = plan.objectNormalSources[0];
  ASSERT_EQ(sourcePlan.routeCount, 3u);
  EXPECT_EQ(sourcePlan.routes[0].pipeline,
            DeferredDebugOverlayPipeline::ObjectNormalDebug);
  EXPECT_EQ(sourcePlan.routes[0].commands, &single);
  EXPECT_EQ(sourcePlan.routes[1].pipeline,
            DeferredDebugOverlayPipeline::ObjectNormalDebugFrontCull);
  EXPECT_EQ(sourcePlan.routes[1].commands, &winding);
  EXPECT_EQ(sourcePlan.routes[2].pipeline,
            DeferredDebugOverlayPipeline::ObjectNormalDebugNoCull);
  EXPECT_EQ(sourcePlan.routes[2].commands, &doubleSided);
  EXPECT_TRUE(sourcePlan.drawDiagnosticCube);
  EXPECT_EQ(sourcePlan.diagnosticCubePipeline,
            DeferredDebugOverlayPipeline::ObjectNormalDebug);
}

TEST(DeferredRasterDebugOverlayPlannerTests,
     GeometryOverlayUsesAggregateOpaqueTransparentOnly) {
  const auto opaqueAggregate = drawCommands(11u);
  const auto transparentAggregate = drawCommands(12u);
  const auto splitOnly = drawCommands(13u);
  auto inputs = readyInputs();
  inputs.frameState.geometryOverlayEnabled = true;
  inputs.sources[0] = {
      .source = DeferredDebugOverlaySource::Scene,
      .geometryReady = true,
      .draws = {.opaqueDrawCommands = &opaqueAggregate,
                .transparentDrawCommands = &transparentAggregate,
                .opaqueSingleSidedDrawCommands = &splitOnly}};
  inputs.sourceCount = 1u;

  const auto plan = buildDeferredDebugOverlayPlan(inputs);

  ASSERT_EQ(plan.geometryOverlaySourceCount, 1u);
  const auto &sourcePlan = plan.geometryOverlaySources[0];
  ASSERT_EQ(sourcePlan.routeCount, 2u);
  EXPECT_EQ(sourcePlan.routes[0].commands, &opaqueAggregate);
  EXPECT_EQ(sourcePlan.routes[0].pipeline,
            DeferredDebugOverlayPipeline::GeometryDebug);
  EXPECT_EQ(sourcePlan.routes[1].commands, &transparentAggregate);
  EXPECT_EQ(sourcePlan.routes[1].pipeline,
            DeferredDebugOverlayPipeline::GeometryDebug);
}

TEST(DeferredRasterDebugOverlayPlannerTests,
     NormalValidationPairsOpaqueTransparentWithFaceFlags) {
  const auto opaqueSingle = drawCommands(14u);
  const auto transparentSingle = drawCommands(15u);
  const auto opaqueWinding = drawCommands(16u);
  const auto transparentWinding = drawCommands(17u);
  const auto opaqueDouble = drawCommands(18u);
  const auto transparentDouble = drawCommands(19u);
  auto inputs = readyInputs();
  inputs.frameState.normalValidationEnabled = true;
  inputs.sources[0] = {
      .source = DeferredDebugOverlaySource::Scene,
      .geometryReady = true,
      .draws = {.opaqueSingleSidedDrawCommands = &opaqueSingle,
                .transparentSingleSidedDrawCommands = &transparentSingle,
                .opaqueWindingFlippedDrawCommands = &opaqueWinding,
                .transparentWindingFlippedDrawCommands = &transparentWinding,
                .opaqueDoubleSidedDrawCommands = &opaqueDouble,
                .transparentDoubleSidedDrawCommands = &transparentDouble}};
  inputs.sourceCount = 1u;

  const auto plan = buildDeferredDebugOverlayPlan(inputs);

  ASSERT_EQ(plan.normalValidationSourceCount, 1u);
  const auto &sourcePlan = plan.normalValidationSources[0];
  ASSERT_EQ(sourcePlan.routeCount, 3u);
  EXPECT_EQ(sourcePlan.routes[0].opaqueCommands, &opaqueSingle);
  EXPECT_EQ(sourcePlan.routes[0].transparentCommands, &transparentSingle);
  EXPECT_EQ(sourcePlan.routes[0].pipeline,
            DeferredDebugOverlayPipeline::NormalValidation);
  EXPECT_EQ(sourcePlan.routes[0].normalValidationFaceFlags, 0u);
  EXPECT_EQ(sourcePlan.routes[1].opaqueCommands, &opaqueWinding);
  EXPECT_EQ(sourcePlan.routes[1].transparentCommands, &transparentWinding);
  EXPECT_EQ(sourcePlan.routes[1].pipeline,
            DeferredDebugOverlayPipeline::NormalValidationFrontCull);
  EXPECT_EQ(sourcePlan.routes[1].normalValidationFaceFlags,
            kDeferredDebugOverlayNormalValidationInvertFaceClassification);
  EXPECT_EQ(sourcePlan.routes[2].opaqueCommands, &opaqueDouble);
  EXPECT_EQ(sourcePlan.routes[2].transparentCommands, &transparentDouble);
  EXPECT_EQ(sourcePlan.routes[2].pipeline,
            DeferredDebugOverlayPipeline::NormalValidationNoCull);
  EXPECT_EQ(sourcePlan.routes[2].normalValidationFaceFlags,
            kDeferredDebugOverlayNormalValidationBothSidesValid);
}

TEST(DeferredRasterDebugOverlayPlannerTests,
     SurfaceNormalsUseAggregateListsAndSurfaceLineWidth) {
  const auto opaqueAggregate = drawCommands(20u);
  const auto transparentAggregate = drawCommands(21u);
  const auto splitOnly = drawCommands(22u);
  auto inputs = readyInputs();
  inputs.frameState.surfaceNormalLinesEnabled = true;
  inputs.frameState.surfaceNormalLineWidth = 4.0f;
  inputs.sources[0] = {
      .source = DeferredDebugOverlaySource::Scene,
      .geometryReady = true,
      .draws = {.opaqueDrawCommands = &opaqueAggregate,
                .transparentDrawCommands = &transparentAggregate,
                .opaqueSingleSidedDrawCommands = &splitOnly}};
  inputs.sourceCount = 1u;

  const auto plan = buildDeferredDebugOverlayPlan(inputs);

  ASSERT_EQ(plan.surfaceNormalSourceCount, 1u);
  const auto &sourcePlan = plan.surfaceNormalSources[0];
  ASSERT_EQ(sourcePlan.routeCount, 1u);
  EXPECT_EQ(sourcePlan.routes[0].pipeline,
            DeferredDebugOverlayPipeline::SurfaceNormalLine);
  EXPECT_EQ(sourcePlan.routes[0].opaqueCommands, &opaqueAggregate);
  EXPECT_EQ(sourcePlan.routes[0].transparentCommands, &transparentAggregate);
  EXPECT_FLOAT_EQ(sourcePlan.routes[0].drawLineWidth, 4.0f);
  EXPECT_FLOAT_EQ(sourcePlan.routes[0].rasterLineWidth, 4.0f);
}

TEST(DeferredRasterDebugOverlayPlannerTests,
     MissingGeometryOrPushConstantsSuppressesOnlyAffectedPlans) {
  const auto commands = drawCommands(23u);
  auto inputs = readyInputs();
  inputs.frameState.wireframeOverlayMode = true;
  inputs.frameState.geometryOverlayEnabled = true;
  inputs.frameState.normalValidationEnabled = true;
  inputs.frameState.surfaceNormalLinesEnabled = true;
  inputs.bindlessPushConstantsReady = false;
  inputs.normalValidationPushConstantsReady = false;
  inputs.sources[0] = {
      .source = DeferredDebugOverlaySource::Scene,
      .geometryReady = true,
      .draws = {.opaqueDrawCommands = &commands,
                .opaqueSingleSidedDrawCommands = &commands}};
  inputs.sources[1] = {
      .source = DeferredDebugOverlaySource::BimMesh,
      .geometryReady = false,
      .draws = {.opaqueDrawCommands = &commands,
                .opaqueSingleSidedDrawCommands = &commands}};
  inputs.sourceCount = 2u;

  const auto plan = buildDeferredDebugOverlayPlan(inputs);

  EXPECT_EQ(plan.wireframeOverlaySourceCount, 1u);
  EXPECT_EQ(plan.wireframeOverlaySources[0].source,
            DeferredDebugOverlaySource::Scene);
  EXPECT_EQ(plan.geometryOverlaySourceCount, 0u);
  EXPECT_EQ(plan.normalValidationSourceCount, 0u);
  EXPECT_EQ(plan.surfaceNormalSourceCount, 1u);
  EXPECT_EQ(plan.surfaceNormalSources[0].source,
            DeferredDebugOverlaySource::Scene);
}

TEST(DeferredRasterDebugOverlayPlannerTests,
     BimSourcesUseMeshPointPlaceholderCurvePlaceholderOnly) {
  const auto mesh = drawCommands(24u);
  const auto points = drawCommands(25u);
  const auto curves = drawCommands(26u);
  auto inputs = readyInputs();
  inputs.frameState.geometryOverlayEnabled = true;
  inputs.sources[0] = {
      .source = DeferredDebugOverlaySource::BimMesh,
      .geometryReady = true,
      .draws = {.opaqueDrawCommands = &mesh}};
  inputs.sources[1] = {
      .source = DeferredDebugOverlaySource::BimPointPlaceholders,
      .geometryReady = true,
      .draws = {.opaqueDrawCommands = &points}};
  inputs.sources[2] = {
      .source = DeferredDebugOverlaySource::BimCurvePlaceholders,
      .geometryReady = true,
      .draws = {.opaqueDrawCommands = &curves}};
  inputs.sourceCount = 3u;

  const auto plan = buildDeferredDebugOverlayPlan(inputs);

  ASSERT_EQ(plan.geometryOverlaySourceCount, 3u);
  EXPECT_EQ(plan.geometryOverlaySources[0].source,
            DeferredDebugOverlaySource::BimMesh);
  EXPECT_EQ(plan.geometryOverlaySources[0].routes[0].commands, &mesh);
  EXPECT_EQ(plan.geometryOverlaySources[1].source,
            DeferredDebugOverlaySource::BimPointPlaceholders);
  EXPECT_EQ(plan.geometryOverlaySources[1].routes[0].commands, &points);
  EXPECT_EQ(plan.geometryOverlaySources[2].source,
            DeferredDebugOverlaySource::BimCurvePlaceholders);
  EXPECT_EQ(plan.geometryOverlaySources[2].routes[0].commands, &curves);
}

} // namespace
