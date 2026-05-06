#include "Container/renderer/scene/DrawCommand.h"
#include "Container/renderer/scene/SceneTransparentDrawPlanner.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

using container::renderer::DrawCommand;
using container::renderer::SceneTransparentDrawPipeline;
using container::renderer::buildSceneTransparentDrawPlan;

[[nodiscard]] std::vector<DrawCommand> drawCommands(uint32_t firstIndex) {
  return {DrawCommand{.objectIndex = firstIndex,
                      .firstIndex = firstIndex,
                      .indexCount = 3u,
                      .instanceCount = 1u}};
}

TEST(SceneTransparentDrawPlannerTests, AggregateMapsToPrimaryRoute) {
  const auto aggregate = drawCommands(1u);

  const auto plan =
      buildSceneTransparentDrawPlan({.aggregate = &aggregate});

  ASSERT_EQ(plan.routeCount, 1u);
  EXPECT_EQ(plan.routes[0].pipeline, SceneTransparentDrawPipeline::Primary);
  EXPECT_EQ(plan.routes[0].commands, &aggregate);
}

TEST(SceneTransparentDrawPlannerTests,
     SplitDrawsTakePrecedenceOverAggregate) {
  const auto aggregate = drawCommands(2u);
  const auto singleSided = drawCommands(3u);
  const auto windingFlipped = drawCommands(4u);
  const auto doubleSided = drawCommands(5u);

  const auto plan =
      buildSceneTransparentDrawPlan({.aggregate = &aggregate,
                                     .singleSided = &singleSided,
                                     .windingFlipped = &windingFlipped,
                                     .doubleSided = &doubleSided});

  ASSERT_EQ(plan.routeCount, 3u);
  EXPECT_EQ(plan.routes[0].pipeline, SceneTransparentDrawPipeline::Primary);
  EXPECT_EQ(plan.routes[0].commands, &singleSided);
  EXPECT_EQ(plan.routes[1].pipeline, SceneTransparentDrawPipeline::FrontCull);
  EXPECT_EQ(plan.routes[1].commands, &windingFlipped);
  EXPECT_EQ(plan.routes[2].pipeline, SceneTransparentDrawPipeline::NoCull);
  EXPECT_EQ(plan.routes[2].commands, &doubleSided);
}

TEST(SceneTransparentDrawPlannerTests,
     WindingOnlySplitSuppressesAggregateFallback) {
  const auto aggregate = drawCommands(6u);
  const auto windingFlipped = drawCommands(7u);

  const auto plan =
      buildSceneTransparentDrawPlan({.aggregate = &aggregate,
                                     .windingFlipped = &windingFlipped});

  ASSERT_EQ(plan.routeCount, 1u);
  EXPECT_EQ(plan.routes[0].pipeline, SceneTransparentDrawPipeline::FrontCull);
  EXPECT_EQ(plan.routes[0].commands, &windingFlipped);
}

TEST(SceneTransparentDrawPlannerTests, EmptyDrawListsProduceNoRoutes) {
  const auto empty = std::vector<DrawCommand>{};

  const auto plan = buildSceneTransparentDrawPlan(
      {.aggregate = &empty, .singleSided = &empty, .doubleSided = &empty});

  EXPECT_EQ(plan.routeCount, 0u);
}

} // namespace
