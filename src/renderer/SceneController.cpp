#include "Container/renderer/SceneController.h"
#include "Container/app/AppConfig.h"
#include "Container/ecs/World.h"
#include "Container/geometry/Model.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneGraph.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/VulkanDevice.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace container::renderer {

using container::gpu::BindlessPushConstants;
using container::gpu::ObjectData;

namespace {

bool transformFlipsWinding(const glm::mat4& transform) {
  const glm::vec3 x(transform[0]);
  const glm::vec3 y(transform[1]);
  const glm::vec3 z(transform[2]);
  const float determinant =
      x.x * (y.y * z.z - y.z * z.y) -
      y.x * (x.y * z.z - x.z * z.y) +
      z.x * (x.y * y.z - x.z * y.y);
  return determinant < 0.0f;
}

}  // namespace

SceneController::SceneController(
    std::shared_ptr<container::gpu::VulkanDevice>  device,
    container::gpu::AllocationManager&             allocationManager,
    container::gpu::PipelineManager&             pipelineManager,
    container::scene::SceneGraph&                     sceneGraph,
    container::scene::SceneManager&                   sceneManager,
    container::ui::GuiManager*                        guiManager,
    const container::app::AppConfig&                           config)
    : device_(std::move(device))
    , allocationManager_(allocationManager)
    , pipelineManager_(pipelineManager)
    , sceneGraph_(sceneGraph)
    , sceneManager_(sceneManager)
    , guiManager_(guiManager)
    , config_(config)
    , world_(std::make_unique<container::ecs::World>()) {
}

SceneController::~SceneController() = default;

container::ecs::World& SceneController::world() { return *world_; }
const container::ecs::World& SceneController::world() const { return *world_; }

// ---------------------------------------------------------------------------
// Static buffer helpers
// ---------------------------------------------------------------------------

void SceneController::writeToBuffer(
    container::gpu::AllocationManager&      allocationManager,
    const container::gpu::AllocatedBuffer&  buffer,
    const void*                              data,
    size_t                                   size) {
  void* mapped = buffer.allocation_info.pMappedData;
  bool  mappedHere = false;
  if (mapped == nullptr) {
    if (vmaMapMemory(allocationManager.memoryManager()->allocator(),
                     buffer.allocation, &mapped) != VK_SUCCESS) {
      throw std::runtime_error("failed to map buffer for writing");
    }
    mappedHere = true;
  }

  std::memcpy(mapped, data, size);
  if (vmaFlushAllocation(allocationManager.memoryManager()->allocator(),
                         buffer.allocation, 0,
                         static_cast<VkDeviceSize>(size)) != VK_SUCCESS) {
    if (mappedHere) {
      vmaUnmapMemory(allocationManager.memoryManager()->allocator(),
                     buffer.allocation);
    }
    throw std::runtime_error("failed to flush buffer after writing");
  }

  if (mappedHere) {
    vmaUnmapMemory(allocationManager.memoryManager()->allocator(),
                   buffer.allocation);
  }
}

bool SceneController::ensureObjectBufferCapacity(
    container::gpu::AllocationManager&  allocationManager,
    container::gpu::AllocatedBuffer&    objectBuffer,
    size_t&                              objectBufferCapacity,
    size_t                               requiredObjectCount) {
  const size_t required = std::max<size_t>(1, requiredObjectCount);
  if (objectBuffer.buffer != VK_NULL_HANDLE &&
      objectBufferCapacity >= required) {
    return false;
  }

  if (objectBuffer.buffer != VK_NULL_HANDLE) {
    allocationManager.destroyBuffer(objectBuffer);
    objectBufferCapacity = 0;
  }

  objectBuffer = allocationManager.createBuffer(
      sizeof(ObjectData) * required, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      VMA_MEMORY_USAGE_AUTO,
      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
          VMA_ALLOCATION_CREATE_MAPPED_BIT);
  objectBufferCapacity = required;
  return true;
}

// ---------------------------------------------------------------------------
// Geometry buffers
// ---------------------------------------------------------------------------

