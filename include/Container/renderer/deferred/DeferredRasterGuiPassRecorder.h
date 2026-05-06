#pragma once

#include "Container/common/CommonVulkan.h"

namespace container::ui {
class GuiManager;
} // namespace container::ui

namespace container::renderer {

struct DeferredRasterGuiPassRecordInputs {
  VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
  container::ui::GuiManager *guiManager{nullptr};
};

[[nodiscard]] bool
recordDeferredRasterGuiPass(const DeferredRasterGuiPassRecordInputs &inputs);

} // namespace container::renderer
