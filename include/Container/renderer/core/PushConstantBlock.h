#pragma once

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/debug/DebugOverlayPushConstants.h"
#include "Container/renderer/lighting/LightPushConstants.h"
#include "Container/utility/SceneData.h"

namespace container::renderer {

// Owns the mutable push-constant state that RendererFrontend fills each frame
// and passes into FrameRecorder::record().
struct PushConstantBlock {
  container::gpu::BindlessPushConstants bindless{};
  LightPushConstants                    light{};
  WireframePushConstants                wireframe{};
  NormalValidationPushConstants         normalValidation{};
  SurfaceNormalPushConstants            surfaceNormal{};
  TransformGizmoPushConstants           transformGizmo{};

  // Build the pointer-based view consumed by FrameRecordParams.
  FramePushConstantState state() {
    return {&bindless, &light, &wireframe, &normalValidation, &surfaceNormal,
            &transformGizmo};
  }
};

}  // namespace container::renderer
