// ecs_tests.cpp
//
// CPU-only unit tests for the ECS layer (Components + World).
// No GPU, no window, and no Vulkan device are required.
//
// Structs under test
// ------------------
//   TransformComponent       (Components.h)
//   MeshComponent            (Components.h)
//   MaterialComponent        (Components.h)
//   LightComponent           (Components.h)
//   CameraComponent          (Components.h)
//   RenderableTag            (Components.h)
//   LightTag                 (Components.h)
//   CameraTag                (Components.h)
//   SceneNodeRef             (Components.h)
//   World                    (World.h)

#include <gtest/gtest.h>

#include "Container/ecs/Components.h"
#include "Container/ecs/World.h"
#include "Container/utility/SceneGraph.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

// ============================================================================
// Component Defaults
// ============================================================================

TEST(ECS_TransformComponent, DefaultIsIdentity) {
  container::ecs::TransformComponent tc;
  EXPECT_EQ(tc.localTransform, glm::mat4(1.0f));
  EXPECT_EQ(tc.worldTransform, glm::mat4(1.0f));
}

TEST(ECS_MeshComponent, DefaultPrimitiveIndexIsMax) {
  container::ecs::MeshComponent mc;
  EXPECT_EQ(mc.primitiveIndex, std::numeric_limits<uint32_t>::max());
}

TEST(ECS_MaterialComponent, DefaultMaterialIndexIsZero) {
  container::ecs::MaterialComponent mat;
  EXPECT_EQ(mat.materialIndex, 0u);
}

TEST(ECS_LightComponent, DefaultIsWhiteUnitPointLight) {
  container::ecs::LightComponent light;
  EXPECT_EQ(light.data.positionRadius, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  EXPECT_EQ(light.data.colorIntensity, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
}

TEST(ECS_CameraComponent, DefaultIsIdentityCameraData) {
  container::ecs::CameraComponent camera;
  EXPECT_EQ(camera.data.viewProj, glm::mat4(1.0f));
  EXPECT_EQ(camera.data.inverseViewProj, glm::mat4(1.0f));
  EXPECT_EQ(camera.data.cameraWorldPosition, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  EXPECT_EQ(camera.data.cameraForward, glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
  EXPECT_FLOAT_EQ(camera.nearPlane, 0.1f);
  EXPECT_FLOAT_EQ(camera.farPlane, 100.0f);
}

TEST(ECS_SceneNodeRef, DefaultNodeIndexIsMax) {
  container::ecs::SceneNodeRef ref;
  EXPECT_EQ(ref.nodeIndex, std::numeric_limits<uint32_t>::max());
}

// ============================================================================
// World — empty state
// ============================================================================

TEST(ECS_World, DefaultConstructionIsEmpty) {
  container::ecs::World world;
  EXPECT_EQ(world.entityCount(), 0u);
  EXPECT_EQ(world.renderableCount(), 0u);
  EXPECT_EQ(world.pointLightCount(), 0u);
  EXPECT_FALSE(world.hasActiveCamera());
}

TEST(ECS_World, ClearOnEmptyIsNoOp) {
  container::ecs::World world;
  world.clear();
  EXPECT_EQ(world.entityCount(), 0u);
}

// ============================================================================
// World — syncFromSceneGraph
// ============================================================================

TEST(ECS_World, SyncFromEmptyGraphProducesNoEntities) {
  container::scene::SceneGraph graph;
  container::ecs::World world;
  world.syncFromSceneGraph(graph);
  EXPECT_EQ(world.entityCount(), 0u);
  EXPECT_EQ(world.renderableCount(), 0u);
}

TEST(ECS_World, SyncCreatesOneEntityPerRenderableNode) {
  container::scene::SceneGraph graph;
  // Node 0: renderable with primitive 0
  graph.createNode(glm::mat4(1.0f), /*materialIndex=*/5, /*renderable=*/true,
                   /*primitiveIndex=*/0);
  // Node 1: NOT renderable
  graph.createNode(glm::mat4(1.0f), /*materialIndex=*/0, /*renderable=*/false);
  // Node 2: renderable with primitive 1
  graph.createNode(glm::mat4(1.0f), /*materialIndex=*/3, /*renderable=*/true,
                   /*primitiveIndex=*/1);

  container::ecs::World world;
  world.syncFromSceneGraph(graph);

  EXPECT_EQ(world.renderableCount(), 2u);
  // Total entities should also be 2 (only renderable nodes become entities)
  EXPECT_EQ(world.entityCount(), 2u);
}

TEST(ECS_World, SyncPreservesTransforms) {
  container::scene::SceneGraph graph;
  const glm::mat4 transform =
      glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3));
  graph.createNode(transform, /*materialIndex=*/0, /*renderable=*/true,
                   /*primitiveIndex=*/0);
  graph.updateWorldTransforms();

  container::ecs::World world;
  world.syncFromSceneGraph(graph);

  std::vector<glm::mat4> worldTransforms;
  world.forEachRenderable([&](const container::ecs::TransformComponent &tc,
                              const container::ecs::MeshComponent &,
                              const container::ecs::MaterialComponent &) {
    worldTransforms.push_back(tc.worldTransform);
  });

  ASSERT_EQ(worldTransforms.size(), 1u);
  EXPECT_EQ(worldTransforms[0], transform);
}

TEST(ECS_World, SyncPreservesMeshAndMaterialIndices) {
  container::scene::SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), /*materialIndex=*/7, /*renderable=*/true,
                   /*primitiveIndex=*/42);

  container::ecs::World world;
  world.syncFromSceneGraph(graph);

  uint32_t primitiveIdx = 0;
  uint32_t materialIdx = 0;
  world.forEachRenderable([&](const container::ecs::TransformComponent &,
                              const container::ecs::MeshComponent &mesh,
                              const container::ecs::MaterialComponent &mat) {
    primitiveIdx = mesh.primitiveIndex;
    materialIdx = mat.materialIndex;
  });

  EXPECT_EQ(primitiveIdx, 42u);
  EXPECT_EQ(materialIdx, 7u);
}

