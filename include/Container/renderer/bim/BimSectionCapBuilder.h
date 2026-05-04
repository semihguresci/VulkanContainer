#pragma once

#include "Container/common/CommonMath.h"
#include "Container/common/CommonVulkan.h"
#include "Container/geometry/Vertex.h"
#include "Container/renderer/DebugOverlayRenderer.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace container::renderer {

struct BimSectionCapTriangle {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  glm::vec3 p0{0.0f};
  glm::vec3 p1{0.0f};
  glm::vec3 p2{0.0f};
};

inline constexpr size_t kBimSectionCapMaxPlanes = 6u;

struct BimSectionCapBuildOptions {
  glm::vec4 sectionPlane{0.0f, 1.0f, 0.0f, 0.0f};
  uint32_t clipPlaneCount{0};
  std::array<glm::vec4, kBimSectionCapMaxPlanes> clipPlanes{};
  float hatchSpacing{0.25f};
  float hatchAngleRadians{0.7853982f};
  float capOffset{0.001f};
  bool crossHatch{false};
};

struct BimSectionCapGeneratedMesh {
  std::vector<container::geometry::Vertex> vertices{};
  std::vector<uint32_t> indices{};
  std::vector<DrawCommand> fillDrawCommands{};
  std::vector<DrawCommand> hatchDrawCommands{};

  [[nodiscard]] bool valid() const {
    return !vertices.empty() && !indices.empty() &&
           (!fillDrawCommands.empty() || !hatchDrawCommands.empty());
  }
};

struct BimSectionClipCapDrawData {
  container::gpu::BufferSlice vertexSlice{};
  container::gpu::BufferSlice indexSlice{};
  VkIndexType indexType{VK_INDEX_TYPE_UINT32};
  std::vector<DrawCommand> fillDrawCommands{};
  std::vector<DrawCommand> hatchDrawCommands{};

  [[nodiscard]] bool valid() const {
    return vertexSlice.buffer != VK_NULL_HANDLE &&
           indexSlice.buffer != VK_NULL_HANDLE &&
           (!fillDrawCommands.empty() || !hatchDrawCommands.empty());
  }
};

class BimSectionCapBuilder {
 public:
  [[nodiscard]] BimSectionCapGeneratedMesh build(
      std::span<const BimSectionCapTriangle> triangles,
      const BimSectionCapBuildOptions& options) const;
};

[[nodiscard]] glm::vec4 normalizedSectionCapPlane(glm::vec4 plane);

[[nodiscard]] BimSectionCapGeneratedMesh BuildBimSectionCapMesh(
    std::span<const BimSectionCapTriangle> triangles,
    const BimSectionCapBuildOptions& options);

}  // namespace container::renderer
