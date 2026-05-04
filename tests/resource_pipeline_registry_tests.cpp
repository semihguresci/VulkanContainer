#include "Container/renderer/FrameResourceRegistry.h"
#include "Container/renderer/PipelineRegistry.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using container::renderer::FrameBufferDesc;
using container::renderer::FrameImageDesc;
using container::renderer::FrameResourceKind;
using container::renderer::FrameResourceLifetime;
using container::renderer::FrameResourceRegistry;
using container::renderer::PipelineRecipe;
using container::renderer::PipelineRecipeKind;
using container::renderer::PipelineRegistry;
using container::renderer::RenderTechniqueId;
using container::renderer::TechniquePipelineKey;
using container::renderer::TechniqueResourceKey;

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

TEST(FrameResourceRegistryTests, RegistersTechniqueScopedResources) {
  FrameResourceRegistry registry;

  registry.registerImage(RenderTechniqueId::DeferredRaster, "scene-color",
                         FrameImageDesc{.format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                        .extent = {1920, 1080, 1},
                                        .usage = VK_IMAGE_USAGE_SAMPLED_BIT});
  registry.registerImage(RenderTechniqueId::PathTracing, "scene-color",
                         FrameImageDesc{.format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                        .extent = {1920, 1080, 1},
                                        .usage = VK_IMAGE_USAGE_STORAGE_BIT});
  registry.registerBuffer(RenderTechniqueId::PathTracing, "accumulation-state",
                          FrameBufferDesc{
                              .size = 4096,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT},
                          FrameResourceLifetime::Persistent);

  EXPECT_EQ(registry.size(), 3u);
  const auto* deferredSceneColor =
      registry.find(TechniqueResourceKey{RenderTechniqueId::DeferredRaster,
                                         "scene-color"});
  ASSERT_NE(deferredSceneColor, nullptr);
  EXPECT_EQ(deferredSceneColor->kind, FrameResourceKind::Image);
  EXPECT_EQ(deferredSceneColor->image.format,
            VK_FORMAT_R16G16B16A16_SFLOAT);

  const auto pathResources =
      registry.resourcesForTechnique(RenderTechniqueId::PathTracing);
  EXPECT_EQ(pathResources.size(), 2u);
}

TEST(FrameResourceRegistryTests, RejectsInvalidAndDuplicateResourceKeys) {
  FrameResourceRegistry registry;
  registry.registerExternal(RenderTechniqueId::DeferredRaster, "swapchain");

  EXPECT_THROW(registry.registerExternal(RenderTechniqueId::DeferredRaster,
                                         "swapchain"),
               std::invalid_argument);
  EXPECT_THROW(registry.registerExternal(RenderTechniqueId::DeferredRaster, ""),
               std::invalid_argument);
}

TEST(FrameResourceRegistryTests, ClearsOnlyRequestedTechniqueResources) {
  FrameResourceRegistry registry;
  registry.registerExternal(RenderTechniqueId::DeferredRaster, "swapchain");
  registry.registerExternal(RenderTechniqueId::RayTracing, "tlas");

  registry.clearTechnique(RenderTechniqueId::RayTracing);

  EXPECT_TRUE(registry.contains(TechniqueResourceKey{
      RenderTechniqueId::DeferredRaster, "swapchain"}));
  EXPECT_FALSE(registry.contains(
      TechniqueResourceKey{RenderTechniqueId::RayTracing, "tlas"}));
}

TEST(PipelineRegistryTests, RegistersTechniqueScopedRecipes) {
  PipelineRegistry registry;

  registry.registerRecipe(PipelineRecipe{
      .key = {RenderTechniqueId::DeferredRaster, "gbuffer"},
      .kind = PipelineRecipeKind::Graphics,
      .shaderStages = {"gbuffer.slang"},
      .layoutName = "scene"});
  registry.registerRecipe(PipelineRecipe{
      .key = {RenderTechniqueId::RayTracing, "primary-rays"},
      .kind = PipelineRecipeKind::RayTracing,
      .shaderStages = {"raygen.slang", "closesthit.slang", "miss.slang"},
      .layoutName = "ray-tracing"});

  const auto* rayRecipe = registry.find(
      TechniquePipelineKey{RenderTechniqueId::RayTracing, "primary-rays"});
  ASSERT_NE(rayRecipe, nullptr);
  EXPECT_EQ(rayRecipe->kind, PipelineRecipeKind::RayTracing);
  EXPECT_EQ(rayRecipe->shaderStages.size(), 3u);
  EXPECT_EQ(registry.recipesForTechnique(RenderTechniqueId::DeferredRaster)
                .size(),
            1u);
}

TEST(PipelineRegistryTests, RejectsInvalidAndDuplicateRecipeKeys) {
  PipelineRegistry registry;
  registry.registerRecipe(PipelineRecipe{
      .key = {RenderTechniqueId::DeferredRaster, "post-process"},
      .kind = PipelineRecipeKind::Graphics});

  EXPECT_THROW(
      registry.registerRecipe(PipelineRecipe{
          .key = {RenderTechniqueId::DeferredRaster, "post-process"},
          .kind = PipelineRecipeKind::Graphics}),
      std::invalid_argument);
  EXPECT_THROW(registry.registerRecipe(PipelineRecipe{
                   .key = {RenderTechniqueId::DeferredRaster, ""},
                   .kind = PipelineRecipeKind::Compute}),
               std::invalid_argument);
}

TEST(TechniqueRegistryGuardrails,
     DeferredCompatibilityStructsStayAlgorithmNeutral) {
  const std::string frameResources =
      readRepoTextFile("include/Container/renderer/FrameResources.h");
  const std::string pipelineTypes =
      readRepoTextFile("include/Container/renderer/PipelineTypes.h");

  for (const std::string& forbidden :
       {"RayTracing", "PathTracing", "Splat", "RadianceField", "NeRF",
        "Gaussian"}) {
    EXPECT_FALSE(contains(frameResources, forbidden))
        << "FrameResources should not grow technique-specific " << forbidden
        << " fields; register them through FrameResourceRegistry.";
    EXPECT_FALSE(contains(pipelineTypes, forbidden))
        << "PipelineTypes should not grow technique-specific " << forbidden
        << " fields; register them through PipelineRegistry.";
  }
}