TEST(ECS_World, SyncClearsPreviousEntities) {
  container::scene::SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0, true, 0);
  graph.createNode(glm::mat4(1.0f), 0, true, 1);

  container::ecs::World world;
  world.syncFromSceneGraph(graph);
  EXPECT_EQ(world.renderableCount(), 2u);

  // Create a new graph with only 1 renderable
  container::scene::SceneGraph graph2;
  graph2.createNode(glm::mat4(1.0f), 0, true, 0);

  world.syncFromSceneGraph(graph2);
  EXPECT_EQ(world.renderableCount(), 1u);
}

TEST(ECS_World, SyncPreservesLightAndCameraEntities) {
  container::ecs::World world;

  container::gpu::PointLightData light{};
  light.positionRadius = glm::vec4(1.0f, 2.0f, 3.0f, 4.0f);
  world.replacePointLights(
      std::span<const container::gpu::PointLightData>{&light, 1});

  container::gpu::CameraData camera{};
  camera.cameraWorldPosition = glm::vec4(5.0f, 6.0f, 7.0f, 1.0f);
  (void)world.setActiveCamera(camera, 0.25f, 750.0f);

  container::scene::SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0, true, 0);
  world.syncFromSceneGraph(graph);

  EXPECT_EQ(world.renderableCount(), 1u);
  EXPECT_EQ(world.pointLightCount(), 1u);
  ASSERT_NE(world.activeCamera(), nullptr);
  EXPECT_EQ(world.activeCamera()->data.cameraWorldPosition,
            glm::vec4(5.0f, 6.0f, 7.0f, 1.0f));
  EXPECT_FLOAT_EQ(world.activeCamera()->nearPlane, 0.25f);
  EXPECT_FLOAT_EQ(world.activeCamera()->farPlane, 750.0f);

  container::scene::SceneGraph emptyGraph;
  world.syncFromSceneGraph(emptyGraph);

  EXPECT_EQ(world.renderableCount(), 0u);
  EXPECT_EQ(world.pointLightCount(), 1u);
  EXPECT_TRUE(world.hasActiveCamera());
  EXPECT_EQ(world.entityCount(), 2u);
}

// ============================================================================
// World — forEachRenderable
// ============================================================================

TEST(ECS_World, ForEachRenderableOnEmptyIsNoOp) {
  container::ecs::World world;
  int callCount = 0;
  world.forEachRenderable(
      [&](const container::ecs::TransformComponent &,
          const container::ecs::MeshComponent &,
          const container::ecs::MaterialComponent &) { ++callCount; });
  EXPECT_EQ(callCount, 0);
}

TEST(ECS_World, ForEachRenderableVisitsAllRenderables) {
  container::scene::SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0, true, 0);
  graph.createNode(glm::mat4(1.0f), 1, true, 1);
  graph.createNode(glm::mat4(1.0f), 2, true, 2);

  container::ecs::World world;
  world.syncFromSceneGraph(graph);

  int callCount = 0;
  world.forEachRenderable(
      [&](const container::ecs::TransformComponent &,
          const container::ecs::MeshComponent &,
          const container::ecs::MaterialComponent &) { ++callCount; });
  EXPECT_EQ(callCount, 3);
}

TEST(ECS_World, ForEachRenderableWithNodePreservesSceneNodeRefs) {
  container::scene::SceneGraph graph;
  const uint32_t first = graph.createNode(glm::mat4(1.0f), 0, true, 10);
  const uint32_t second = graph.createNode(glm::mat4(1.0f), 1, true, 11);

  container::ecs::World world;
  world.syncFromSceneGraph(graph);

  std::vector<uint32_t> visitedNodes;
  world.forEachRenderableWithNode(
      [&](const container::ecs::TransformComponent &,
          const container::ecs::MeshComponent &,
          const container::ecs::MaterialComponent &,
          const container::ecs::SceneNodeRef &node) {
        visitedNodes.push_back(node.nodeIndex);
      });

  EXPECT_EQ(visitedNodes.size(), 2u);
  EXPECT_NE(std::find(visitedNodes.begin(), visitedNodes.end(), first),
            visitedNodes.end());
  EXPECT_NE(std::find(visitedNodes.begin(), visitedNodes.end(), second),
            visitedNodes.end());
}

