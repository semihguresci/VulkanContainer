#include "Container/renderer/bim/BimSectionClipCapPassPlanner.h"

#include "Container/renderer/scene/DrawCommand.h"

#include <algorithm>

namespace container::renderer {

namespace {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand> *commands) {
  return commands != nullptr && !commands->empty();
}

[[nodiscard]] bool commonSectionClipCapReady(
    const BimSectionClipCapPassInputs &inputs) {
  return inputs.enabled && inputs.geometryReady && inputs.wireframeLayoutReady &&
         inputs.wireframePushConstantsReady &&
         (inputs.fillEnabled || inputs.hatchEnabled);
}

void appendRoute(BimSectionClipCapPassPlan &plan,
                 BimSectionClipCapPassRoute route) {
  if (!hasDrawCommands(route.commands) || plan.routeCount >= plan.routes.size()) {
    return;
  }

  plan.routes[plan.routeCount] = route;
  ++plan.routeCount;
}

} // namespace

float sanitizeBimSectionClipCapLineWidth(float lineWidth) {
  return std::max(lineWidth, 1.0f);
}

float rasterBimSectionClipCapLineWidth(float lineWidth,
                                       bool wideLinesSupported) {
  return wideLinesSupported ? sanitizeBimSectionClipCapLineWidth(lineWidth)
                            : 1.0f;
}

BimSectionClipCapPassPlanner::BimSectionClipCapPassPlanner(
    BimSectionClipCapPassInputs inputs)
    : inputs_(inputs) {}

BimSectionClipCapPassPlan BimSectionClipCapPassPlanner::build() const {
  BimSectionClipCapPassPlan plan{};
  if (!commonSectionClipCapReady(inputs_)) {
    return plan;
  }

  if (inputs_.fillEnabled && inputs_.fillPipelineReady) {
    appendRoute(plan,
                {.pipeline = BimSectionClipCapPassPipeline::Fill,
                 .commands = inputs_.fillDrawCommands,
                 .color = glm::vec3(inputs_.fillColor),
                 .opacity = inputs_.fillColor.a,
                 .drawLineWidth = 1.0f,
                 .rasterLineWidth = 1.0f,
                 .rasterLineWidthApplies = false,
                 .resetRasterLineWidth = false});
  }

  if (inputs_.hatchEnabled && inputs_.hatchPipelineReady) {
    const float lineWidth = rasterBimSectionClipCapLineWidth(
        inputs_.hatchLineWidth, inputs_.wideLinesSupported);
    appendRoute(plan,
                {.pipeline = BimSectionClipCapPassPipeline::Hatch,
                 .commands = inputs_.hatchDrawCommands,
                 .color = glm::vec3(inputs_.hatchColor),
                 .opacity = inputs_.hatchColor.a,
                 .drawLineWidth = lineWidth,
                 .rasterLineWidth = lineWidth,
                 .rasterLineWidthApplies = true,
                 .resetRasterLineWidth = true});
  }

  plan.active = plan.routeCount > 0u;
  return plan;
}

BimSectionClipCapPassPlan buildBimSectionClipCapPassPlan(
    const BimSectionClipCapPassInputs &inputs) {
  return BimSectionClipCapPassPlanner(inputs).build();
}

} // namespace container::renderer
