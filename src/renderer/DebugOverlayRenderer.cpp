#include "Container/renderer/DebugOverlayRenderer.h"

#include <algorithm>
#include <cstdint>

namespace container::renderer {

using container::gpu::BindlessPushConstants;

void DebugOverlayRenderer::drawScene(VkCommandBuffer cmd,
                                      VkPipelineLayout layout,
                                      const std::vector<DrawCommand>& commands,
                                      BindlessPushConstants& pc) const {
  for (const DrawCommand& dc : commands) {
    pc.objectIndex = dc.objectIndex;
    vkCmdPushConstants(cmd, layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(BindlessPushConstants), &pc);
    vkCmdDrawIndexed(cmd, dc.indexCount, 1, dc.firstIndex, 0, dc.objectIndex);
  }
}

void DebugOverlayRenderer::drawWireframe(VkCommandBuffer cmd,
                                          VkPipelineLayout layout,
                                          const std::vector<DrawCommand>& commands,
                                          const glm::vec3& color,
                                          float intensity,
                                          float lineWidth,
                                          WireframePushConstants& pc) const {
  pc.colorIntensity = glm::vec4(color, std::clamp(intensity, 0.0f, 1.0f));
  pc.lineWidth      = std::max(lineWidth, 1.0f);
  for (const DrawCommand& dc : commands) {
    pc.objectIndex = dc.objectIndex;
    vkCmdPushConstants(cmd, layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(WireframePushConstants), &pc);
    vkCmdDrawIndexed(cmd, dc.indexCount, 1, dc.firstIndex, 0, dc.objectIndex);
  }
}

void DebugOverlayRenderer::recordNormalValidation(
    VkCommandBuffer cmd, VkPipelineLayout layout,
    const std::vector<DrawCommand>& opaque,
    const std::vector<DrawCommand>& transparent,
    uint32_t faceClassificationFlags,
    const container::gpu::NormalValidationSettings& settings,
    NormalValidationPushConstants& pc) const {
  pc.showFaceFill = settings.showFaceFill ? 1u : 0u;
  pc.faceAlpha    = settings.faceAlpha;
  pc.faceClassificationFlags = faceClassificationFlags;

  constexpr VkShaderStageFlags kStages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
      VK_SHADER_STAGE_FRAGMENT_BIT;

  auto draw = [&](const std::vector<DrawCommand>& cmds) {
    for (const DrawCommand& dc : cmds) {
      pc.objectIndex = dc.objectIndex;
      vkCmdPushConstants(cmd, layout, kStages, 0,
                         sizeof(NormalValidationPushConstants), &pc);
      vkCmdDrawIndexed(cmd, dc.indexCount, 1, dc.firstIndex, 0, dc.objectIndex);
    }
  };
  draw(opaque);
  draw(transparent);
}

void DebugOverlayRenderer::recordSurfaceNormals(
    VkCommandBuffer cmd, VkPipelineLayout layout,
    const std::vector<DrawCommand>& opaque,
    const std::vector<DrawCommand>& transparent,
    const container::gpu::NormalValidationSettings& settings,
    SurfaceNormalPushConstants& pc) const {
  pc.lineLength = settings.lineLength;
  pc.lineOffset = settings.lineOffset;

  constexpr VkShaderStageFlags kStages =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
      VK_SHADER_STAGE_FRAGMENT_BIT;

  auto draw = [&](const std::vector<DrawCommand>& cmds) {
    for (const DrawCommand& dc : cmds) {
      pc.objectIndex = dc.objectIndex;
      vkCmdPushConstants(cmd, layout, kStages, 0,
                         sizeof(SurfaceNormalPushConstants), &pc);
      vkCmdDrawIndexed(cmd, dc.indexCount, 1, dc.firstIndex, 0, dc.objectIndex);
    }
  };
  draw(opaque);
  draw(transparent);
}

}  // namespace container::renderer
