#include "Container/renderer/PipelineRegistry.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace container::renderer {

namespace {

void validateKey(const TechniquePipelineKey& key) {
  if (key.name.empty()) {
    throw std::invalid_argument("pipeline recipe name must not be empty");
  }
}

}  // namespace

void PipelineRegistry::registerRecipe(PipelineRecipe recipe) {
  validateKey(recipe.key);
  if (contains(recipe.key)) {
    throw std::invalid_argument("pipeline recipe is already registered");
  }
  recipes_.push_back(std::move(recipe));
}

const PipelineRecipe* PipelineRegistry::find(
    const TechniquePipelineKey& key) const {
  const std::size_t index = findIndex(key);
  return index == recipes_.size() ? nullptr : &recipes_[index];
}

std::vector<const PipelineRecipe*> PipelineRegistry::recipesForTechnique(
    RenderTechniqueId technique) const {
  std::vector<const PipelineRecipe*> matches;
  for (const PipelineRecipe& recipe : recipes_) {
    if (recipe.key.technique == technique) {
      matches.push_back(&recipe);
    }
  }
  return matches;
}

void PipelineRegistry::clearTechnique(RenderTechniqueId technique) {
  std::erase_if(recipes_, [technique](const PipelineRecipe& recipe) {
    return recipe.key.technique == technique;
  });
}

void PipelineRegistry::clear() { recipes_.clear(); }

std::size_t PipelineRegistry::findIndex(
    const TechniquePipelineKey& key) const {
  const auto it =
      std::find_if(recipes_.begin(), recipes_.end(),
                   [&key](const PipelineRecipe& recipe) {
                     return recipe.key == key;
                   });
  if (it == recipes_.end()) {
    return recipes_.size();
  }
  return static_cast<std::size_t>(std::distance(recipes_.begin(), it));
}

}  // namespace container::renderer
