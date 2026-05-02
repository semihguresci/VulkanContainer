#include "Container/renderer/RenderSurfaceInteractionController.h"

#include "Container/common/CommonGLFW.h"
#include "Container/renderer/CameraController.h"
#include "Container/renderer/DebugRenderState.h"
#include "Container/utility/InputManager.h"

namespace container::renderer {

namespace {

bool anyControlKeyDown(const container::window::InputFrameSnapshot& input) {
  return input.keyDown(GLFW_KEY_LEFT_CONTROL) ||
         input.keyDown(GLFW_KEY_RIGHT_CONTROL);
}

bool anyShiftKeyDown(const container::window::InputFrameSnapshot& input) {
  return input.keyDown(GLFW_KEY_LEFT_SHIFT) ||
         input.keyDown(GLFW_KEY_RIGHT_SHIFT);
}

bool anyAltKeyDown(const container::window::InputFrameSnapshot& input) {
  return input.keyDown(GLFW_KEY_LEFT_ALT) || input.keyDown(GLFW_KEY_RIGHT_ALT);
}

bool anyPointerButtonDown(const container::window::InputFrameSnapshot& input) {
  return input.mouseButtonDown(GLFW_MOUSE_BUTTON_LEFT) ||
         input.mouseButtonDown(GLFW_MOUSE_BUTTON_MIDDLE) ||
         input.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT);
}

bool escapePressed(const container::window::InputFrameSnapshot& input) {
  return input.keyPressed(GLFW_KEY_ESCAPE);
}

float wheelSpeedScale(const container::window::InputFrameSnapshot& input) {
  if (anyControlKeyDown(input)) {
    return 0.25f;
  }
  if (anyShiftKeyDown(input)) {
    return 3.0f;
  }
  return 1.0f;
}

float transformDragSpeedScale(
    const container::window::InputFrameSnapshot& input) {
  if (anyControlKeyDown(input)) {
    return 0.25f;
  }
  if (anyShiftKeyDown(input)) {
    return 3.0f;
  }
  return 1.0f;
}

}  // namespace

void RenderSurfaceInteractionController::setTool(
    container::ui::ViewportTool tool) {
  state_.tool = tool;
  state_.transformAxis = container::ui::TransformAxis::Free;
}

void RenderSurfaceInteractionController::setTransformSpace(
    container::ui::TransformSpace transformSpace) {
  state_.transformSpace = transformSpace;
}

void RenderSurfaceInteractionController::setTransformAxis(
    container::ui::TransformAxis transformAxis) {
  state_.transformAxis = transformAxis;
}

void RenderSurfaceInteractionController::setGesture(
    container::ui::ViewportGesture gesture) {
  state_.gesture = gesture;
}

void RenderSurfaceInteractionController::process(Context context) {
  const auto input = context.inputManager.frameSnapshot();

  const bool cancelPressed = escapePressed(input);
  const bool hadActiveGesture =
      state_.gesture != container::ui::ViewportGesture::None;
  if (!input.focused) {
    context.inputManager.setLookMode(false);
    setGesture(container::ui::ViewportGesture::None);
    activePointerButton_ = -1;
  } else if (cancelPressed &&
             (hadActiveGesture || !context.guiCapturingKeyboard)) {
    context.inputManager.setLookMode(false);
    setGesture(container::ui::ViewportGesture::None);
    activePointerButton_ = -1;
    if (hadActiveGesture) {
      if (context.setStatusMessage) {
        context.setStatusMessage("Viewport gesture cancelled");
      }
    } else if (state_.tool != container::ui::ViewportTool::Select) {
      setTool(container::ui::ViewportTool::Select);
      if (context.setStatusMessage) {
        context.setStatusMessage("Viewport tool: Select");
      }
    } else if (context.clearSelection && context.hasSelection) {
      context.clearSelection();
    }
  }

  const bool viewportOwnsGesture =
      state_.gesture != container::ui::ViewportGesture::None;
  const bool shortcutsAllowed =
      viewportOwnsGesture || !context.guiCapturingKeyboard;
  handleDebugShortcuts(input, context, shortcutsAllowed);
  handleToolShortcuts(input, context, shortcutsAllowed);

  handleLookOwnership(input, context);

  const bool viewportInputAllowed =
      input.focused &&
      (state_.gesture != container::ui::ViewportGesture::None ||
       (!context.guiCapturingMouse && !context.guiCapturingKeyboard));
  handleViewportActions(input, context, viewportInputAllowed);

  if (state_.gesture == container::ui::ViewportGesture::FlyLook) {
    (void)context.inputManager.update(context.deltaTime);
  } else {
    context.inputManager.endFrame();
  }
}

