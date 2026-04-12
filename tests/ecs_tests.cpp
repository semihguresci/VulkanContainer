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
//   RenderableTag            (Components.h)
//   SceneNodeRef             (Components.h)
//   World                    (World.h)

#include <gtest/gtest.h>

#include "Container/ecs/Components.h"
#include "Container/ecs/World.h"
#include "Container/utility/SceneGraph.h"

#include <glm/gtc/matrix_transform.hpp>

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
  const glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3));
  graph.createNode(transform, /*materialIndex=*/0, /*renderable=*/true,
                   /*primitiveIndex=*/0);
  graph.updateWorldTransforms();

  container::ecs::World world;
  world.syncFromSceneGraph(graph);

  std::vector<glm::mat4> worldTransforms;
  world.forEachRenderable(
      [&](const container::ecs::TransformComponent& tc,
          const container::ecs::MeshComponent&,
          const container::ecs::MaterialComponent&) {
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
  uint32_t materialIdx  = 0;
  world.forEachRenderable(
      [&](const container::ecs::TransformComponent&,
          const container::ecs::MeshComponent& mesh,
          const container::ecs::MaterialComponent& mat) {
        primitiveIdx = mesh.primitiveIndex;
        materialIdx  = mat.materialIndex;
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

// ============================================================================
// World — forEachRenderable
// ============================================================================

TEST(ECS_World, ForEachRenderableOnEmptyIsNoOp) {
  container::ecs::World world;
  int callCount = 0;
  world.forEachRenderable(
      [&](const container::ecs::TransformComponent&,
          const container::ecs::MeshComponent&,
          const container::ecs::MaterialComponent&) { ++callCount; });
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
      [&](const container::ecs::TransformComponent&,
          const container::ecs::MeshComponent&,
          const container::ecs::MaterialComponent&) { ++callCount; });
  EXPECT_EQ(callCount, 3);
}

// ============================================================================
// World — clear
// ============================================================================

TEST(ECS_World, ClearRemovesAllEntities) {
  container::scene::SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0, true, 0);

  container::ecs::World world;
  world.syncFromSceneGraph(graph);
  EXPECT_GT(world.entityCount(), 0u);

  world.clear();
  EXPECT_EQ(world.entityCount(), 0u);
  EXPECT_EQ(world.renderableCount(), 0u);
}

}  // namespace
