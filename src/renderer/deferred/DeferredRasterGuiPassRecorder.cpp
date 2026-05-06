#include "Container/renderer/deferred/DeferredRasterGuiPassRecorder.h"

#include "Container/utility/GuiManager.h"

namespace container::renderer {

bool recordDeferredRasterGuiPass(
    const DeferredRasterGuiPassRecordInputs &inputs) {
  if (inputs.commandBuffer == VK_NULL_HANDLE || inputs.guiManager == nullptr) {
    return false;
  }
  inputs.guiManager->render(inputs.commandBuffer);
  return true;
}

} // namespace container::renderer
