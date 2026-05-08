#include "Container/renderer/deferred/DeferredRasterTechnique.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/TechniqueDebugModel.h"
#include "Container/renderer/debug/DebugUiPresenter.h"
#include "Container/utility/GuiManager.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

class MinimalTechnique final : public container::renderer::RenderTechnique {
 public:
  [[nodiscard]] container::renderer::RenderTechniqueId id() const override {
    return container::renderer::RenderTechniqueId::ForwardRaster;
  }

  [[nodiscard]] std::string_view name() const override {
    return container::renderer::renderTechniqueName(id());
  }

  [[nodiscard]] std::string_view displayName() const override {
    return container::renderer::renderTechniqueDisplayName(id());
  }

  [[nodiscard]] container::renderer::RenderTechniqueAvailability availability(
      const container::renderer::RenderSystemContext&) const override {
    return container::renderer::RenderTechniqueAvailability::availableNow();
  }

  void buildFrameGraph(container::renderer::RenderSystemContext&) override {}
};

std::string readRepoTextFile(const std::filesystem::path& relativePath) {
  const std::filesystem::path path =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / relativePath;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST(TechniqueDebugModelTests, DefaultTechniqueModelUsesTechniqueIdentity) {
  MinimalTechnique technique;

  const container::renderer::TechniqueDebugModel model =
      technique.debugModel();

  EXPECT_EQ(model.techniqueName, "forward-raster");
  EXPECT_EQ(model.displayName, "Forward raster");
  EXPECT_TRUE(model.panels.empty());
}

TEST(TechniqueDebugModelTests, DeferredRasterPublishesDebugPanels) {
  container::renderer::DeferredRasterTechnique technique;

  const container::renderer::TechniqueDebugModel model =
      technique.debugModel();

  ASSERT_EQ(model.techniqueName, "deferred-raster");
  ASSERT_FALSE(model.panels.empty());
  EXPECT_EQ(model.panels.front().id, "deferred-frame");
  EXPECT_FALSE(model.panels.front().controls.empty());
}

TEST(TechniqueDebugModelTests,
     DebugPresenterPublishesRenderGraphDebugModelRows) {
  container::ui::GuiManager guiManager;
  container::renderer::RenderGraphDebugModel debugModel{};
  debugModel.passes.push_back(
      {.passName = "Lighting",
       .enabled = false,
       .active = false,
       .locked = true,
       .autoDisabled = true,
       .skipReason = "Disabled",
       .dependencyNote = "inactive: disabled by test"});

  container::renderer::DebugUiPresenter::publishRenderGraphDebugModel(
      guiManager, debugModel);

  ASSERT_EQ(guiManager.renderPassToggles().size(), 1u);
  const container::ui::RenderPassToggle& toggle =
      guiManager.renderPassToggles().front();
  EXPECT_EQ(toggle.name, "Lighting");
  EXPECT_FALSE(toggle.enabled);
  EXPECT_TRUE(toggle.locked);
  EXPECT_TRUE(toggle.autoDisabled);
  EXPECT_EQ(toggle.dependencyNote, "inactive: disabled by test");
}

TEST(TechniqueDebugModelGuardrails, DebugModelsStayUiBackendNeutral) {
  const std::string debugModelHeader =
      readRepoTextFile("include/Container/renderer/core/TechniqueDebugModel.h");
  const std::string renderTechniqueHeader =
      readRepoTextFile("include/Container/renderer/core/RenderTechnique.h");

  for (const std::string& forbidden : {"imgui", "ImGui", "GuiManager", "Vk"}) {
    EXPECT_FALSE(contains(debugModelHeader, forbidden))
        << "Technique debug models should stay UI/backend neutral.";
  }
  EXPECT_TRUE(contains(renderTechniqueHeader, "debugModel()"));
  EXPECT_TRUE(contains(renderTechniqueHeader, "SceneProviderRegistry"));
}

TEST(TechniqueDebugModelGuardrails, GuiHeaderKeepsRendererDebugModelsNeutral) {
  const std::string guiHeader =
      readRepoTextFile("include/Container/utility/GuiManager.h");
  const std::string guiDebugState =
      readRepoTextFile("include/Container/utility/GuiDebugState.h");
  const std::string debugPresenterHeader = readRepoTextFile(
      "include/Container/renderer/debug/DebugUiPresenter.h");
  const std::string debugPresenter =
      readRepoTextFile("src/renderer/debug/DebugUiPresenter.cpp");

  EXPECT_TRUE(contains(guiHeader, "GuiDebugState.h"));
  EXPECT_TRUE(contains(guiHeader, "struct RendererTelemetryView"));
  EXPECT_FALSE(contains(guiHeader, "BimSemanticColorMode.h"));
  EXPECT_FALSE(contains(guiHeader, "RendererTelemetry.h"));
  EXPECT_FALSE(contains(guiHeader,
                        "container::renderer::RendererTelemetryView "
                        "rendererTelemetry_"));
  EXPECT_TRUE(contains(guiDebugState, "GuiRendererTelemetryView"));
  EXPECT_TRUE(contains(debugPresenterHeader, "TechniqueDebugModel.h"));
  EXPECT_TRUE(
      contains(debugPresenterHeader, "publishRenderGraphDebugModel"));
  EXPECT_FALSE(contains(debugPresenterHeader, "RenderGraph.h"));
  EXPECT_FALSE(contains(debugPresenterHeader, "GuiManager.h"));
  EXPECT_TRUE(contains(debugPresenter, "graph.debugModel()"));
}