void SceneController::createGeometryBuffers() {
  const auto& sceneVertices = sceneManager_.vertices();
  const auto& sceneIndices  = sceneManager_.indices();
  const container::geometry::Model cube = container::geometry::Model::MakeCube();

  std::vector<container::geometry::Vertex> mergedVertices;
  mergedVertices.reserve(sceneVertices.size() + cube.vertices().size());
  mergedVertices.insert(mergedVertices.end(), sceneVertices.begin(),
                        sceneVertices.end());
  const uint32_t diagVertexBase =
      static_cast<uint32_t>(mergedVertices.size());
  mergedVertices.insert(mergedVertices.end(), cube.vertices().begin(),
                        cube.vertices().end());

  std::vector<uint32_t> mergedIndices;
  mergedIndices.reserve(sceneIndices.size() + cube.indices().size());
  mergedIndices.insert(mergedIndices.end(), sceneIndices.begin(),
                       sceneIndices.end());
  const VkDeviceSize diagIndexByteOffset =
      static_cast<VkDeviceSize>(mergedIndices.size()) * sizeof(uint32_t);
  for (const uint32_t index : cube.indices()) {
    mergedIndices.push_back(index + diagVertexBase);
  }

  vertexSlice_ = allocationManager_.uploadVertices(
      std::span<const container::geometry::Vertex>(mergedVertices));
  indexSlice_ = allocationManager_.uploadIndices(
      std::span<const uint32_t>(mergedIndices));

  diagCubeVertexSlice_ = vertexSlice_;
  diagCubeIndexSlice_  = indexSlice_;
  diagCubeIndexSlice_.offset = indexSlice_.offset + diagIndexByteOffset;
  diagCubeIndexSlice_.size =
      static_cast<VkDeviceSize>(cube.indices().size()) * sizeof(uint32_t);
  diagCubeIndexCount_ = static_cast<uint32_t>(cube.indices().size());
}

void SceneController::createSceneBuffers(
    const container::gpu::AllocatedBuffer& cameraBuffer,
    container::gpu::AllocatedBuffer&       objectBuffer,
    size_t&                                 objectBufferCapacity) {
  ensureObjectBufferCapacity(allocationManager_, objectBuffer,
                             objectBufferCapacity,
                             sceneGraph_.renderableNodes().size());
  syncObjectDataFromSceneGraph(false);
  // Upload is deferred to updateObjectBuffer; descriptor sets updated by caller.
}

// ---------------------------------------------------------------------------
// syncObjectDataFromSceneGraph
// ---------------------------------------------------------------------------

