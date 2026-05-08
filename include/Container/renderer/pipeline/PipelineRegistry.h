#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/core/RenderTechnique.h"

#include <cstddef>
#include <string>
#include <vector>

namespace container::renderer {

struct TechniquePipelineKey {
  RenderTechniqueId technique{RenderTechniqueId::DeferredRaster};
  std::string name{};

  [[nodiscard]] bool operator==(const TechniquePipelineKey& other) const {
    return technique == other.technique && name == other.name;
  }
};

enum class PipelineRecipeKind {
  Graphics,
  Compute,
  RayTracing,
};

struct PipelineRecipe {
  TechniquePipelineKey key{};
  PipelineRecipeKind kind{PipelineRecipeKind::Graphics};
  std::vector<std::string> shaderStages{};
  std::string layoutName{};
};

struct RegisteredPipelineHandle {
  TechniquePipelineKey key{};
  VkPipeline pipeline{VK_NULL_HANDLE};
};

struct RegisteredPipelineLayout {
  TechniquePipelineKey key{};
  VkPipelineLayout layout{VK_NULL_HANDLE};
};

class PipelineRegistry {
 public:
  void registerRecipe(PipelineRecipe recipe);
  void registerHandle(RegisteredPipelineHandle handle);
  void registerLayout(RegisteredPipelineLayout layout);

  [[nodiscard]] const PipelineRecipe* find(
      const TechniquePipelineKey& key) const;
  [[nodiscard]] const RegisteredPipelineHandle* findHandle(
      const TechniquePipelineKey& key) const;
  [[nodiscard]] const RegisteredPipelineLayout* findLayout(
      const TechniquePipelineKey& key) const;
  [[nodiscard]] VkPipeline pipelineHandle(
      const TechniquePipelineKey& key) const;
  [[nodiscard]] VkPipelineLayout pipelineLayout(
      const TechniquePipelineKey& key) const;
  [[nodiscard]] bool contains(const TechniquePipelineKey& key) const {
    return find(key) != nullptr;
  }
  [[nodiscard]] std::vector<const PipelineRecipe*> recipesForTechnique(
      RenderTechniqueId technique) const;
  [[nodiscard]] std::vector<const RegisteredPipelineHandle*>
  pipelineHandlesForTechnique(RenderTechniqueId technique) const;
  [[nodiscard]] std::vector<const RegisteredPipelineLayout*>
  pipelineLayoutsForTechnique(RenderTechniqueId technique) const;
  [[nodiscard]] std::size_t size() const { return recipes_.size(); }
  [[nodiscard]] std::size_t handleCount() const { return handles_.size(); }
  [[nodiscard]] std::size_t layoutCount() const { return layouts_.size(); }

  void clearHandles();
  void clearLayouts();
  void clearTechnique(RenderTechniqueId technique);
  void clear();

 private:
  [[nodiscard]] std::size_t findIndex(const TechniquePipelineKey& key) const;
  [[nodiscard]] std::size_t findHandleIndex(
      const TechniquePipelineKey& key) const;
  [[nodiscard]] std::size_t findLayoutIndex(
      const TechniquePipelineKey& key) const;

  std::vector<PipelineRecipe> recipes_{};
  std::vector<RegisteredPipelineHandle> handles_{};
  std::vector<RegisteredPipelineLayout> layouts_{};
};

}  // namespace container::renderer
