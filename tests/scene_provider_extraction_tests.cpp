#include "Container/renderer/RenderExtraction.h"
#include "Container/renderer/RendererDebugModels.h"
#include "Container/scene/SceneProvider.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using container::renderer::RadianceFieldDispatchInput;
using container::renderer::RadianceFieldExtractor;
using container::renderer::RayTracingGeometryBuildInput;
using container::renderer::RayTracingSceneExtractor;
using container::renderer::SplattingDispatchInput;
using container::renderer::SplattingExtractor;
using container::scene::IRenderSceneProvider;
using container::scene::SceneProviderId;
using container::scene::SceneProviderKind;
using container::scene::SceneProviderRegistry;
using container::scene::SceneProviderSnapshot;

class TestProvider final : public IRenderSceneProvider {
 public:
  TestProvider(SceneProviderKind kind, std::string id, std::size_t elements)
      : snapshot_{.kind = kind,
                  .id = SceneProviderId{std::move(id)},
                  .elementCount = elements} {
    snapshot_.displayName = snapshot_.id.value;
  }

  [[nodiscard]] SceneProviderSnapshot snapshot() const override {
    return snapshot_;
  }

 private:
  SceneProviderSnapshot snapshot_{};
};

class TestRayTracingExtractor final : public RayTracingSceneExtractor {
 public:
  [[nodiscard]] std::vector<RayTracingGeometryBuildInput> extract(
      const std::vector<SceneProviderSnapshot>& providers) const override {
    std::vector<RayTracingGeometryBuildInput> inputs;
    for (const SceneProviderSnapshot& provider : providers) {
      if (provider.kind == SceneProviderKind::Mesh ||
          provider.kind == SceneProviderKind::Bim) {
        inputs.push_back({
            .providerId = provider.id,
            .triangleGeometryCount = provider.elementCount,
            .instanceCount = provider.elementCount > 0 ? 1u : 0u,
            .geometryRevision = provider.revision.geometry,
            .hasOpaqueGeometry = provider.elementCount > 0,
        });
      }
    }
    return inputs;
  }
};

class TestSplattingExtractor final : public SplattingExtractor {
 public:
  [[nodiscard]] std::vector<SplattingDispatchInput> extract(
      const std::vector<SceneProviderSnapshot>& providers) const override {
    std::vector<SplattingDispatchInput> inputs;
    for (const SceneProviderSnapshot& provider : providers) {
      if (provider.kind == SceneProviderKind::GaussianSplatting) {
        inputs.push_back({.providerId = provider.id,
                          .splatCount = provider.elementCount,
                          .geometryRevision = provider.revision.geometry});
      }
    }
    return inputs;
  }
};