void SceneController::syncObjectDataFromSceneGraph(bool showDiagCube) {
  sceneGraph_.updateWorldTransforms();

  // Sync ECS registry from the scene graph so views are up-to-date.
  world_->syncFromSceneGraph(sceneGraph_);

  objectData_.clear();
  opaqueDrawCommands_.clear();
  transparentDrawCommands_.clear();
  opaqueSingleSidedDrawCommands_.clear();
  opaqueWindingFlippedDrawCommands_.clear();
  opaqueDoubleSidedDrawCommands_.clear();
  transparentSingleSidedDrawCommands_.clear();
  transparentWindingFlippedDrawCommands_.clear();
  transparentDoubleSidedDrawCommands_.clear();
  const uint32_t renderableCount = world_->renderableCount();
  objectData_.reserve(renderableCount);
  opaqueDrawCommands_.reserve(renderableCount);
  transparentDrawCommands_.reserve(renderableCount);
  opaqueSingleSidedDrawCommands_.reserve(renderableCount);
  opaqueWindingFlippedDrawCommands_.reserve(renderableCount);
  opaqueDoubleSidedDrawCommands_.reserve(renderableCount);
  transparentSingleSidedDrawCommands_.reserve(renderableCount);
  transparentWindingFlippedDrawCommands_.reserve(renderableCount);
  transparentDoubleSidedDrawCommands_.reserve(renderableCount);

  world_->forEachRenderable(
      [this](const container::ecs::TransformComponent& transform,
             const container::ecs::MeshComponent&      mesh,
             const container::ecs::MaterialComponent&   material) {
        if (mesh.primitiveIndex == std::numeric_limits<uint32_t>::max() ||
            mesh.primitiveIndex >= sceneManager_.primitiveRanges().size()) {
          return;
        }

        const auto& primitive =
            sceneManager_.primitiveRanges()[mesh.primitiveIndex];
        const uint32_t materialIndex =
            sceneManager_.resolveGpuMaterialIndex(material.materialIndex);

        ObjectData object{};
        object.model = transform.worldTransform;
        object.objectInfo.x = materialIndex;
        {
          const glm::mat3 model3  = glm::mat3(transform.worldTransform);
          const glm::mat3 normal3 = glm::transpose(glm::inverse(model3));
          object.normalMatrix0 = glm::vec4(normal3[0], 0.0f);
          object.normalMatrix1 = glm::vec4(normal3[1], 0.0f);
          object.normalMatrix2 = glm::vec4(normal3[2], 0.0f);
        }
        const bool materialDoubleSided =
            sceneManager_.isMaterialDoubleSided(materialIndex);
        const bool materialTransparent =
            sceneManager_.isMaterialTransparent(materialIndex);
        const bool rasterDoubleSided =
            materialDoubleSided || primitive.disableBackfaceCulling;
        const bool windingFlipped =
            transformFlipsWinding(transform.worldTransform);
        object.objectInfo.y =
            rasterDoubleSided ? container::gpu::kObjectFlagDoubleSided : 0u;

        // Compute world-space bounding sphere from primitive vertices.
        {
          const auto& verts   = sceneManager_.vertices();
          const auto& indices = sceneManager_.indices();
          glm::vec3 localMin(std::numeric_limits<float>::max());
          glm::vec3 localMax(std::numeric_limits<float>::lowest());
          const uint32_t endIdx = primitive.firstIndex + primitive.indexCount;
          for (uint32_t i = primitive.firstIndex; i < endIdx && i < indices.size(); ++i) {
            const glm::vec3& pos = verts[indices[i]].position;
            localMin = glm::min(localMin, pos);
            localMax = glm::max(localMax, pos);
          }
          if (primitive.indexCount > 0) {
            const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
            float localRadius = 0.0f;
            for (uint32_t i = primitive.firstIndex; i < endIdx && i < indices.size(); ++i) {
              localRadius = std::max(localRadius,
                  glm::length(verts[indices[i]].position - localCenter));
            }
            const glm::vec3 worldCenter =
                glm::vec3(transform.worldTransform * glm::vec4(localCenter, 1.0f));
            const float scaleMax = std::max({
                glm::length(glm::vec3(transform.worldTransform[0])),
                glm::length(glm::vec3(transform.worldTransform[1])),
                glm::length(glm::vec3(transform.worldTransform[2]))});
            const float heightInflation =
                std::abs(sceneManager_.resolveMaterialHeightScale(
                    materialIndex)) *
                scaleMax;
            object.boundingSphere =
                glm::vec4(worldCenter, localRadius * scaleMax + heightInflation);
          }
        }

        const uint32_t objectIndex =
            static_cast<uint32_t>(objectData_.size());
        objectData_.push_back(object);

        DrawCommand drawCommand{};
        drawCommand.objectIndex = objectIndex;
        drawCommand.firstIndex  = primitive.firstIndex;
        drawCommand.indexCount  = primitive.indexCount;

        if (materialTransparent) {
          transparentDrawCommands_.push_back(drawCommand);
          if (rasterDoubleSided) {
            transparentDoubleSidedDrawCommands_.push_back(drawCommand);
          } else if (windingFlipped) {
            transparentWindingFlippedDrawCommands_.push_back(drawCommand);
          } else {
            transparentSingleSidedDrawCommands_.push_back(drawCommand);
          }
        } else {
          opaqueDrawCommands_.push_back(drawCommand);
          if (rasterDoubleSided) {
            opaqueDoubleSidedDrawCommands_.push_back(drawCommand);
          } else if (windingFlipped) {
            opaqueWindingFlippedDrawCommands_.push_back(drawCommand);
          } else {
            opaqueSingleSidedDrawCommands_.push_back(drawCommand);
          }
        }
      });

  // Diagnostic cube: appended last so its objectIndex is always known.
  diagCubeObjectIndex_ = std::numeric_limits<uint32_t>::max();
  if (showDiagCube && diagCubeVertexSlice_.buffer != VK_NULL_HANDLE) {
    ObjectData cubeObject{};
    glm::vec3  diagCenter{0.0f};
    float      diagScale = 0.5f;
    const auto& bounds = sceneManager_.modelBounds();
    if (bounds.valid) {
      diagCenter = bounds.center +
                   glm::vec3(bounds.radius * 1.75f, 0.0f, 0.0f);
      diagScale = std::max(0.25f, bounds.radius * 0.45f);
    }
    const glm::mat4 cubeModel =
        glm::translate(glm::mat4(1.0f), diagCenter) *
        glm::scale(glm::mat4(1.0f), glm::vec3(diagScale));
    cubeObject.model = cubeModel;
    const glm::mat3 cubeNormal =
        glm::transpose(glm::inverse(glm::mat3(cubeModel)));
    cubeObject.normalMatrix0 = glm::vec4(cubeNormal[0], 0.0f);
    cubeObject.normalMatrix1 = glm::vec4(cubeNormal[1], 0.0f);
    cubeObject.normalMatrix2 = glm::vec4(cubeNormal[2], 0.0f);
    cubeObject.objectInfo.x = sceneManager_.diagnosticMaterialIndex();
    diagCubeObjectIndex_ = static_cast<uint32_t>(objectData_.size());
    objectData_.push_back(cubeObject);
  }
}

