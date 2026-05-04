#include "Container/renderer/DeferredRasterTechnique.h"
#include "Container/renderer/RenderTechnique.h"
#include "Container/renderer/TechniqueDebugModel.h"

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

TEST(TechniqueDebugModelGuardrails, DebugModelsStayUiBackendNeutral) {
  const std::string debugModelHeader =
      readRepoTextFile("include/Container/renderer/TechniqueDebugModel.h");
  const std::string renderTechniqueHeader =
      readRepoTextFile("include/Container/renderer/RenderTechnique.h");

  for (const std::string& forbidden : {"imgui", "ImGui", "GuiManager", "Vk"}) {
    EXPECT_FALSE(contains(debugModelHeader, forbidden))
        << "Technique debug models should stay UI/backend neutral.";
  }
  EXPECT_TRUE(contains(renderTechniqueHeader, "debugModel()"));
  EXPECT_TRUE(contains(renderTechniqueHeader, "SceneProviderRegistry"));
}
