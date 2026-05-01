#pragma once

#include "Container/renderer/FrameResourceManager.h"
#include "Container/renderer/PipelineTypes.h"
#include "Container/renderer/RenderPassManager.h"

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