// ---------------------------------------------------------------------------
// updateObjectBuffer
// ---------------------------------------------------------------------------

bool SceneController::updateObjectBuffer(
    container::gpu::AllocatedBuffer&       objectBuffer,
    size_t&                                 objectBufferCapacity,
    const container::gpu::AllocatedBuffer& cameraBuffer) {
  const bool showDiagCube =
      guiManager_ && guiManager_->showNormalDiagCube() &&
      diagCubeVertexSlice_.buffer != VK_NULL_HANDLE;
  syncObjectDataFromSceneGraph(showDiagCube);

  const bool bufferRecreated = ensureObjectBufferCapacity(
      allocationManager_, objectBuffer, objectBufferCapacity,
      objectData_.size());
  if (objectBuffer.buffer == VK_NULL_HANDLE || objectData_.empty())
    return bufferRecreated;

  writeToBuffer(allocationManager_, objectBuffer, objectData_.data(),
                sizeof(ObjectData) * objectData_.size());
  return bufferRecreated;
}

// ---------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------

void SceneController::buildSceneGraph(uint32_t& outRootNode,
                                      uint32_t& outSelectedMeshNode,
                                      uint32_t& outCubeNode) {
  sceneManager_.populateSceneGraph(sceneGraph_);
  outRootNode = container::scene::SceneGraph::kInvalidNode;

  const uint32_t existingNodeCount =
      static_cast<uint32_t>(sceneGraph_.nodeCount());
  if (existingNodeCount > 0) {
    outRootNode = sceneGraph_.createNode(
        glm::mat4(1.0f), sceneManager_.defaultMaterialIndex(), false);
    for (uint32_t i = 0; i < existingNodeCount; ++i) {
      const auto* node = sceneGraph_.getNode(i);
      if (node != nullptr &&
          node->parent == container::scene::SceneGraph::kInvalidNode) {
        sceneGraph_.setParent(i, outRootNode);
      }
    }
    sceneGraph_.updateWorldTransforms();
  }

  const auto& renderable = sceneGraph_.renderableNodes();
  if (renderable.empty()) {
    outSelectedMeshNode = container::scene::SceneGraph::kInvalidNode;
  } else if (std::find(renderable.begin(), renderable.end(),
                       outSelectedMeshNode) == renderable.end()) {
    outSelectedMeshNode = renderable.front();
  }
  outCubeNode = container::scene::SceneGraph::kInvalidNode;
}

void SceneController::addSceneObject(
    const glm::mat4&                         transform,
    uint32_t                                 rootNode,
    container::gpu::AllocatedBuffer&        objectBuffer,
    size_t&                                  objectBufferCapacity,
    const container::gpu::AllocatedBuffer&  cameraBuffer) {
  if (sceneGraph_.renderableNodes().size() >= config_.maxSceneObjects) {
    if (guiManager_) {
      guiManager_->setStatusMessage("Reached maximum scene object capacity");
    }
    return;
  }
  const uint32_t node = sceneGraph_.createNode(
      transform, sceneManager_.defaultMaterialIndex(), true);
  sceneGraph_.setParent(node, rootNode);
  sceneGraph_.updateWorldTransforms();
  updateObjectBuffer(objectBuffer, objectBufferCapacity, cameraBuffer);
  if (guiManager_) {
    guiManager_->setStatusMessage("Added object to scene");
  }
}

bool SceneController::reloadSceneModel(
    const std::string&                      path,
    float                                   importScale,
    container::gpu::AllocatedBuffer&       objectBuffer,
    size_t&                                 objectBufferCapacity,
    const container::gpu::AllocatedBuffer& cameraBuffer,
    VkIndexType&                            outIndexType,
    uint32_t&                               outRootNode,
    uint32_t&                               outSelectedMeshNode,
    uint32_t&                               outCubeNode) {
  vkDeviceWaitIdle(device_->device());
  const std::array<container::gpu::AllocatedBuffer, 1> cameraBuffers = {
      cameraBuffer};
  const bool result =
      sceneManager_.reloadModel(path, importScale, cameraBuffers, objectBuffer);
  outIndexType = sceneManager_.indexType();
  buildSceneGraph(outRootNode, outSelectedMeshNode, outCubeNode);
  ensureObjectBufferCapacity(allocationManager_, objectBuffer,
                             objectBufferCapacity,
                             sceneGraph_.renderableNodes().size());
  createGeometryBuffers();
  if (guiManager_) {
    guiManager_->setStatusMessage(
        result ? "Loaded model: " + path : "Failed to load model: " + path);
  }
  return result;
}

}  // namespace container::renderer