// ============================================================================
// World â€” point lights
// ============================================================================

TEST(ECS_World, ReplacePointLightsCreatesLightEntities) {
  std::vector<container::gpu::PointLightData> lights(2);
  lights[0].positionRadius = glm::vec4(0.0f, 1.0f, 2.0f, 3.0f);
  lights[1].positionRadius = glm::vec4(4.0f, 5.0f, 6.0f, 7.0f);

  container::ecs::World world;
  world.replacePointLights(lights);

  EXPECT_EQ(world.entityCount(), 2u);
  EXPECT_EQ(world.pointLightCount(), 2u);

  float radiusSum = 0.0f;
  world.forEachPointLight([&](const container::ecs::LightComponent &light) {
    radiusSum += light.data.positionRadius.w;
  });
  EXPECT_FLOAT_EQ(radiusSum, 10.0f);
}

TEST(ECS_World, ReplacePointLightsClearsPreviousLightEntitiesOnly) {
  std::vector<container::gpu::PointLightData> initialLights(2);
  std::vector<container::gpu::PointLightData> replacementLights(1);

  container::scene::SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0, true, 0);

  container::ecs::World world;
  world.syncFromSceneGraph(graph);
  (void)world.setActiveCamera(container::gpu::CameraData{});
  world.replacePointLights(initialLights);
  EXPECT_EQ(world.renderableCount(), 1u);
  EXPECT_EQ(world.pointLightCount(), 2u);
  EXPECT_TRUE(world.hasActiveCamera());

  world.replacePointLights(replacementLights);

  EXPECT_EQ(world.renderableCount(), 1u);
  EXPECT_EQ(world.pointLightCount(), 1u);
  EXPECT_TRUE(world.hasActiveCamera());
  EXPECT_EQ(world.entityCount(), 3u);
}

// ============================================================================
// World â€” active camera
// ============================================================================

TEST(ECS_World, SetActiveCameraCreatesAndUpdatesSingleCameraEntity) {
  container::gpu::CameraData first{};
  first.cameraWorldPosition = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);

  container::ecs::World world;
  const auto firstEntity = world.setActiveCamera(first, 0.2f, 200.0f);

  ASSERT_NE(world.activeCamera(), nullptr);
  EXPECT_TRUE(world.hasActiveCamera());
  EXPECT_EQ(world.activeCamera()->data.cameraWorldPosition,
            glm::vec4(1.0f, 2.0f, 3.0f, 1.0f));
  EXPECT_FLOAT_EQ(world.activeCamera()->nearPlane, 0.2f);
  EXPECT_FLOAT_EQ(world.activeCamera()->farPlane, 200.0f);

  container::gpu::CameraData second{};
  second.cameraWorldPosition = glm::vec4(4.0f, 5.0f, 6.0f, 1.0f);
  const auto secondEntity = world.setActiveCamera(second, 0.05f, 500.0f);

  EXPECT_EQ(firstEntity, secondEntity);
  ASSERT_NE(world.activeCamera(), nullptr);
  EXPECT_EQ(world.activeCamera()->data.cameraWorldPosition,
            glm::vec4(4.0f, 5.0f, 6.0f, 1.0f));
  EXPECT_FLOAT_EQ(world.activeCamera()->nearPlane, 0.05f);
  EXPECT_FLOAT_EQ(world.activeCamera()->farPlane, 500.0f);
  EXPECT_EQ(world.entityCount(), 1u);
}

TEST(ECS_World, ClearActiveCameraRemovesCameraEntity) {
  container::ecs::World world;
  (void)world.setActiveCamera(container::gpu::CameraData{});
  ASSERT_TRUE(world.hasActiveCamera());

  world.clearActiveCamera();

  EXPECT_FALSE(world.hasActiveCamera());
  EXPECT_EQ(world.entityCount(), 0u);
}

// ============================================================================
// World — clear
// ============================================================================

TEST(ECS_World, ClearRemovesAllEntities) {
  container::scene::SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0, true, 0);

  container::ecs::World world;
  world.syncFromSceneGraph(graph);
  std::vector<container::gpu::PointLightData> lights{
      container::gpu::PointLightData{}};
  world.replacePointLights(lights);
  (void)world.setActiveCamera(container::gpu::CameraData{});
  EXPECT_GT(world.entityCount(), 0u);

  world.clear();
  EXPECT_EQ(world.entityCount(), 0u);
  EXPECT_EQ(world.renderableCount(), 0u);
  EXPECT_EQ(world.pointLightCount(), 0u);
  EXPECT_FALSE(world.hasActiveCamera());
}

} // namespace
