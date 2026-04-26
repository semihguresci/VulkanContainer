#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/common/CommonMath.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneData.h"

#include <cstdint>
#include <vector>

namespace container::renderer {

struct DrawCommand {
  uint32_t objectIndex{0};
  uint32_t firstIndex{0};
  uint32_t indexCount{0};
};

struct WireframePushConstants {
  alignas(4) uint32_t objectIndex{0};
  alignas(4) float padding0{0.0f};
  alignas(4) float padding1{0.0f};
  alignas(4) float padding2{0.0f};
  alignas(4) float padding3{0.0f};
  alignas(4) float padding4{0.0f};
  alignas(4) float padding5{0.0f};
  alignas(4) float padding6{0.0f};
  alignas(16) glm::vec4 colorIntensity{0.0f, 1.0f, 0.0f, 1.0f};
  alignas(4) float lineWidth{1.0f};
  alignas(4) float padding7{0.0f};
  alignas(4) float padding8{0.0f};
  alignas(4) float padding9{0.0f};
};
static_assert(offsetof(WireframePushConstants, colorIntensity) == 32);
static_assert(offsetof(WireframePushConstants, lineWidth) == 48);
static_assert(sizeof(WireframePushConstants) == 64);

struct NormalValidationPushConstants {
  alignas(4) uint32_t objectIndex{0};
  alignas(4) uint32_t showFaceFill{1};
  alignas(4) float faceAlpha{1.0f};
  alignas(4) float padding0{0.0f};
};
static_assert(sizeof(NormalValidationPushConstants) == 16);

struct SurfaceNormalPushConstants {
  alignas(4) uint32_t objectIndex{0};
  alignas(4) float lineLength{0.16f};
  alignas(4) float lineOffset{0.002f};
};
static_assert(sizeof(SurfaceNormalPushConstants) == 12);

// Stateless draw helpers and debug/overlay recording.
// Caller binds pipelines and descriptor sets before calling helpers where noted.
class DebugOverlayRenderer {
 public:
  DebugOverlayRenderer() = default;

  // --- Geometry draw helpers ---
  void drawScene(VkCommandBuffer cmd,
                 VkPipelineLayout layout,
                 const std::vector<DrawCommand>& commands,
                 container::gpu::BindlessPushConstants& pc) const;

  void drawWireframe(VkCommandBuffer cmd,
                     VkPipelineLayout layout,
                     const std::vector<DrawCommand>& commands,
                     const glm::vec3& color,
                     float intensity,
                     float lineWidth,
                     WireframePushConstants& pc) const;

  // --- Debug overlay passes (pipeline already bound by caller) ---
  void recordNormalValidation(
      VkCommandBuffer cmd,
      VkPipelineLayout layout,
      const std::vector<DrawCommand>& opaque,
      const std::vector<DrawCommand>& transparent,
                            const container::gpu::NormalValidationSettings& settings,
                            NormalValidationPushConstants& pc) const;

        void recordSurfaceNormals(
            VkCommandBuffer cmd,
            VkPipelineLayout layout,
            const std::vector<DrawCommand>& opaque,
            const std::vector<DrawCommand>& transparent,
            const container::gpu::NormalValidationSettings& settings,
            SurfaceNormalPushConstants& pc) const;
      };

      }  // namespace container::renderer