class TestRadianceFieldExtractor final : public RadianceFieldExtractor {
 public:
  [[nodiscard]] std::vector<RadianceFieldDispatchInput> extract(
      const std::vector<SceneProviderSnapshot>& providers) const override {
    std::vector<RadianceFieldDispatchInput> inputs;
    for (const SceneProviderSnapshot& provider : providers) {
      if (provider.kind == SceneProviderKind::RadianceField) {
        inputs.push_back({.providerId = provider.id,
                          .fieldCount = 1u,
                          .geometryRevision = provider.revision.geometry,
                          .usesOccupancyData = true});
      }
    }
    return inputs;
  }
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

TEST(SceneProviderRegistryTests, RegistersAndFiltersBackendNeutralProviders) {
  TestProvider mesh{SceneProviderKind::Mesh, "mesh-scene", 4};
  TestProvider bim{SceneProviderKind::Bim, "bim-scene", 12};
  TestProvider splats{SceneProviderKind::GaussianSplatting, "splats", 2048};

  SceneProviderRegistry registry;
  registry.registerProvider(mesh);
  registry.registerProvider(bim);
  registry.registerProvider(splats);

  ASSERT_NE(registry.find(SceneProviderId{"bim-scene"}), nullptr);
  EXPECT_EQ(registry.providersForKind(SceneProviderKind::Mesh).size(), 1u);
  EXPECT_EQ(registry.providersForKind(SceneProviderKind::Bim).size(), 1u);
  EXPECT_EQ(registry.providersForKind(SceneProviderKind::GaussianSplatting)
                .size(),
            1u);
  EXPECT_EQ(registry.snapshots().size(), 3u);
}

TEST(SceneProviderRegistryTests, RejectsEmptyAndDuplicateProviderIds) {
  TestProvider mesh{SceneProviderKind::Mesh, "mesh-scene", 4};
  TestProvider duplicate{SceneProviderKind::Mesh, "mesh-scene", 8};
  TestProvider empty{SceneProviderKind::Mesh, "", 1};

  SceneProviderRegistry registry;
  registry.registerProvider(mesh);

  EXPECT_THROW(registry.registerProvider(duplicate), std::invalid_argument);
  EXPECT_THROW(registry.registerProvider(empty), std::invalid_argument);
}

TEST(RenderExtractionTests, ExtractorsRouteProvidersByRepresentation) {
  TestProvider mesh{SceneProviderKind::Mesh, "mesh-scene", 4};
  TestProvider bim{SceneProviderKind::Bim, "bim-scene", 12};
  TestProvider splats{SceneProviderKind::GaussianSplatting, "splats", 2048};
  TestProvider field{SceneProviderKind::RadianceField, "field", 1};

  SceneProviderRegistry registry;
  registry.registerProvider(mesh);
  registry.registerProvider(bim);
  registry.registerProvider(splats);
  registry.registerProvider(field);
  const std::vector<SceneProviderSnapshot> snapshots = registry.snapshots();

  const TestRayTracingExtractor rayTracingExtractor;
  const TestSplattingExtractor splattingExtractor;
  const TestRadianceFieldExtractor radianceFieldExtractor;

  const std::vector<RayTracingGeometryBuildInput> rayInputs =
      rayTracingExtractor.extract(snapshots);
  const std::vector<SplattingDispatchInput> splatInputs =
      splattingExtractor.extract(snapshots);
  const std::vector<RadianceFieldDispatchInput> fieldInputs =
      radianceFieldExtractor.extract(snapshots);

  EXPECT_EQ(rayInputs.size(), 2u);
  EXPECT_EQ(splatInputs.size(), 1u);
  EXPECT_EQ(splatInputs.front().splatCount, 2048u);
  EXPECT_EQ(fieldInputs.size(), 1u);
  EXPECT_TRUE(fieldInputs.front().usesOccupancyData);
}

TEST(SceneDebugModelTests, BuildsUiNeutralProviderDebugState) {
  TestProvider mesh{SceneProviderKind::Mesh, "mesh-scene", 4};
  TestProvider splats{SceneProviderKind::GaussianSplatting, "splats", 2048};

  SceneProviderRegistry registry;
  registry.registerProvider(mesh);
  registry.registerProvider(splats);

  const container::renderer::SceneDebugModel debugModel =
      container::renderer::buildSceneDebugModel(registry.snapshots());

  ASSERT_EQ(debugModel.providers.size(), 2u);
  EXPECT_EQ(debugModel.providers[0].providerId, "mesh-scene");
  EXPECT_EQ(debugModel.providers[0].kind, "mesh");
  EXPECT_EQ(debugModel.providers[0].elementCount, 4u);
  EXPECT_EQ(debugModel.providers[1].providerId, "splats");
  EXPECT_EQ(debugModel.providers[1].kind, "gaussian-splatting");
}

TEST(RenderExtractionGuardrails, ProviderContractsStayBackendNeutral) {
  const std::string providerHeader =
      readRepoTextFile("include/Container/scene/SceneProvider.h");
  const std::string extractionHeader =
      readRepoTextFile("include/Container/renderer/RenderExtraction.h");
  const std::string debugModelHeader =
      readRepoTextFile("include/Container/renderer/RendererDebugModels.h");
  const std::string rendererFrontend =
      readRepoTextFile("src/renderer/RendererFrontend.cpp");

  for (const std::string& forbidden :
       {"Vk", "DrawCommand", "FrameRecordParams", "PrimitiveRange"}) {
    EXPECT_FALSE(contains(providerHeader, forbidden))
        << "Scene providers must stay backend-neutral.";
  }
  EXPECT_FALSE(contains(extractionHeader, "FrameRecordParams"));
  EXPECT_FALSE(contains(extractionHeader, "GBuffer"));
  EXPECT_FALSE(contains(debugModelHeader, "GuiManager"));
  EXPECT_TRUE(contains(rendererFrontend, "SceneProviderRegistry"));
  EXPECT_TRUE(contains(rendererFrontend,
                       ".sceneProviders = subs_.sceneProviderRegistry.get()"));
}
