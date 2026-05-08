#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace container::renderer {

struct DrawCommand;

enum class ShadowPassPipeline : uint32_t {
  Primary = 0,
  FrontCull = 1,
  NoCull = 2,
};

enum class ShadowPassBimGpuSlot : uint32_t {
  OpaqueSingleSided = 0,
  OpaqueWindingFlipped = 1,
  OpaqueDoubleSided = 2,
};

struct ShadowPassDrawLists {
  const std::vector<DrawCommand> *singleSided{nullptr};
  const std::vector<DrawCommand> *windingFlipped{nullptr};
  const std::vector<DrawCommand> *doubleSided{nullptr};
};

struct ShadowPassCpuRoute {
  ShadowPassPipeline pipeline{ShadowPassPipeline::Primary};
  const std::vector<DrawCommand> *commands{nullptr};
};

struct ShadowPassSceneGpuRoute {
  bool active{false};
  ShadowPassPipeline pipeline{ShadowPassPipeline::Primary};
};

struct ShadowPassBimGpuRoute {
  ShadowPassBimGpuSlot slot{ShadowPassBimGpuSlot::OpaqueSingleSided};
  ShadowPassPipeline pipeline{ShadowPassPipeline::Primary};
};

struct ShadowPassDrawInputs {
  bool sceneGeometryReady{false};
  bool bimGeometryReady{false};
  bool sceneGpuCullActive{false};
  bool bimGpuFilteredMeshActive{false};
  ShadowPassDrawLists sceneDraws{};
  ShadowPassDrawLists bimDraws{};
};

struct ShadowPassDrawPlan {
  ShadowPassSceneGpuRoute sceneGpuRoute{};
  std::array<ShadowPassCpuRoute, 3> sceneCpuRoutes{};
  uint32_t sceneCpuRouteCount{0};
  std::array<ShadowPassBimGpuRoute, 3> bimGpuRoutes{};
  uint32_t bimGpuRouteCount{0};
  std::array<ShadowPassCpuRoute, 3> bimCpuRoutes{};
  uint32_t bimCpuRouteCount{0};
};

class ShadowPassDrawPlanner {
public:
  explicit ShadowPassDrawPlanner(ShadowPassDrawInputs inputs);

  [[nodiscard]] ShadowPassDrawPlan build() const;

private:
  ShadowPassDrawInputs inputs_{};
};

[[nodiscard]] ShadowPassDrawPlan
buildShadowPassDrawPlan(const ShadowPassDrawInputs &inputs);

} // namespace container::renderer
