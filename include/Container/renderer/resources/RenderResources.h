#pragma once

#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/pipeline/PipelineTypes.h"
#include "Container/renderer/core/RenderPassManager.h"

namespace container::renderer {

// Bundles the built render-pass and pipeline state that RendererFrontend
// creates once (and recreates on resize).  Keeps these correlated objects
// together instead of scattered across RendererFrontend's member list.
struct RenderResources {
  RenderPasses                    renderPasses{};
  PipelineBuildResult             builtPipelines{};
  GBufferFormats                  gBufferFormats{};
};

}  // namespace container::renderer
