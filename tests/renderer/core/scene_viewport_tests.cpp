#include "Container/renderer/scene/SceneViewport.h"

#include <gtest/gtest.h>

namespace {

using container::renderer::buildSceneViewportScissor;

TEST(SceneViewportTests, BuildsNegativeHeightViewportForSceneConvention) {
  const auto state = buildSceneViewportScissor({1920u, 1080u});

  EXPECT_FLOAT_EQ(state.viewport.x, 0.0f);
  EXPECT_FLOAT_EQ(state.viewport.y, 1080.0f);
  EXPECT_FLOAT_EQ(state.viewport.width, 1920.0f);
  EXPECT_FLOAT_EQ(state.viewport.height, -1080.0f);
  EXPECT_FLOAT_EQ(state.viewport.minDepth, 0.0f);
  EXPECT_FLOAT_EQ(state.viewport.maxDepth, 1.0f);
  EXPECT_EQ(state.scissor.offset.x, 0);
  EXPECT_EQ(state.scissor.offset.y, 0);
  EXPECT_EQ(state.scissor.extent.width, 1920u);
  EXPECT_EQ(state.scissor.extent.height, 1080u);
}

}  // namespace
