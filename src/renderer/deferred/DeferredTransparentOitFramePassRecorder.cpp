#include "Container/renderer/deferred/DeferredTransparentOitFramePassRecorder.h"

#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/deferred/DeferredRasterFrameState.h"
#include "Container/renderer/deferred/DeferredRasterResourceBridge.h"
#include "Container/renderer/deferred/DeferredTransparentOitRecorder.h"

namespace container::renderer {

namespace {

DeferredTransparentOitFrameResourceInputs deferredTransparentOitInputs(
    const FrameRecordParams &p, const OitManager *oitManager) {
  DeferredTransparentOitFrameResourceInputs inputs{};
  inputs.oitManager = oitManager;
  inputs.resources = {
      .headPointerImage =
          deferredRasterImage(p, DeferredRasterImageId::OitHeadPointers),
      .nodeBuffer = deferredRasterBuffer(p, DeferredRasterBufferId::OitNode),
      .counterBuffer =
          deferredRasterBuffer(p, DeferredRasterBufferId::OitCounter)};
  return inputs;
}

}  // namespace

DeferredTransparentOitFramePassRecorder::
    DeferredTransparentOitFramePassRecorder(
        DeferredTransparentOitFramePassServices services)
    : services_(services) {}

bool DeferredTransparentOitFramePassRecorder::enabled(
    const FrameRecordParams &p) const {
  return shouldRecordTransparentOit(p, services_.guiManager);
}

RenderPassReadiness DeferredTransparentOitFramePassRecorder::readiness(
    const FrameRecordParams &p) const {
  return enabled(p) ? renderPassReady() : renderPassNotNeeded();
}

bool DeferredTransparentOitFramePassRecorder::recordClear(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  if (!enabled(p)) {
    return false;
  }
  return recordDeferredTransparentOitClearCommands(
      cmd, deferredTransparentOitInputs(p, services_.oitManager));
}

bool DeferredTransparentOitFramePassRecorder::recordResolvePreparation(
    VkCommandBuffer cmd, const FrameRecordParams &p) const {
  if (!enabled(p)) {
    return false;
  }
  return recordDeferredTransparentOitResolvePreparationCommands(
      cmd, deferredTransparentOitInputs(p, services_.oitManager));
}

} // namespace container::renderer
