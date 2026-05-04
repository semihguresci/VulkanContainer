#include "Container/renderer/RenderTechnique.h"
#include "Container/renderer/RendererDeviceCapabilities.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestTechnique final : public container::renderer::RenderTechnique {
 public:
  TestTechnique(container::renderer::RenderTechniqueId id,
                bool available = true,
                std::string reason = {})
      : id_(id), available_(available), reason_(std::move(reason)) {}

  [[nodiscard]] container::renderer::RenderTechniqueId id() const override {
    return id_;
  }

  [[nodiscard]] std::string_view name() const override {
    return container::renderer::renderTechniqueName(id_);
  }

  [[nodiscard]] std::string_view displayName() const override {
    return container::renderer::renderTechniqueDisplayName(id_);
  }

  [[nodiscard]] container::renderer::RenderTechniqueAvailability availability(
      const container::renderer::RenderSystemContext&) const override {
    if (!available_) {
      return container::renderer::RenderTechniqueAvailability::unavailable(
          reason_);
    }
    return container::renderer::RenderTechniqueAvailability::availableNow();
  }

  void buildFrameGraph(
      container::renderer::RenderSystemContext&) override {
    ++buildFrameGraphCallCount;
  }

  int buildFrameGraphCallCount{0};

 private:
  container::renderer::RenderTechniqueId id_{
      container::renderer::RenderTechniqueId::DeferredRaster};
  bool available_{true};
  std::string reason_{};
};

class RayTracingRequiredTechnique final
    : public container::renderer::RenderTechnique {
 public:
  [[nodiscard]] container::renderer::RenderTechniqueId id() const override {
    return container::renderer::RenderTechniqueId::RayTracing;
  }

  [[nodiscard]] std::string_view name() const override {
    return container::renderer::renderTechniqueName(id());
  }

  [[nodiscard]] std::string_view displayName() const override {
    return container::renderer::renderTechniqueDisplayName(id());
  }

  [[nodiscard]] container::renderer::RenderTechniqueAvailability availability(
      const container::renderer::RenderSystemContext& context) const override {
    if (context.deviceCapabilities == nullptr ||
        !context.deviceCapabilities->supportsRayTracing()) {
      return container::renderer::RenderTechniqueAvailability::unavailable(
          "ray tracing device capabilities are unavailable");
    }
    return container::renderer::RenderTechniqueAvailability::availableNow();
  }

  void buildFrameGraph(container::renderer::RenderSystemContext&) override {}
};

[[nodiscard]] bool contains(std::string_view haystack,
                            std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

[[nodiscard]] std::string readRepoTextFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("failed to open " + path.string());
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace

TEST(RenderTechniqueRegistry, ParsesStableTechniqueNames) {
  using container::renderer::RenderTechniqueId;

  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("deferred-raster"),
            RenderTechniqueId::DeferredRaster);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("forward-raster"),
            RenderTechniqueId::ForwardRaster);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("ray-tracing"),
            RenderTechniqueId::RayTracing);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("path-tracing"),
            RenderTechniqueId::PathTracing);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName(
                "gaussian-splatting"),
            RenderTechniqueId::GaussianSplatting);
  EXPECT_EQ(container::renderer::renderTechniqueIdFromName("radiance-field"),
            RenderTechniqueId::RadianceField);
  EXPECT_FALSE(container::renderer::renderTechniqueIdFromName("unknown")
                   .has_value());
}

