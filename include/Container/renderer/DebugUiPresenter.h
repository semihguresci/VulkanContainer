#pragma once

#include "Container/renderer/RenderGraph.h"
#include "Container/utility/GuiManager.h"

namespace container::renderer {

// Keeps renderer debug UI synchronization out of RendererFrontend's frame
// submission path. The presenter translates RenderGraph state into GUI toggle
// rows and applies edited toggle state back to the graph.
class DebugUiPresenter {
 public:
  static void publishRenderPasses(container::ui::GuiManager& guiManager,
                                  const RenderGraph& graph);

  // Returns true when protected/dependent toggles were corrected.
  static bool applyRenderPassToggles(container::ui::GuiManager& guiManager,
                                     RenderGraph& graph);
};

}  // namespace container::renderer
