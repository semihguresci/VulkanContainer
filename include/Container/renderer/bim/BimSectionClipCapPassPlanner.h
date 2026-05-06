#pragma once

#include <array>
#include <cstdint>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vector>

namespace container::renderer {

struct DrawCommand;

enum class BimSectionClipCapPassPipeline : uint32_t {
  Fill = 0,
  Hatch = 1,
};

struct BimSectionClipCapPassInputs {
  bool enabled{false};
  bool fillEnabled{false};
  bool hatchEnabled{false};
  bool geometryReady{false};
  bool wireframeLayoutReady{false};
  bool wireframePushConstantsReady{false};
  bool wideLinesSupported{false};
  bool fillPipelineReady{false};
  bool hatchPipelineReady{false};
  glm::vec4 fillColor{0.06f, 0.08f, 0.10f, 0.82f};
  glm::vec4 hatchColor{0.85f, 0.72f, 0.32f, 0.95f};
  float hatchLineWidth{1.0f};
  const std::vector<DrawCommand> *fillDrawCommands{nullptr};
  const std::vector<DrawCommand> *hatchDrawCommands{nullptr};
};

struct BimSectionClipCapPassRoute {
  BimSectionClipCapPassPipeline pipeline{BimSectionClipCapPassPipeline::Fill};
  const std::vector<DrawCommand> *commands{nullptr};
  glm::vec3 color{1.0f};
  float opacity{1.0f};
  float drawLineWidth{1.0f};
  float rasterLineWidth{1.0f};
  bool rasterLineWidthApplies{false};
  bool resetRasterLineWidth{false};
};

struct BimSectionClipCapPassPlan {
  bool active{false};
  std::array<BimSectionClipCapPassRoute, 2> routes{};
  uint32_t routeCount{0};
};

class BimSectionClipCapPassPlanner {
public:
  explicit BimSectionClipCapPassPlanner(BimSectionClipCapPassInputs inputs);

  [[nodiscard]] BimSectionClipCapPassPlan build() const;

private:
  BimSectionClipCapPassInputs inputs_{};
};

[[nodiscard]] float sanitizeBimSectionClipCapLineWidth(float lineWidth);
[[nodiscard]] float rasterBimSectionClipCapLineWidth(float lineWidth,
                                                     bool wideLinesSupported);

[[nodiscard]] BimSectionClipCapPassPlan
buildBimSectionClipCapPassPlan(const BimSectionClipCapPassInputs &inputs);

} // namespace container::renderer
