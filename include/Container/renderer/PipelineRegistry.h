#pragma once

#include "Container/renderer/RenderTechnique.h"

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

class PipelineRegistry {
 public:
  void registerRecipe(PipelineRecipe recipe);

  [[nodiscard]] const PipelineRecipe* find(
      const TechniquePipelineKey& key) const;
  [[nodiscard]] bool contains(const TechniquePipelineKey& key) const {
    return find(key) != nullptr;
  }
  [[nodiscard]] std::vector<const PipelineRecipe*> recipesForTechnique(
      RenderTechniqueId technique) const;
  [[nodiscard]] std::size_t size() const { return recipes_.size(); }

  void clearTechnique(RenderTechniqueId technique);
  void clear();

 private:
  [[nodiscard]] std::size_t findIndex(const TechniquePipelineKey& key) const;

  std::vector<PipelineRecipe> recipes_{};
};

}  // namespace container::renderer
