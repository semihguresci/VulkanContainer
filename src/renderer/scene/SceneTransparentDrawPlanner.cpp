#include "Container/renderer/scene/SceneTransparentDrawPlanner.h"

#include "Container/renderer/scene/DrawCommand.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool hasSplitTransparentDrawCommands(
    const SceneTransparentDrawLists &draws) {
  return hasDrawCommands(draws.singleSided) ||
         hasDrawCommands(draws.windingFlipped) ||
         hasDrawCommands(draws.doubleSided);
}

[[nodiscard]] const std::vector<DrawCommand> *
primaryTransparentDrawCommands(const SceneTransparentDrawLists &draws) {
  return hasSplitTransparentDrawCommands(draws) ? draws.singleSided
                                                : draws.aggregate;
}

void appendRoute(SceneTransparentDrawPlan &plan,
                 SceneTransparentDrawPipeline pipeline,
                 const std::vector<DrawCommand> *commands) {
  if (!hasDrawCommands(commands) || plan.routeCount >= plan.routes.size()) {
    return;
  }

  plan.routes[plan.routeCount] = {.pipeline = pipeline, .commands = commands};
  ++plan.routeCount;
}

} // namespace

SceneTransparentDrawPlanner::SceneTransparentDrawPlanner(
    SceneTransparentDrawLists draws)
    : draws_(draws) {}

SceneTransparentDrawPlan SceneTransparentDrawPlanner::build() const {
  SceneTransparentDrawPlan plan{};
  appendRoute(plan, SceneTransparentDrawPipeline::Primary,
              primaryTransparentDrawCommands(draws_));
  appendRoute(plan, SceneTransparentDrawPipeline::FrontCull,
              draws_.windingFlipped);
  appendRoute(plan, SceneTransparentDrawPipeline::NoCull, draws_.doubleSided);
  return plan;
}

SceneTransparentDrawPlan
buildSceneTransparentDrawPlan(const SceneTransparentDrawLists &draws) {
  return SceneTransparentDrawPlanner(draws).build();
}

} // namespace container::renderer
