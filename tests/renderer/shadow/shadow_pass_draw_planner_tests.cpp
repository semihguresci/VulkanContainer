#include "Container/renderer/scene/DrawCommand.h"
#include "Container/renderer/shadow/ShadowPassDrawPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::DrawCommand;
using container::renderer::ShadowPassBimGpuSlot;
using container::renderer::ShadowPassPipeline;
using container::renderer::buildShadowPassDrawPlan;

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t firstIndex) {
  return {DrawCommand{.objectIndex = firstIndex,
                      .firstIndex = firstIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

TEST(ShadowPassDrawPlannerTests,
     SceneGpuCullSuppressesSceneCpuSingleSidedOnly) {
  const auto singleSided = drawCommands(1u);
  const auto windingFlipped = drawCommands(2u);
  const auto doubleSided = drawCommands(3u);

  const auto plan = buildShadowPassDrawPlan(
      {.sceneGeometryReady = true,
       .sceneGpuCullActive = true,
       .sceneDraws = {.singleSided = &singleSided,
                      .windingFlipped = &windingFlipped,
                      .doubleSided = &doubleSided}});

  EXPECT_TRUE(plan.sceneGpuRoute.active);
  EXPECT_EQ(plan.sceneGpuRoute.pipeline, ShadowPassPipeline::Primary);
  ASSERT_EQ(plan.sceneCpuRouteCount, 2u);
  EXPECT_EQ(plan.sceneCpuRoutes[0].pipeline, ShadowPassPipeline::FrontCull);
  EXPECT_EQ(plan.sceneCpuRoutes[0].commands, &windingFlipped);
  EXPECT_EQ(plan.sceneCpuRoutes[1].pipeline, ShadowPassPipeline::NoCull);
  EXPECT_EQ(plan.sceneCpuRoutes[1].commands, &doubleSided);
}

TEST(ShadowPassDrawPlannerTests, SceneCpuRoutesKeepStableOrder) {
  const auto singleSided = drawCommands(4u);
  const auto windingFlipped = drawCommands(5u);
  const auto doubleSided = drawCommands(6u);

  const auto plan = buildShadowPassDrawPlan(
      {.sceneGeometryReady = true,
       .sceneDraws = {.singleSided = &singleSided,
                      .windingFlipped = &windingFlipped,
                      .doubleSided = &doubleSided}});

  EXPECT_FALSE(plan.sceneGpuRoute.active);
  ASSERT_EQ(plan.sceneCpuRouteCount, 3u);
  EXPECT_EQ(plan.sceneCpuRoutes[0].pipeline, ShadowPassPipeline::Primary);
  EXPECT_EQ(plan.sceneCpuRoutes[1].pipeline, ShadowPassPipeline::FrontCull);
  EXPECT_EQ(plan.sceneCpuRoutes[2].pipeline, ShadowPassPipeline::NoCull);
}

TEST(ShadowPassDrawPlannerTests,
     BimGpuFilteredRoutesKeepSlotAndPipelineOrder) {
  const auto singleSided = drawCommands(7u);

  const auto plan = buildShadowPassDrawPlan(
      {.bimGeometryReady = true,
       .bimGpuFilteredMeshActive = true,
       .bimDraws = {.singleSided = &singleSided}});

  ASSERT_EQ(plan.bimGpuRouteCount, 3u);
  EXPECT_EQ(plan.bimGpuRoutes[0].slot,
            ShadowPassBimGpuSlot::OpaqueSingleSided);
  EXPECT_EQ(plan.bimGpuRoutes[0].pipeline, ShadowPassPipeline::Primary);
  EXPECT_EQ(plan.bimGpuRoutes[1].slot,
            ShadowPassBimGpuSlot::OpaqueWindingFlipped);
  EXPECT_EQ(plan.bimGpuRoutes[1].pipeline, ShadowPassPipeline::FrontCull);
  EXPECT_EQ(plan.bimGpuRoutes[2].slot,
            ShadowPassBimGpuSlot::OpaqueDoubleSided);
  EXPECT_EQ(plan.bimGpuRoutes[2].pipeline, ShadowPassPipeline::NoCull);
  ASSERT_EQ(plan.bimCpuRouteCount, 1u);
  EXPECT_EQ(plan.bimCpuRoutes[0].commands, &singleSided);
}

TEST(ShadowPassDrawPlannerTests, BimCpuRoutesKeepStableOrder) {
  const auto singleSided = drawCommands(8u);
  const auto windingFlipped = drawCommands(9u);
  const auto doubleSided = drawCommands(10u);

  const auto plan = buildShadowPassDrawPlan(
      {.bimGeometryReady = true,
       .bimDraws = {.singleSided = &singleSided,
                    .windingFlipped = &windingFlipped,
                    .doubleSided = &doubleSided}});

  ASSERT_EQ(plan.bimCpuRouteCount, 3u);
  EXPECT_EQ(plan.bimCpuRoutes[0].pipeline, ShadowPassPipeline::Primary);
  EXPECT_EQ(plan.bimCpuRoutes[1].pipeline, ShadowPassPipeline::FrontCull);
  EXPECT_EQ(plan.bimCpuRoutes[2].pipeline, ShadowPassPipeline::NoCull);
}

TEST(ShadowPassDrawPlannerTests, MissingGeometrySuppressesAllRoutes) {
  const auto singleSided = drawCommands(11u);

  const auto plan = buildShadowPassDrawPlan(
      {.sceneGeometryReady = false,
       .bimGeometryReady = false,
       .sceneGpuCullActive = true,
       .bimGpuFilteredMeshActive = true,
       .sceneDraws = {.singleSided = &singleSided},
       .bimDraws = {.singleSided = &singleSided}});

  EXPECT_FALSE(plan.sceneGpuRoute.active);
  EXPECT_EQ(plan.sceneCpuRouteCount, 0u);
  EXPECT_EQ(plan.bimGpuRouteCount, 0u);
  EXPECT_EQ(plan.bimCpuRouteCount, 0u);
}

} // namespace
