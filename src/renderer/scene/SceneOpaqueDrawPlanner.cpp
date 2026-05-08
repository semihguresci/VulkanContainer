#include "Container/renderer/scene/SceneOpaqueDrawPlanner.h"

#include "Container/renderer/scene/DrawCommand.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

void appendCpuRoute(SceneOpaqueDrawPlan &plan, SceneOpaqueDrawRouteKind kind,
                    SceneOpaqueDrawPipeline pipeline,
                    const std::vector<DrawCommand> *commands) {
  if (!hasDrawCommands(commands) || plan.cpuRouteCount >= plan.cpuRoutes.size()) {
    return;
  }

  plan.cpuRoutes[plan.cpuRouteCount] = {
      .kind = kind,
      .pipeline = pipeline,
      .commands = commands,
  };
  ++plan.cpuRouteCount;
}

} // namespace

SceneOpaqueDrawPlanner::SceneOpaqueDrawPlanner(SceneOpaqueDrawInputs inputs)
    : inputs_(inputs) {}

SceneOpaqueDrawPlan SceneOpaqueDrawPlanner::build() const {
  SceneOpaqueDrawPlan plan{};
  plan.useGpuIndirectSingleSided =
      inputs_.gpuIndirectAvailable && hasDrawCommands(inputs_.draws.singleSided);
  plan.gpuIndirectRoute.commands =
      plan.useGpuIndirectSingleSided ? inputs_.draws.singleSided : nullptr;

  if (!plan.useGpuIndirectSingleSided) {
    appendCpuRoute(plan, SceneOpaqueDrawRouteKind::CpuSingleSided,
                   SceneOpaqueDrawPipeline::Primary,
                   inputs_.draws.singleSided);
  }
  appendCpuRoute(plan, SceneOpaqueDrawRouteKind::CpuWindingFlipped,
                 SceneOpaqueDrawPipeline::FrontCull,
                 inputs_.draws.windingFlipped);
  appendCpuRoute(plan, SceneOpaqueDrawRouteKind::CpuDoubleSided,
                 SceneOpaqueDrawPipeline::NoCull, inputs_.draws.doubleSided);
  return plan;
}

SceneOpaqueDrawPlan
buildSceneOpaqueDrawPlan(const SceneOpaqueDrawInputs &inputs) {
  return SceneOpaqueDrawPlanner(inputs).build();
}

} // namespace container::renderer
