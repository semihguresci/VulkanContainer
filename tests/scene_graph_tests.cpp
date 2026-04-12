// scene_graph_tests.cpp
//
// CPU-only unit tests for the SceneGraph and SceneNode types.
// No GPU, no window, and no Vulkan device are required.

#include <gtest/gtest.h>

#include "Container/utility/SceneGraph.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <limits>

namespace {

using container::scene::SceneGraph;
using container::scene::SceneNode;

// ============================================================================
// SceneNode defaults
// ============================================================================

TEST(SceneNode, DefaultsAreIdentityAndInvalid) {
  SceneNode node;
  EXPECT_EQ(node.localTransform, glm::mat4(1.0f));
  EXPECT_EQ(node.worldTransform, glm::mat4(1.0f));
  EXPECT_EQ(node.parent, SceneGraph::kInvalidNode);
  EXPECT_EQ(node.materialIndex, 0u);
  EXPECT_EQ(node.primitiveIndex, SceneGraph::kInvalidNode);
  EXPECT_FALSE(node.renderable);
  EXPECT_TRUE(node.children.empty());
}

// ============================================================================
// SceneGraph — empty state
// ============================================================================

TEST(SceneGraph, DefaultConstructionIsEmpty) {
  SceneGraph graph;
  EXPECT_EQ(graph.nodeCount(), 0u);
  EXPECT_TRUE(graph.renderableNodes().empty());
}

TEST(SceneGraph, GetNodeOnEmptyReturnsNull) {
  SceneGraph graph;
  EXPECT_EQ(graph.getNode(0), nullptr);
  EXPECT_EQ(graph.getNode(SceneGraph::kInvalidNode), nullptr);
}

// ============================================================================
// SceneGraph — createNode
// ============================================================================

TEST(SceneGraph, CreateNodeIncreasesCount) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0);
  EXPECT_EQ(graph.nodeCount(), 1u);

  graph.createNode(glm::mat4(1.0f), 0);
  EXPECT_EQ(graph.nodeCount(), 2u);
}

TEST(SceneGraph, CreateNodeReturnsSequentialIndices) {
  SceneGraph graph;
  EXPECT_EQ(graph.createNode(glm::mat4(1.0f), 0), 0u);
  EXPECT_EQ(graph.createNode(glm::mat4(1.0f), 0), 1u);
  EXPECT_EQ(graph.createNode(glm::mat4(1.0f), 0), 2u);
}

TEST(SceneGraph, CreateNodePreservesTransformAndMaterial) {
  SceneGraph graph;
  const glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(3, 4, 5));
  const uint32_t idx = graph.createNode(t, 7, false, 42);

  const auto* node = graph.getNode(idx);
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->localTransform, t);
  EXPECT_EQ(node->materialIndex, 7u);
  EXPECT_EQ(node->primitiveIndex, 42u);
  EXPECT_FALSE(node->renderable);
}

TEST(SceneGraph, CreateRenderableNodeRegistersInList) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0, true, 0);
  EXPECT_EQ(graph.renderableNodes().size(), 1u);
}

TEST(SceneGraph, CreateNonRenderableNodeDoesNotRegister) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0, false);
  EXPECT_TRUE(graph.renderableNodes().empty());
}

// ============================================================================
// SceneGraph — setRenderable
// ============================================================================

TEST(SceneGraph, SetRenderableTrueRegistersNode) {
  SceneGraph graph;
  const uint32_t idx = graph.createNode(glm::mat4(1.0f), 0, false);
  EXPECT_TRUE(graph.renderableNodes().empty());

  graph.setRenderable(idx, true);
  EXPECT_EQ(graph.renderableNodes().size(), 1u);
  EXPECT_EQ(graph.renderableNodes()[0], idx);
}

TEST(SceneGraph, SetRenderableFalseUnregistersNode) {
  SceneGraph graph;
  const uint32_t idx = graph.createNode(glm::mat4(1.0f), 0, true, 0);
  EXPECT_EQ(graph.renderableNodes().size(), 1u);

  graph.setRenderable(idx, false);
  EXPECT_TRUE(graph.renderableNodes().empty());
}

TEST(SceneGraph, SetRenderableTwiceDoesNotDuplicate) {
  SceneGraph graph;
  const uint32_t idx = graph.createNode(glm::mat4(1.0f), 0, true, 0);
  graph.setRenderable(idx, true);  // already renderable
  EXPECT_EQ(graph.renderableNodes().size(), 1u);
}

// ============================================================================
// SceneGraph — parent/child
// ============================================================================

TEST(SceneGraph, SetParentEstablishesRelationship) {
  SceneGraph graph;
  const uint32_t parent = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t child  = graph.createNode(glm::mat4(1.0f), 0);

  graph.setParent(child, parent);

  const auto* childNode  = graph.getNode(child);
  const auto* parentNode = graph.getNode(parent);
  ASSERT_NE(childNode, nullptr);
  ASSERT_NE(parentNode, nullptr);
  EXPECT_EQ(childNode->parent, parent);
  ASSERT_EQ(parentNode->children.size(), 1u);
  EXPECT_EQ(parentNode->children[0], child);
}