TEST(RenderTechniqueRegistry,
     KnownTechniqueDescriptorsExposeFutureAlgorithmsWithoutRegisteringThem) {
  using container::renderer::RenderTechniqueId;

  const auto descriptors =
      container::renderer::knownRenderTechniqueDescriptors();
  ASSERT_EQ(descriptors.size(), 6u);

  const auto findDescriptor = [&](RenderTechniqueId id) {
    return std::ranges::find_if(descriptors, [id](const auto& descriptor) {
      return descriptor.id == id;
    });
  };

  const auto deferred = findDescriptor(RenderTechniqueId::DeferredRaster);
  ASSERT_NE(deferred, descriptors.end());
  EXPECT_TRUE(deferred->implemented);

  const auto rayTracing = findDescriptor(RenderTechniqueId::RayTracing);
  ASSERT_NE(rayTracing, descriptors.end());
  EXPECT_FALSE(rayTracing->implemented);
  EXPECT_TRUE(rayTracing->requiresRayTracing);

  const auto pathTracing = findDescriptor(RenderTechniqueId::PathTracing);
  ASSERT_NE(pathTracing, descriptors.end());
  EXPECT_FALSE(pathTracing->implemented);
  EXPECT_TRUE(pathTracing->requiresPathTracing);

  container::renderer::RenderTechniqueRegistry registry =
      container::renderer::createDefaultRenderTechniqueRegistry();
  EXPECT_EQ(registry.techniques().size(), 1u);
  EXPECT_EQ(registry.find(RenderTechniqueId::RayTracing), nullptr);
  EXPECT_EQ(registry.find(RenderTechniqueId::PathTracing), nullptr);
}

TEST(RenderTechniqueRegistry, RejectsNullAndDuplicateRegistrations) {
  using container::renderer::RenderTechniqueId;

  container::renderer::RenderTechniqueRegistry registry;
  EXPECT_THROW(registry.registerTechnique(nullptr), std::invalid_argument);

  registry.registerTechnique(
      std::make_unique<TestTechnique>(RenderTechniqueId::DeferredRaster));
  EXPECT_THROW(
      registry.registerTechnique(
          std::make_unique<TestTechnique>(RenderTechniqueId::DeferredRaster)),
      std::invalid_argument);
}

TEST(RenderTechniqueRegistry, FallsBackToDeferredWhenRequestedUnavailable) {
  using container::renderer::RenderTechniqueId;

  container::renderer::RenderTechniqueRegistry registry;
  registry.registerTechnique(
      std::make_unique<TestTechnique>(RenderTechniqueId::DeferredRaster));
  registry.registerTechnique(std::make_unique<TestTechnique>(
      RenderTechniqueId::RayTracing, false,
      "ray tracing device extensions are unavailable"));

  container::renderer::RenderSystemContext context{};
  const container::renderer::RenderTechniqueSelection selection =
      registry.select(RenderTechniqueId::RayTracing, context);

  ASSERT_NE(selection.technique, nullptr);
  EXPECT_TRUE(selection.usedFallback);
  EXPECT_EQ(selection.requested, RenderTechniqueId::RayTracing);
  EXPECT_EQ(selection.selected, RenderTechniqueId::DeferredRaster);
  EXPECT_EQ(selection.technique->id(), RenderTechniqueId::DeferredRaster);
  EXPECT_TRUE(contains(selection.unavailableReason,
                       "device extensions are unavailable"));
}

TEST(RenderTechniqueRegistry, UsesDeviceCapabilitiesForTechniqueAvailability) {
  using container::renderer::RenderTechniqueId;

  container::renderer::RenderTechniqueRegistry registry;
  registry.registerTechnique(
      std::make_unique<TestTechnique>(RenderTechniqueId::DeferredRaster));
  registry.registerTechnique(std::make_unique<RayTracingRequiredTechnique>());

  const container::renderer::RendererDeviceCapabilities rasterOnly =
      container::renderer::RendererDeviceCapabilities::rasterOnly();
  container::renderer::RenderSystemContext unavailableContext{
      .deviceCapabilities = &rasterOnly};

  const container::renderer::RenderTechniqueSelection fallback =
      registry.select(RenderTechniqueId::RayTracing, unavailableContext);
  EXPECT_TRUE(fallback.usedFallback);
  EXPECT_EQ(fallback.selected, RenderTechniqueId::DeferredRaster);
  EXPECT_TRUE(contains(fallback.unavailableReason,
                       "ray tracing device capabilities"));

  container::renderer::RendererDeviceCapabilities rayTracing{};
  rayTracing.descriptorIndexing = true;
  rayTracing.bufferDeviceAddress = true;
  rayTracing.accelerationStructure = true;
  rayTracing.rayTracingPipeline = true;
  container::renderer::RenderSystemContext availableContext{
      .deviceCapabilities = &rayTracing};

  const container::renderer::RenderTechniqueSelection selection =
      registry.select(RenderTechniqueId::RayTracing, availableContext);
  EXPECT_FALSE(selection.usedFallback);
  EXPECT_EQ(selection.selected, RenderTechniqueId::RayTracing);
  ASSERT_NE(selection.technique, nullptr);
  EXPECT_EQ(selection.technique->id(), RenderTechniqueId::RayTracing);
}

