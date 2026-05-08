#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace container::renderer {

struct DrawCommand;

enum class SceneTransparentDrawPipeline : uint32_t {
  Primary = 0,
  FrontCull = 1,
  NoCull = 2,
};

struct SceneTransparentDrawLists {
  const std::vector<DrawCommand> *aggregate{nullptr};
  const std::vector<DrawCommand> *singleSided{nullptr};
  const std::vector<DrawCommand> *windingFlipped{nullptr};
  const std::vector<DrawCommand> *doubleSided{nullptr};
};

struct SceneTransparentDrawRoute {
  SceneTransparentDrawPipeline pipeline{SceneTransparentDrawPipeline::Primary};
  const std::vector<DrawCommand> *commands{nullptr};
};

struct SceneTransparentDrawPlan {
  std::array<SceneTransparentDrawRoute, 3> routes{};
  uint32_t routeCount{0};
};

class SceneTransparentDrawPlanner {
public:
  explicit SceneTransparentDrawPlanner(SceneTransparentDrawLists draws);

  [[nodiscard]] SceneTransparentDrawPlan build() const;

private:
  SceneTransparentDrawLists draws_{};
};

[[nodiscard]] SceneTransparentDrawPlan
buildSceneTransparentDrawPlan(const SceneTransparentDrawLists &draws);

} // namespace container::renderer
