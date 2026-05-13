#include <Container/utility/SceneGraph.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace container::scene {

namespace {

bool matrixFinite(const glm::mat4& matrix) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!std::isfinite(matrix[column][row])) {
        return false;
      }
    }
  }
  return true;
}

bool transformBasisInvertible(const glm::mat4& matrix) {
  if (!matrixFinite(matrix)) {
    return false;
  }
  float maxColumnLength = 0.0f;
  for (int column = 0; column < 3; ++column) {
    const glm::vec3 basisColumn(matrix[column]);
    maxColumnLength = std::max(maxColumnLength, glm::length(basisColumn));
  }
  if (maxColumnLength <= 0.0f) {
    return false;
  }

  const float determinant = glm::determinant(glm::mat3(matrix));
  if (!std::isfinite(determinant)) {
    return false;
  }

  const float relativeThreshold = std::numeric_limits<float>::epsilon() *
                                  maxColumnLength * maxColumnLength *
                                  maxColumnLength * 64.0f;
  return std::abs(determinant) > relativeThreshold;
}

}  // namespace

uint32_t SceneGraph::createNode(const glm::mat4& localTransform,
                                uint32_t materialIndex, bool renderable,
                                uint32_t primitiveIndex, std::string name) {
  const uint32_t nodeIndex = static_cast<uint32_t>(nodes_.size());
  SceneNode node{};
  node.localTransform = localTransform;
  node.worldTransform = localTransform;
  node.parent = kInvalidNode;
  node.materialIndex = materialIndex;
  node.primitiveIndex = primitiveIndex;
  node.renderable = renderable;
  node.visible = true;
  node.name = std::move(name);
  nodes_.push_back(std::move(node));
  roots_.push_back(nodeIndex);
  if (renderable) {
    renderableNodes_.push_back(nodeIndex);
  }
  ++revision_;
  return nodeIndex;
}

bool SceneGraph::canSetParent(uint32_t child,
                              std::optional<uint32_t> parentIndex) const {
  if (child >= nodes_.size()) {
    return false;
  }
  if (!parentIndex.has_value()) {
    return true;
  }
  const uint32_t newParent = parentIndex.value();
  return newParent < nodes_.size() && newParent != child &&
         !isDescendant(child, newParent);
}

bool SceneGraph::setParent(uint32_t child, std::optional<uint32_t> parentIndex) {
  if (!canSetParent(child, parentIndex)) return false;

  SceneNode& childNode = nodes_[child];
  const uint32_t newParent =
      parentIndex.has_value() ? parentIndex.value() : kInvalidNode;
  if (childNode.parent == newParent) {
    return false;
  }

  if (childNode.parent != kInvalidNode) {
    auto& siblings = nodes_[childNode.parent].children;
    std::erase(siblings, child);
  } else {
    std::erase(roots_, child);
  }

  if (parentIndex.has_value() && parentIndex.value() < nodes_.size()) {
    childNode.parent = parentIndex.value();
    nodes_[childNode.parent].children.push_back(child);
  } else {
    childNode.parent = kInvalidNode;
    roots_.push_back(child);
  }
  ++revision_;
  return true;
}

bool SceneGraph::setParentPreserveWorldTransform(
    uint32_t child, std::optional<uint32_t> parentIndex) {
  if (!canSetParent(child, parentIndex)) return false;

  const uint32_t newParent =
      parentIndex.has_value() ? parentIndex.value() : kInvalidNode;
  if (nodes_[child].parent == newParent) {
    return false;
  }

  updateWorldTransforms();
  const glm::mat4 originalWorld = nodes_[child].worldTransform;
  const glm::mat4 parentWorld =
      parentIndex.has_value() ? nodes_[parentIndex.value()].worldTransform
                              : glm::mat4(1.0f);
  if (!transformBasisInvertible(parentWorld)) {
    return false;
  }
  const glm::mat4 newLocal = glm::inverse(parentWorld) * originalWorld;
  if (!matrixFinite(newLocal)) {
    return false;
  }
  const bool changed = setParent(child, parentIndex);
  if (!changed) {
    return false;
  }
  nodes_[child].localTransform = newLocal;
  updateWorldTransforms();
  return true;
}

bool SceneGraph::isDescendant(uint32_t ancestor, uint32_t candidate) const {
  if (ancestor >= nodes_.size() || candidate >= nodes_.size()) return false;

  uint32_t current = candidate;
  while (current != kInvalidNode && current < nodes_.size()) {
    if (current == ancestor) return true;
    current = nodes_[current].parent;
  }
  return false;
}

void SceneGraph::setLocalTransform(uint32_t nodeIndex,
                                   const glm::mat4& localTransform) {
  if (nodeIndex >= nodes_.size()) return;
  if (nodes_[nodeIndex].localTransform == localTransform) return;
  nodes_[nodeIndex].localTransform = localTransform;
  ++revision_;
}

void SceneGraph::setRenderable(uint32_t nodeIndex, bool renderable) {
  if (nodeIndex >= nodes_.size()) return;
  if (nodes_[nodeIndex].renderable == renderable) return;
  nodes_[nodeIndex].renderable = renderable;
  if (renderable) {
    registerRenderable(nodeIndex);
  } else {
    unregisterRenderable(nodeIndex);
  }
  ++revision_;
}

void SceneGraph::setVisible(uint32_t nodeIndex, bool visible) {
  if (nodeIndex >= nodes_.size()) return;
  if (nodes_[nodeIndex].visible == visible) return;
  nodes_[nodeIndex].visible = visible;
  ++revision_;
}

bool SceneGraph::isNodeEffectivelyVisible(uint32_t nodeIndex) const {
  if (nodeIndex >= nodes_.size()) return false;

  uint32_t current = nodeIndex;
  while (current != kInvalidNode && current < nodes_.size()) {
    const SceneNode& node = nodes_[current];
    if (!node.visible) return false;
    current = node.parent;
  }
  return true;
}

void SceneGraph::updateWorldTransforms() {
  for (uint32_t root : roots_) {
    updateWorldRecursive(root, glm::mat4(1.0f));
  }
}

const SceneNode* SceneGraph::getNode(uint32_t nodeIndex) const {
  if (nodeIndex >= nodes_.size()) return nullptr;
  return &nodes_[nodeIndex];
}

SceneNode* SceneGraph::getNode(uint32_t nodeIndex) {
  if (nodeIndex >= nodes_.size()) return nullptr;
  return &nodes_[nodeIndex];
}

void SceneGraph::updateWorldRecursive(uint32_t nodeIndex,
                                      const glm::mat4& parentTransform) {
  SceneNode& node = nodes_[nodeIndex];
  node.worldTransform = parentTransform * node.localTransform;
  for (uint32_t child : node.children) {
    updateWorldRecursive(child, node.worldTransform);
  }
}

void SceneGraph::registerRenderable(uint32_t nodeIndex) {
  if (std::ranges::contains(renderableNodes_, nodeIndex)) {
    return;
  }
  renderableNodes_.push_back(nodeIndex);
}

void SceneGraph::unregisterRenderable(uint32_t nodeIndex) {
  std::erase(renderableNodes_, nodeIndex);
}

}  // namespace container::scene
