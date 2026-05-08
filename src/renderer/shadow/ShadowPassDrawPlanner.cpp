#include "Container/renderer/shadow/ShadowPassDrawPlanner.h"

#include "Container/renderer/scene/DrawCommand.h"

namespace container::renderer {

namespace {

// Shadow-map casters are visibility geometry; winding/normal direction must
// not decide whether a surface blocks light.
constexpr ShadowPassPipeline kShadowCasterPipeline = ShadowPassPipeline::NoCull;

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
                          .pipeline = kShadowCasterPipeline};
    if (!plan.sceneGpuRoute.active) {
      appendCpuRoute(plan, true, kShadowCasterPipeline,
                     inputs_.sceneDraws.singleSided);
    }
    appendCpuRoute(plan, true, kShadowCasterPipeline,
                   inputs_.sceneDraws.windingFlipped);
    appendCpuRoute(plan, true, kShadowCasterPipeline,
                   inputs_.sceneDraws.doubleSided);
  }

  if (inputs_.bimGeometryReady) {
    if (inputs_.bimGpuFilteredMeshActive) {
      appendBimGpuRoute(plan, ShadowPassBimGpuSlot::OpaqueSingleSided,
                        kShadowCasterPipeline);
      appendBimGpuRoute(plan, ShadowPassBimGpuSlot::OpaqueWindingFlipped,
                        kShadowCasterPipeline);
      appendBimGpuRoute(plan, ShadowPassBimGpuSlot::OpaqueDoubleSided,
                        kShadowCasterPipeline);
    }
    appendCpuRoute(plan, false, kShadowCasterPipeline,
                   inputs_.bimDraws.singleSided);
    appendCpuRoute(plan, false, kShadowCasterPipeline,
                   inputs_.bimDraws.windingFlipped);
    appendCpuRoute(plan, false, kShadowCasterPipeline,
                   inputs_.bimDraws.doubleSided);
  }

  return plan;
}

ShadowPassDrawPlan
buildShadowPassDrawPlan(const ShadowPassDrawInputs &inputs) {
  return ShadowPassDrawPlanner(inputs).build();
}

} // namespace container::renderer