void RenderSurfaceInteractionController::handleDebugShortcuts(
    const container::window::InputFrameSnapshot& input, Context& context,
    bool shortcutsAllowed) {
  if (!shortcutsAllowed) {
    return;
  }

  if (input.keyPressed(GLFW_KEY_F6)) {
    context.debugState.directionalOnly = !context.debugState.directionalOnly;
    if (context.setStatusMessage) {
      context.setStatusMessage(
          context.debugState.directionalOnly
              ? "Debug: directional-only enabled"
              : "Debug: directional-only disabled");
    }
  }

  if (input.keyPressed(GLFW_KEY_F7)) {
    context.debugState.visualizePointLightStencil =
        !context.debugState.visualizePointLightStencil;
    if (context.setStatusMessage) {
      context.setStatusMessage(
          context.debugState.visualizePointLightStencil
              ? "Debug: point-light stencil visualization enabled"
              : "Debug: point-light stencil visualization disabled");
    }
  }

  if (input.keyPressed(GLFW_KEY_F8)) {
    context.debugState.freezeCulling = !context.debugState.freezeCulling;
    if (!context.debugState.freezeCulling && context.onCullingUnfrozen) {
      context.onCullingUnfrozen();
    }
    if (context.setStatusMessage) {
      context.setStatusMessage(
          context.debugState.freezeCulling
              ? "Debug: culling camera frozen (F8 to unfreeze)"
              : "Debug: culling camera unfrozen");
    }
  }
}

void RenderSurfaceInteractionController::handleToolShortcuts(
    const container::window::InputFrameSnapshot& input, Context& context,
    bool shortcutsAllowed) {
  if (!shortcutsAllowed ||
      state_.gesture != container::ui::ViewportGesture::None ||
      input.mouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)) {
    return;
  }

  auto setToolWithStatus = [&](container::ui::ViewportTool tool,
                               const char* label) {
    setTool(tool);
    if (context.setStatusMessage) {
      context.setStatusMessage(std::string("Viewport tool: ") + label);
    }
  };

  if (input.keyPressed(GLFW_KEY_W)) {
    setToolWithStatus(container::ui::ViewportTool::Translate, "Move");
  } else if (input.keyPressed(GLFW_KEY_E)) {
    setToolWithStatus(container::ui::ViewportTool::Rotate, "Rotate");
  } else if (input.keyPressed(GLFW_KEY_R)) {
    setToolWithStatus(container::ui::ViewportTool::Scale, "Scale");
  }

  if (input.keyPressed(GLFW_KEY_L)) {
    setTransformSpace(state_.transformSpace ==
                              container::ui::TransformSpace::Local
                          ? container::ui::TransformSpace::World
                          : container::ui::TransformSpace::Local);
    if (context.setStatusMessage) {
      context.setStatusMessage(
          state_.transformSpace == container::ui::TransformSpace::Local
              ? "Transform space: Local"
              : "Transform space: World");
    }
  }

  auto toggleAxisWithStatus = [&](container::ui::TransformAxis axis,
                                  const char* label) {
    const container::ui::TransformAxis nextAxis =
        state_.transformAxis == axis ? container::ui::TransformAxis::Free
                                     : axis;
    setTransformAxis(nextAxis);
    if (context.setStatusMessage) {
      context.setStatusMessage(
          std::string("Transform axis: ") +
          (nextAxis == container::ui::TransformAxis::Free ? "Free" : label));
    }
  };

  if (state_.tool != container::ui::ViewportTool::Select) {
    if (input.keyPressed(GLFW_KEY_X)) {
      toggleAxisWithStatus(container::ui::TransformAxis::X, "X");
    } else if (input.keyPressed(GLFW_KEY_Y)) {
      toggleAxisWithStatus(container::ui::TransformAxis::Y, "Y");
    } else if (input.keyPressed(GLFW_KEY_Z)) {
      toggleAxisWithStatus(container::ui::TransformAxis::Z, "Z");
    }
  }
}

