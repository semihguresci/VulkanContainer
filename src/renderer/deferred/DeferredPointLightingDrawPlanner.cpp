#include "Container/renderer/deferred/DeferredPointLightingDrawPlanner.h"

#include <algorithm>

namespace container::renderer {

namespace {

[[nodiscard]] uint32_t ceilDiv(uint32_t value, uint32_t divisor) {
  return (value + divisor - 1u) / divisor;
}

[[nodiscard]] uint32_t tileCount(uint32_t pixels) {
  return std::max(1u, ceilDiv(pixels, container::gpu::kTileSize));
}

} // namespace

DeferredPointLightingDrawPlan buildDeferredPointLightingDrawPlan(
    const DeferredPointLightingDrawInputs &inputs) {
  DeferredPointLightingDrawPlan plan{};
  plan.path = inputs.state.path;
  plan.stencilPipeline =
      inputs.debugVisualizePointLightStencil
          ? DeferredPointLightingStencilPipeline::PointLightStencilDebug
          : DeferredPointLightingStencilPipeline::PointLight;

  if (inputs.state.path == DeferredPointLightingPath::Tiled) {
    plan.tiledPushConstants.tileCountX = tileCount(inputs.framebufferWidth);
    plan.tiledPushConstants.tileCountY = tileCount(inputs.framebufferHeight);
    plan.tiledPushConstants.depthSliceCount =
        container::gpu::kClusterDepthSlices;
    plan.tiledPushConstants.cameraNear = inputs.cameraNear;
    plan.tiledPushConstants.cameraFar = inputs.cameraFar;
    return plan;
  }

  if (inputs.state.path != DeferredPointLightingPath::Stencil) {
    return plan;
  }

  plan.lightVolumeIndexCount = inputs.lightVolumeIndexCount;
  const uint32_t requestedLightCount =
      std::min(inputs.state.stencilLightCount,
               container::gpu::kMaxDeferredPointLights);
  const uint32_t availableLightCount = static_cast<uint32_t>(
      std::min<size_t>(inputs.pointLights.size(),
                       container::gpu::kMaxDeferredPointLights));
  plan.stencilRouteCount = std::min(requestedLightCount, availableLightCount);

  for (uint32_t lightIndex = 0u; lightIndex < plan.stencilRouteCount;
       ++lightIndex) {
    plan.stencilRoutes[lightIndex] = {.light = inputs.pointLights[lightIndex]};
  }
  return plan;
}

} // namespace container::renderer
