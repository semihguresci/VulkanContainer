#pragma once

#include "Container/common/CommonVulkan.h"
#include "Container/geometry/Vertex.h"
#include "Container/renderer/DebugOverlayRenderer.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/VulkanMemoryManager.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace container::gpu {
class AllocationManager;
struct AllocatedBuffer;
}  // namespace container::gpu

namespace container::geometry::dotbim {
struct Model;
}  // namespace container::geometry::dotbim

namespace container::scene {
class SceneManager;
}  // namespace container::scene

namespace container::renderer {

struct BimPickHit {
  uint32_t objectIndex{std::numeric_limits<uint32_t>::max()};
  float distance{std::numeric_limits<float>::max()};
  float depth{0.0f};
  bool hit{false};
};

// Owns sidecar model draw data independently from the regular scene graph.
// Supports dotbim, tessellated IFC, IFCX, USD/USDC/USDZ meshes, and
// glTF fallback content routed through the same BIM render passes.
class BimManager {
 public:
  explicit BimManager(container::gpu::AllocationManager& allocationManager);
  ~BimManager();

  BimManager(const BimManager&) = delete;
  BimManager& operator=(const BimManager&) = delete;

  void loadModel(const std::string& path,
                 float importScale,
                 container::scene::SceneManager& sceneManager);
  void clear();

  [[nodiscard]] bool hasScene() const;
  [[nodiscard]] const std::string& modelPath() const { return modelPath_; }

  [[nodiscard]] container::gpu::BufferSlice vertexSlice() const {
    return vertexSlice_;
  }
  [[nodiscard]] container::gpu::BufferSlice indexSlice() const {
    return indexSlice_;
  }
  [[nodiscard]] VkIndexType indexType() const { return VK_INDEX_TYPE_UINT32; }

  [[nodiscard]] const std::vector<container::gpu::ObjectData>& objectData() const {
    return objectData_;
  }
  [[nodiscard]] uint64_t objectDataRevision() const {
    return objectDataRevision_;
  }
  [[nodiscard]] VkBuffer objectBuffer() const { return objectBuffer_.buffer; }
  [[nodiscard]] VkDeviceSize objectBufferSize() const;

  [[nodiscard]] const container::gpu::AllocatedBuffer& objectAllocatedBuffer() const {
    return objectBuffer_;
  }

  [[nodiscard]] const std::vector<DrawCommand>& opaqueDrawCommands() const {
    return opaqueDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand>& opaqueSingleSidedDrawCommands() const {
    return opaqueSingleSidedDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand>& opaqueWindingFlippedDrawCommands() const {
    return opaqueWindingFlippedDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand>& opaqueDoubleSidedDrawCommands() const {
    return opaqueDoubleSidedDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand>& transparentDrawCommands() const {
    return transparentDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand>& transparentSingleSidedDrawCommands() const {
    return transparentSingleSidedDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand>& transparentWindingFlippedDrawCommands() const {
    return transparentWindingFlippedDrawCommands_;
  }
  [[nodiscard]] const std::vector<DrawCommand>& transparentDoubleSidedDrawCommands() const {
    return transparentDoubleSidedDrawCommands_;
  }
  [[nodiscard]] BimPickHit pickRenderableObject(
      const container::gpu::CameraData& cameraData,
      VkExtent2D viewportExtent,
      double cursorX,
      double cursorY) const;
  [[nodiscard]] BimPickHit pickTransparentRenderableObject(
      const container::gpu::CameraData& cameraData,
      VkExtent2D viewportExtent,
      double cursorX,
      double cursorY) const;
  void collectDrawCommandsForObject(
      uint32_t objectIndex,
      std::vector<DrawCommand>& outCommands) const;

 private:
  struct MeshRange {
    uint32_t meshId{0};
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    glm::vec3 boundsCenter{0.0f};
    float boundsRadius{0.0f};
  };

  [[nodiscard]] std::filesystem::path resolveModelPath(
      const std::string& path) const;

  void loadDotBim(const std::filesystem::path& path,
                  float importScale,
                  container::scene::SceneManager& sceneManager);
  void loadIfc(const std::filesystem::path& path,
               float importScale,
               container::scene::SceneManager& sceneManager);
  void loadGltfFallback(const std::filesystem::path& path,
                        float importScale,
                        container::scene::SceneManager& sceneManager);
  void loadPreparedModel(
      const container::geometry::dotbim::Model& model,
      const std::filesystem::path& path,
      std::string_view format,
      container::scene::SceneManager& sceneManager);
  void buildDrawDataFromModel(
      const container::geometry::dotbim::Model& model,
      container::scene::SceneManager& sceneManager);
  [[nodiscard]] BimPickHit pickRenderableObjectForDraws(
      const container::gpu::CameraData& cameraData,
      VkExtent2D viewportExtent,
      double cursorX,
      double cursorY,
      bool includeOpaque,
      bool includeTransparent) const;
  void uploadGeometry(std::span<const container::geometry::Vertex> vertices,
                      std::span<const uint32_t> indices);
  void uploadObjects();

  container::gpu::AllocationManager& allocationManager_;
  std::string modelPath_{};

  container::gpu::BufferSlice vertexSlice_{};
  container::gpu::BufferSlice indexSlice_{};
  container::gpu::AllocatedBuffer vertexBuffer_{};
  container::gpu::AllocatedBuffer indexBuffer_{};
  container::gpu::AllocatedBuffer objectBuffer_{};
  size_t objectBufferCapacity_{0};
  uint64_t objectDataRevision_{0};

  std::vector<container::geometry::Vertex> vertices_{};
  std::vector<uint32_t> indices_{};
  std::vector<container::gpu::ObjectData> objectData_{};
  std::vector<DrawCommand> opaqueDrawCommands_{};
  std::vector<DrawCommand> opaqueSingleSidedDrawCommands_{};
  std::vector<DrawCommand> opaqueWindingFlippedDrawCommands_{};
  std::vector<DrawCommand> opaqueDoubleSidedDrawCommands_{};
  std::vector<DrawCommand> transparentDrawCommands_{};
  std::vector<DrawCommand> transparentSingleSidedDrawCommands_{};
  std::vector<DrawCommand> transparentWindingFlippedDrawCommands_{};
  std::vector<DrawCommand> transparentDoubleSidedDrawCommands_{};
};

}  // namespace container::renderer