void RenderSurfaceInteractionController::handleLookOwnership(
    const container::window::InputFrameSnapshot& input, Context& context) {
  if (state_.gesture != container::ui::ViewportGesture::None) {
    const bool gestureButtonReleased =
        activePointerButton_ >= 0 &&
        input.mouseButtonReleased(activePointerButton_);
    const bool gestureButtonStillDown =
        activePointerButton_ < 0 || input.mouseButtonDown(activePointerButton_);
    if (gestureButtonReleased || !gestureButtonStillDown || !input.focused) {
      context.inputManager.setLookMode(false);
      setGesture(container::ui::ViewportGesture::None);
      activePointerButton_ = -1;
    }
    return;
  }

  if (context.guiCapturingMouse) {
    return;
  }

  if (input.mouseButtonPressed(GLFW_MOUSE_BUTTON_MIDDLE)) {
    setGesture(anyShiftKeyDown(input) ? container::ui::ViewportGesture::Pan
                                      : container::ui::ViewportGesture::Orbit);
    activePointerButton_ = GLFW_MOUSE_BUTTON_MIDDLE;
    return;
  }

  if (input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT) && anyAltKeyDown(input)) {
    setGesture(container::ui::ViewportGesture::Orbit);
    activePointerButton_ = GLFW_MOUSE_BUTTON_LEFT;
    return;
  }

  if (input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT) &&
      state_.tool != container::ui::ViewportTool::Select &&
      context.selectedMeshNode != std::numeric_limits<uint32_t>::max() &&
      context.transformSelectedByDrag) {
    setGesture(container::ui::ViewportGesture::TransformDrag);
    activePointerButton_ = GLFW_MOUSE_BUTTON_LEFT;
    return;
  }

  if (input.mouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT) && anyAltKeyDown(input)) {
    setGesture(container::ui::ViewportGesture::Pan);
    activePointerButton_ = GLFW_MOUSE_BUTTON_RIGHT;
    return;
  }

  if (input.mouseButtonPressed(GLFW_MOUSE_BUTTON_RIGHT)) {
    context.inputManager.setLookMode(true);
    setGesture(container::ui::ViewportGesture::FlyLook);
    activePointerButton_ = GLFW_MOUSE_BUTTON_RIGHT;
  }
}

void RenderSurfaceInteractionController::handleViewportActions(
    const container::window::InputFrameSnapshot& input, Context& context,
    bool viewportInputAllowed) {
  if (!viewportInputAllowed) {
    if (context.clearHover) {
      context.clearHover();
    }
    return;
  }

  if (state_.gesture == container::ui::ViewportGesture::Orbit &&
      (input.cursorDeltaX != 0.0 || input.cursorDeltaY != 0.0)) {
    context.cameraController.orbit(
        context.selectedMeshNode, static_cast<float>(input.cursorDeltaX),
        static_cast<float>(input.cursorDeltaY),
        anyShiftKeyDown(input) ? 0.25f : 1.0f);
  } else if (state_.gesture == container::ui::ViewportGesture::Pan &&
             (input.cursorDeltaX != 0.0 || input.cursorDeltaY != 0.0)) {
    context.cameraController.pan(
        context.selectedMeshNode, static_cast<float>(input.cursorDeltaX),
        static_cast<float>(input.cursorDeltaY),
        anyShiftKeyDown(input) ? 0.25f : 1.0f);
  } else if (state_.gesture == container::ui::ViewportGesture::TransformDrag &&
             (input.cursorDeltaX != 0.0 || input.cursorDeltaY != 0.0) &&
             context.transformSelectedByDrag) {
    const double dragScale =
        static_cast<double>(transformDragSpeedScale(input));
    context.transformSelectedByDrag(state_.tool, state_.transformSpace,
                                    state_.transformAxis,
                                    input.cursorDeltaX * dragScale,
                                    input.cursorDeltaY * dragScale);
  }

  if (input.mouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT) &&
      state_.tool == container::ui::ViewportTool::Select &&
      !anyAltKeyDown(input) && context.selectAtCursor) {
    context.selectAtCursor(input.framebufferCursorX, input.framebufferCursorY);
  }

  if (state_.gesture == container::ui::ViewportGesture::None &&
      !anyPointerButtonDown(input) && context.hoverAtCursor) {
    context.hoverAtCursor(input.framebufferCursorX, input.framebufferCursorY);
  } else if (state_.gesture != container::ui::ViewportGesture::None &&
             context.clearHover) {
    context.clearHover();
  }

  if (input.keyPressed(GLFW_KEY_HOME)) {
    context.cameraController.resetCameraForScene();
    if (context.setStatusMessage) {
      context.setStatusMessage("Camera reset to scene bounds");
    }
  } else if (input.keyPressed(GLFW_KEY_F)) {
    context.cameraController.frameNodeOrScene(context.selectedMeshNode);
    if (context.setStatusMessage) {
      context.setStatusMessage("Camera framed selection");
    }
  }

  if (input.scrollDeltaY == 0.0) {
    return;
  }

  if (state_.gesture == container::ui::ViewportGesture::FlyLook) {
    context.cameraController.adjustMoveSpeed(
        static_cast<float>(input.scrollDeltaY));
    if (context.setStatusMessage) {
      context.setStatusMessage(
          "Fly speed: " + std::to_string(context.cameraController.moveSpeed()));
    }
    return;
  }

  context.cameraController.dolly(context.selectedMeshNode,
                                 static_cast<float>(input.scrollDeltaY),
                                 wheelSpeedScale(input));
}

}  // namespace container::renderer
