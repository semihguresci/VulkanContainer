#include <Container/utility/SceneGraph.h>

#include <algorithm>

namespace container::scene {

uint32_t SceneGraph::createNode(const glm::mat4& localTransform,
                                uint32_t materialIndex, bool renderable,
                                uint32_t primitiveIndex) {
  const uint32_t nodeIndex = static_cast<uint32_t>(nodes_.size());
  nodes_.push_back({localTransform, localTransform, kInvalidNode, materialIndex,
                    primitiveIndex, renderable, {}});
  roots_.push_back(nodeIndex);
  if (renderable) {
    renderableNodes_.push_back(nodeIndex);
  }
  ++revision_;
  return nodeIndex;
}

void SceneGraph::setParent(uint32_t child, std::optional<uint32_t> parentIndex) {
  if (child >= nodes_.size()) return;

  if (parentIndex.has_value()) {
    const uint32_t newParent = parentIndex.value();
    if (newParent >= nodes_.size()) {
      return;
    } else if (newParent == child || isDescendant(child, newParent)) {
      return;
    }
  }

  SceneNode& childNode = nodes_[child];
  const uint32_t newParent =
      parentIndex.has_value() ? parentIndex.value() : kInvalidNode;
  if (childNode.parent == newParent) {
    return;
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
