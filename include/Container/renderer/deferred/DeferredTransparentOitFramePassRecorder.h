#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/renderer/core/RenderGraph.h"

#include <cstdint>
#include <optional>

namespace container::ui {
class GuiManager;
enum class GBufferViewMode : uint32_t;
} // namespace container::ui

namespace container::renderer {

class OitManager;
struct FrameRecordParams;

struct DeferredTransparentOitFramePassServices {
  const OitManager *oitManager{nullptr};
  const container::ui::GuiManager *guiManager{nullptr};
  std::optional<container::ui::GBufferViewMode> fallbackDisplayMode{};
};

class DeferredTransparentOitFramePassRecorder {
public:
  explicit DeferredTransparentOitFramePassRecorder(
      DeferredTransparentOitFramePassServices services);

  [[nodiscard]] bool enabled(const FrameRecordParams &p) const;
  [[nodiscard]] RenderPassReadiness readiness(const FrameRecordParams &p) const;
  [[nodiscard]] bool recordClear(VkCommandBuffer cmd,
                                 const FrameRecordParams &p) const;
  [[nodiscard]] bool recordResolvePreparation(VkCommandBuffer cmd,
                                              const FrameRecordParams &p) const;

private:
  DeferredTransparentOitFramePassServices services_{};
};

} // namespace container::renderer
