#include "Container/utility/InputManager.h"

#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

namespace {

TEST(InputManagerTests, TracksKeyTransitionsSeparatelyFromHeldState) {
  container::window::InputManager input;

  input.handleKey(GLFW_KEY_W, GLFW_PRESS);
  auto snapshot = input.frameSnapshot();
  EXPECT_TRUE(snapshot.keyDown(GLFW_KEY_W));
  EXPECT_TRUE(snapshot.keyPressed(GLFW_KEY_W));
  EXPECT_FALSE(snapshot.keyReleased(GLFW_KEY_W));

  input.endFrame();
  snapshot = input.frameSnapshot();
  EXPECT_TRUE(snapshot.keyDown(GLFW_KEY_W));
  EXPECT_FALSE(snapshot.keyPressed(GLFW_KEY_W));

  input.handleKey(GLFW_KEY_W, GLFW_RELEASE);
  snapshot = input.frameSnapshot();
  EXPECT_FALSE(snapshot.keyDown(GLFW_KEY_W));
  EXPECT_FALSE(snapshot.keyPressed(GLFW_KEY_W));
  EXPECT_TRUE(snapshot.keyReleased(GLFW_KEY_W));
}

TEST(InputManagerTests, IgnoresRepeatAsPressedEdge) {
  container::window::InputManager input;

  input.handleKey(GLFW_KEY_F, GLFW_REPEAT);
  const auto snapshot = input.frameSnapshot();
  EXPECT_TRUE(snapshot.keyDown(GLFW_KEY_F));
  EXPECT_FALSE(snapshot.keyPressed(GLFW_KEY_F));
}

TEST(InputManagerTests, TracksMouseButtonTransitionsAndScroll) {
  container::window::InputManager input;

  input.handleMouseButton(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
  input.handleScroll(0.0, 2.0);

  auto snapshot = input.frameSnapshot();
  EXPECT_TRUE(snapshot.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT));
  EXPECT_TRUE(snapshot.mouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT));
  EXPECT_DOUBLE_EQ(snapshot.scrollDeltaY, 2.0);

  input.endFrame();
  snapshot = input.frameSnapshot();
  EXPECT_TRUE(snapshot.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT));
  EXPECT_FALSE(snapshot.mouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT));
  EXPECT_DOUBLE_EQ(snapshot.scrollDeltaY, 0.0);
}

TEST(InputManagerTests, FocusLossClearsHeldInputAndLookMode) {
  container::window::InputManager input;

  input.handleKey(GLFW_KEY_W, GLFW_PRESS);
  input.handleMouseButton(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
  input.handleScroll(0.0, 1.0);
  input.setLookMode(true);

  input.handleWindowFocus(false);

  const auto snapshot = input.frameSnapshot();
  EXPECT_FALSE(snapshot.focused);
  EXPECT_FALSE(snapshot.lookModeActive);
  EXPECT_FALSE(snapshot.keyDown(GLFW_KEY_W));
  EXPECT_FALSE(snapshot.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT));
  EXPECT_DOUBLE_EQ(snapshot.scrollDeltaY, 0.0);
}

}  // namespace
