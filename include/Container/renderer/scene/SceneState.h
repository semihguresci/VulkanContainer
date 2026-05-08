#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstdint>
#include <limits>

namespace container::renderer {

// Bookkeeping values that describe the currently loaded scene.
// Flows between SceneController, LightingManager, and FrameRecorder.
struct SceneState {
  uint32_t rootNode{container::scene::SceneGraph::kInvalidNode};
  uint32_t cubeNode{container::scene::SceneGraph::kInvalidNode};
  uint32_t selectedMeshNode{container::scene::SceneGraph::kInvalidNode};
  uint32_t diagCubeObjectIndex{std::numeric_limits<uint32_t>::max()};

  VkIndexType                       indexType{VK_INDEX_TYPE_UINT32};
  container::gpu::BufferSlice      vertexSlice{};
  container::gpu::BufferSlice      indexSlice{};
};

}  // namespace container::renderer
