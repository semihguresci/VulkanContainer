#pragma once

#include "Container/geometry/Model.h"
#include "Container/scene/MeshSceneProviderBuilder.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace container::scene {

struct MeshSceneProviderMaterialFacts {
  bool transparent{false};
  bool doubleSided{false};
};

using MeshSceneProviderMaterialResolver =
    std::function<MeshSceneProviderMaterialFacts(uint32_t)>;

struct MeshSceneProviderAssetAdapterInput {
  std::span<const container::geometry::PrimitiveRange> primitiveRanges{};
  std::size_t materialCount{0};
  std::size_t instanceCount{0};
  SceneProviderBounds bounds{};
  MeshSceneProviderMaterialResolver materialFactsAt{};
};

[[nodiscard]] inline std::vector<MeshSceneProviderPrimitive>
meshSceneProviderPrimitivesFromImport(
    std::span<const container::geometry::PrimitiveRange> primitiveRanges) {
  std::vector<MeshSceneProviderPrimitive> primitives;
  primitives.reserve(primitiveRanges.size());

  for (const container::geometry::PrimitiveRange& primitive : primitiveRanges) {
    primitives.push_back(MeshSceneProviderPrimitive{
        .firstIndex = primitive.firstIndex,
        .indexCount = primitive.indexCount,
        .materialIndex = primitive.materialIndex,
        .doubleSided = primitive.disableBackfaceCulling,
    });
  }

  return primitives;
}

[[nodiscard]] inline std::vector<MeshSceneMaterialProperties>
meshSceneMaterialPropertiesFromImport(
    std::size_t materialCount,
    const MeshSceneProviderMaterialResolver& materialFactsAt) {
  std::vector<MeshSceneMaterialProperties> materials;
  materials.reserve(materialCount);

  for (std::size_t materialIndex = 0; materialIndex < materialCount;
       ++materialIndex) {
    const MeshSceneProviderMaterialFacts facts =
        materialFactsAt
            ? materialFactsAt(static_cast<uint32_t>(materialIndex))
            : MeshSceneProviderMaterialFacts{};
    materials.push_back(MeshSceneMaterialProperties{
        .transparent = facts.transparent,
        .doubleSided = facts.doubleSided,
    });
  }

  return materials;
}

[[nodiscard]] inline MeshSceneAsset buildMeshSceneProviderAsset(
    const MeshSceneProviderAssetAdapterInput& input) {
  const std::vector<MeshSceneProviderPrimitive> primitives =
      meshSceneProviderPrimitivesFromImport(input.primitiveRanges);
  const std::vector<MeshSceneMaterialProperties> materials =
      meshSceneMaterialPropertiesFromImport(input.materialCount,
                                            input.materialFactsAt);

  return buildMeshSceneAsset({
      .primitives = primitives,
      .materials = materials,
      .materialCount = input.materialCount,
      .instanceCount = input.instanceCount,
      .bounds = input.bounds,
  });
}

}  // namespace container::scene