TEST(SceneGraph, SetParentNulloptDetachesChild) {
  SceneGraph graph;
  const uint32_t parent = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t child  = graph.createNode(glm::mat4(1.0f), 0);

  graph.setParent(child, parent);
  graph.setParent(child, std::nullopt);

  const auto* childNode  = graph.getNode(child);
  const auto* parentNode = graph.getNode(parent);
  ASSERT_NE(childNode, nullptr);
  ASSERT_NE(parentNode, nullptr);
  EXPECT_EQ(childNode->parent, SceneGraph::kInvalidNode);
  EXPECT_TRUE(parentNode->children.empty());
}

TEST(SceneGraph, ReparentMovesChild) {
  SceneGraph graph;
  const uint32_t p1 = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t p2 = graph.createNode(glm::mat4(1.0f), 0);
  const uint32_t c  = graph.createNode(glm::mat4(1.0f), 0);

  graph.setParent(c, p1);
  graph.setParent(c, p2);

  EXPECT_TRUE(graph.getNode(p1)->children.empty());
  ASSERT_EQ(graph.getNode(p2)->children.size(), 1u);
  EXPECT_EQ(graph.getNode(p2)->children[0], c);
  EXPECT_EQ(graph.getNode(c)->parent, p2);
}

// ============================================================================
// SceneGraph — transforms
// ============================================================================

TEST(SceneGraph, SetLocalTransformUpdatesNode) {
  SceneGraph graph;
  const uint32_t idx = graph.createNode(glm::mat4(1.0f), 0);
  const glm::mat4 t = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));

  graph.setLocalTransform(idx, t);
  EXPECT_EQ(graph.getNode(idx)->localTransform, t);
}

TEST(SceneGraph, UpdateWorldTransformsPropagatesParentTransform) {
  SceneGraph graph;
  const glm::mat4 parentT = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));
  const glm::mat4 childT  = glm::translate(glm::mat4(1.0f), glm::vec3(0, 5, 0));

  const uint32_t parent = graph.createNode(parentT, 0);
  const uint32_t child  = graph.createNode(childT, 0);
  graph.setParent(child, parent);

  graph.updateWorldTransforms();

  // Parent world = parentT (no parent above it)
  EXPECT_EQ(graph.getNode(parent)->worldTransform, parentT);
  // Child world = parentT * childT
  const glm::mat4 expected = parentT * childT;
  const auto& actual = graph.getNode(child)->worldTransform;
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      EXPECT_NEAR(actual[c][r], expected[c][r], 1e-5f)
          << "mismatch at [" << c << "][" << r << "]";
}

TEST(SceneGraph, RootNodeWorldTransformEqualsLocal) {
  SceneGraph graph;
  const glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3));
  const uint32_t idx = graph.createNode(t, 0);

  graph.updateWorldTransforms();

  EXPECT_EQ(graph.getNode(idx)->worldTransform, t);
}

TEST(SceneGraph, ThreeLevelHierarchyPropagatesCorrectly) {
  SceneGraph graph;
  const glm::mat4 t1 = glm::translate(glm::mat4(1.0f), glm::vec3(1, 0, 0));
  const glm::mat4 t2 = glm::translate(glm::mat4(1.0f), glm::vec3(0, 2, 0));
  const glm::mat4 t3 = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 3));

  const uint32_t n1 = graph.createNode(t1, 0);
  const uint32_t n2 = graph.createNode(t2, 0);
  const uint32_t n3 = graph.createNode(t3, 0);
  graph.setParent(n2, n1);
  graph.setParent(n3, n2);

  graph.updateWorldTransforms();

  const glm::mat4 expected3 = t1 * t2 * t3;
  const auto& actual3 = graph.getNode(n3)->worldTransform;
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      EXPECT_NEAR(actual3[c][r], expected3[c][r], 1e-5f);
}

// ============================================================================
// SceneGraph — getNode bounds
// ============================================================================

TEST(SceneGraph, GetNodeOutOfBoundsReturnsNull) {
  SceneGraph graph;
  graph.createNode(glm::mat4(1.0f), 0);

  EXPECT_EQ(graph.getNode(1), nullptr);
  EXPECT_EQ(graph.getNode(999), nullptr);
}

TEST(SceneGraph, GetNodeConstAndMutableAgree) {
  SceneGraph graph;
  const uint32_t idx = graph.createNode(glm::mat4(1.0f), 5);

  const auto& constGraph = graph;
  EXPECT_EQ(constGraph.getNode(idx)->materialIndex, 5u);
  EXPECT_EQ(graph.getNode(idx)->materialIndex, 5u);
}

}  // namespace
