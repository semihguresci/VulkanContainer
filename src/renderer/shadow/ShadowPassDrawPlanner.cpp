#include "Container/renderer/shadow/ShadowPassDrawPlanner.h"

#include "Container/renderer/scene/DrawCommand.h"

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

void appendCpuRoute(ShadowPassDrawPlan &plan, bool sceneRoute,
                    ShadowPassPipeline pipeline,
                    const std::vector<DrawCommand> *commands) {
  if (!hasDrawCommands(commands)) {
    return;
  }

  std::array<ShadowPassCpuRoute, 3> &routes =
      sceneRoute ? plan.sceneCpuRoutes : plan.bimCpuRoutes;
  uint32_t &routeCount =
      sceneRoute ? plan.sceneCpuRouteCount : plan.bimCpuRouteCount;
  if (routeCount >= routes.size()) {
    return;
  }

  routes[routeCount] = {.pipeline = pipeline, .commands = commands};
  ++routeCount;
}

void appendBimGpuRoute(ShadowPassDrawPlan &plan, ShadowPassBimGpuSlot slot,
                       ShadowPassPipeline pipeline) {
  if (plan.bimGpuRouteCount >= plan.bimGpuRoutes.size()) {
    return;
  }

  plan.bimGpuRoutes[plan.bimGpuRouteCount] = {.slot = slot,
                                              .pipeline = pipeline};
  ++plan.bimGpuRouteCount;
}

} // namespace

ShadowPassDrawPlanner::ShadowPassDrawPlanner(ShadowPassDrawInputs inputs)
    : inputs_(inputs) {}

ShadowPassDrawPlan ShadowPassDrawPlanner::build() const {
  ShadowPassDrawPlan plan{};

  if (inputs_.sceneGeometryReady) {
    plan.sceneGpuRoute = {.active = inputs_.sceneGpuCullActive,
                          .pipeline = ShadowPassPipeline::Primary};
    if (!plan.sceneGpuRoute.active) {
      appendCpuRoute(plan, true, ShadowPassPipeline::Primary,
                     inputs_.sceneDraws.singleSided);
    }
    appendCpuRoute(plan, true, ShadowPassPipeline::FrontCull,
                   inputs_.sceneDraws.windingFlipped);
    appendCpuRoute(plan, true, ShadowPassPipeline::NoCull,
                   inputs_.sceneDraws.doubleSided);
  }

  if (inputs_.bimGeometryReady) {
    if (inputs_.bimGpuFilteredMeshActive) {
      appendBimGpuRoute(plan, ShadowPassBimGpuSlot::OpaqueSingleSided,
                        ShadowPassPipeline::Primary);
      appendBimGpuRoute(plan, ShadowPassBimGpuSlot::OpaqueWindingFlipped,
                        ShadowPassPipeline::FrontCull);
      appendBimGpuRoute(plan, ShadowPassBimGpuSlot::OpaqueDoubleSided,
                        ShadowPassPipeline::NoCull);
    }
    appendCpuRoute(plan, false, ShadowPassPipeline::Primary,
                   inputs_.bimDraws.singleSided);
    appendCpuRoute(plan, false, ShadowPassPipeline::FrontCull,
                   inputs_.bimDraws.windingFlipped);
    appendCpuRoute(plan, false, ShadowPassPipeline::NoCull,
                   inputs_.bimDraws.doubleSided);
  }

  return plan;
}

ShadowPassDrawPlan
buildShadowPassDrawPlan(const ShadowPassDrawInputs &inputs) {
  return ShadowPassDrawPlanner(inputs).build();
}

} // namespace container::renderer
