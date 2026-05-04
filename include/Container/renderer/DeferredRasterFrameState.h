#pragma once

#include "Container/renderer/FrameRecorder.h"

#include <array>
#include <cstdint>
#include <vector>

namespace container::ui {
class GuiManager;
enum class GBufferViewMode : uint32_t;
}  // namespace container::ui

namespace container::renderer {

[[nodiscard]] bool hasDrawCommands(const std::vector<DrawCommand>* commands);
[[nodiscard]] bool hasSplitOpaqueDrawCommands(const FrameDrawLists& draws);
[[nodiscard]] bool hasOpaqueDrawCommands(const FrameDrawLists& draws);
[[nodiscard]] bool hasTransparentDrawCommands(const FrameDrawLists& draws);
[[nodiscard]] bool hasTransparentDrawCommands(const FrameRecordParams& p);
[[nodiscard]] bool hasBimOpaqueDrawCommands(const FrameBimResources& bim);
[[nodiscard]] bool hasBimTransparentGeometry(const FrameRecordParams& p);

[[nodiscard]] std::array<const FrameDrawLists*, 3> bimSurfaceDrawListSet(
    const FrameBimResources& bim);

[[nodiscard]] container::ui::GBufferViewMode currentDisplayMode(
    const container::ui::GuiManager* guiManager);
[[nodiscard]] bool displayModeRecordsShadowAtlas(
    container::ui::GBufferViewMode mode);
[[nodiscard]] bool displayModeRecordsTileCull(
    container::ui::GBufferViewMode mode);
[[nodiscard]] bool displayModeRecordsGtao(container::ui::GBufferViewMode mode);
[[nodiscard]] bool displayModeRecordsExposureAdaptation(
    container::ui::GBufferViewMode mode);
[[nodiscard]] bool displayModeRecordsBloom(
    container::ui::GBufferViewMode mode);

[[nodiscard]] container::gpu::ExposureSettings sanitizeExposureSettings(
    container::gpu::ExposureSettings settings);
[[nodiscard]] bool shouldRecordTransparentOit(
    const FrameRecordParams& p,
    const container::ui::GuiManager* guiManager);

[[nodiscard]] RenderPassReadiness renderPassReady();
[[nodiscard]] RenderPassReadiness renderPassNotNeeded();
[[nodiscard]] RenderPassReadiness renderPassMissingResource(
    RenderResourceId resource);

}  // namespace container::renderer
