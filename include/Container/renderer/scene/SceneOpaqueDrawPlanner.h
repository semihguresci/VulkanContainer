#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace container::renderer {

struct DrawCommand;

enum class SceneOpaqueDrawRouteKind : uint32_t {
  GpuIndirectSingleSided = 0,
  CpuSingleSided = 1,
  CpuWindingFlipped = 2,
  CpuDoubleSided = 3,
};

enum class SceneOpaqueDrawPipeline : uint32_t {
  Primary = 0,
  FrontCull = 1,
  NoCull = 2,
};

struct SceneOpaqueDrawLists {
  const std::vector<DrawCommand> *singleSided{nullptr};
  const std::vector<DrawCommand> *windingFlipped{nullptr};
  const std::vector<DrawCommand> *doubleSided{nullptr};
};

struct SceneOpaqueDrawInputs {
  bool gpuIndirectAvailable{false};
  SceneOpaqueDrawLists draws{};
};

struct SceneOpaqueDrawRoute {
  SceneOpaqueDrawRouteKind kind{SceneOpaqueDrawRouteKind::CpuSingleSided};
  SceneOpaqueDrawPipeline pipeline{SceneOpaqueDrawPipeline::Primary};
  const std::vector<DrawCommand> *commands{nullptr};
};

struct SceneOpaqueDrawPlan {
  bool useGpuIndirectSingleSided{false};
  SceneOpaqueDrawRoute gpuIndirectRoute{
      .kind = SceneOpaqueDrawRouteKind::GpuIndirectSingleSided,
      .pipeline = SceneOpaqueDrawPipeline::Primary,
      .commands = nullptr};
  std::array<SceneOpaqueDrawRoute, 3> cpuRoutes{};
  uint32_t cpuRouteCount{0};
};

class SceneOpaqueDrawPlanner {
public:
  explicit SceneOpaqueDrawPlanner(SceneOpaqueDrawInputs inputs);

  [[nodiscard]] SceneOpaqueDrawPlan build() const;

private:
  SceneOpaqueDrawInputs inputs_{};
};

[[nodiscard]] SceneOpaqueDrawPlan
buildSceneOpaqueDrawPlan(const SceneOpaqueDrawInputs &inputs);

} // namespace container::renderer