TEST(RendererDeviceCapabilities, ReportsMissingRayAndPathRequirements) {
  const container::renderer::RendererDeviceCapabilities rasterOnly =
      container::renderer::RendererDeviceCapabilities::rasterOnly();

  const std::vector<std::string> missingRayTracing =
      rasterOnly.missingRayTracingRequirements();
  EXPECT_FALSE(missingRayTracing.empty());
  EXPECT_NE(std::ranges::find(missingRayTracing, "buffer device address"),
            missingRayTracing.end());
  EXPECT_NE(std::ranges::find(missingRayTracing, "acceleration structure"),
            missingRayTracing.end());
  EXPECT_NE(std::ranges::find(missingRayTracing, "ray tracing pipeline"),
            missingRayTracing.end());

  const std::vector<std::string> missingPathTracing =
      rasterOnly.missingPathTracingRequirements();
  EXPECT_NE(std::ranges::find(missingPathTracing, "storage image atomics"),
            missingPathTracing.end());

  container::renderer::RendererDeviceCapabilities full{};
  full.descriptorIndexing = true;
  full.bufferDeviceAddress = true;
  full.accelerationStructure = true;
  full.rayTracingPipeline = true;
  full.storageImageAtomics = true;
  EXPECT_TRUE(full.supportsPathTracing());
  EXPECT_TRUE(full.missingPathTracingRequirements().empty());
}

TEST(RenderTechniqueRegistry, DefaultRegistryRegistersOnlyDeferredRaster) {
  container::renderer::RenderTechniqueRegistry registry =
      container::renderer::createDefaultRenderTechniqueRegistry();

  ASSERT_NE(registry.find(
                container::renderer::RenderTechniqueId::DeferredRaster),
            nullptr);
  EXPECT_EQ(registry.techniques().size(), 1u);
}

TEST(RenderTechniqueGuardrails, FrameRecorderDoesNotOwnGraphRegistration) {
  const std::filesystem::path frameRecorderPath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "src" / "renderer" /
      "FrameRecorder.cpp";
  const std::string source = readRepoTextFile(frameRecorderPath);

  EXPECT_FALSE(contains(source, "void FrameRecorder::buildGraph()"));
  EXPECT_FALSE(contains(source, "graph_.addPass("));
}

TEST(RenderTechniqueGuardrails,
     FutureTechniquePassesStayOutOfDeferredRasterGraph) {
  const std::filesystem::path techniquePath =
      std::filesystem::path(CONTAINER_SOURCE_DIR) / "src" / "renderer" /
      "DeferredRasterTechnique.cpp";
  const std::string source = readRepoTextFile(techniquePath);

  ASSERT_TRUE(contains(source, "DeferredRasterTechnique::buildFrameGraph"));
  ASSERT_TRUE(contains(source, "RenderGraphBuilder graph"));
  ASSERT_TRUE(contains(source, "graph.addPass("));
  EXPECT_FALSE(contains(source, "recorder.graph_.addPass("));
  EXPECT_FALSE(contains(source, "RayTracing"));
  EXPECT_FALSE(contains(source, "PathTracing"));
  EXPECT_FALSE(contains(source, "Splat"));
  EXPECT_FALSE(contains(source, "RadianceField"));
  EXPECT_FALSE(contains(source, "NeRF"));
}
