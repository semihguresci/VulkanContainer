#pragma once

#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/utility/SceneData.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace container::renderer {

struct ShadowCascadeSceneDataView {
  const std::vector<container::gpu::ObjectData> *objectData{nullptr};
  uint64_t objectDataRevision{0};
};

struct ShadowCascadeSurfaceDrawLists {
  const std::vector<DrawCommand> *singleSided{nullptr};
  const std::vector<DrawCommand> *windingFlipped{nullptr};
  const std::vector<DrawCommand> *doubleSided{nullptr};
  bool cpuFallbackAllowed{true};
};

struct ShadowCascadeDrawPlannerInputs {
  ShadowCascadeSceneDataView scene{};
  ShadowCascadeSceneDataView bimScene{};
  ShadowCascadeSurfaceDrawLists sceneDraws{};
  std::array<ShadowCascadeSurfaceDrawLists, container::gpu::kShadowCascadeCount>
      bimDraws{};
  uint32_t bimDrawListCount{0};
  std::array<bool, container::gpu::kShadowCascadeCount>
      sceneSingleSidedUsesGpuCull{};
  std::array<bool, container::gpu::kShadowCascadeCount> shadowPassActive{};
  std::array<bool, container::gpu::kShadowCascadeCount> shadowCullPassActive{};
  bool hasBimShadowGeometry{false};
  bool useGpuShadowCull{false};
  const void *shadowManagerIdentity{nullptr};
  const container::gpu::ShadowData *shadowData{nullptr};
  std::function<bool(uint32_t, const glm::vec4 &)> cascadeIntersectsSphere{};
};

struct ShadowCascadeDrawPlan {
  std::array<std::vector<DrawCommand>, container::gpu::kShadowCascadeCount>
      sceneSingleSided{};
  std::array<std::vector<DrawCommand>, container::gpu::kShadowCascadeCount>
      sceneWindingFlipped{};
  std::array<std::vector<DrawCommand>, container::gpu::kShadowCascadeCount>
      sceneDoubleSided{};
  std::array<std::vector<DrawCommand>, container::gpu::kShadowCascadeCount>
      bimSingleSided{};
  std::array<std::vector<DrawCommand>, container::gpu::kShadowCascadeCount>
      bimWindingFlipped{};
  std::array<std::vector<DrawCommand>, container::gpu::kShadowCascadeCount>
      bimDoubleSided{};
  uint64_t signature{0};

  [[nodiscard]] size_t cpuCommandCount(uint32_t cascadeIndex,
                                       bool includeSceneSingleSided) const;
};

class ShadowCascadeDrawPlanner {
public:
  explicit ShadowCascadeDrawPlanner(ShadowCascadeDrawPlannerInputs inputs);

  [[nodiscard]] uint64_t signature() const;
  [[nodiscard]] ShadowCascadeDrawPlan build() const;

private:
  ShadowCascadeDrawPlannerInputs inputs_{};
};

[[nodiscard]] uint64_t
computeShadowCascadeDrawSignature(const ShadowCascadeDrawPlannerInputs &inputs);

[[nodiscard]] ShadowCascadeDrawPlan
buildShadowCascadeDrawPlan(const ShadowCascadeDrawPlannerInputs &inputs);

} // namespace container::renderer
