#include <Container/utility/SceneGraph.h>

#include <algorithm>

namespace utility::scene {

uint32_t SceneGraph::createNode(const glm::mat4& localTransform,
                                uint32_t materialIndex, bool renderable) {
  const uint32_t nodeIndex = static_cast<uint32_t>(nodes_.size());
  nodes_.push_back({localTransform, localTransform, kInvalidNode, materialIndex,
                    renderable, {}});
  roots_.push_back(nodeIndex);
  if (renderable) {
    renderableNodes_.push_back(nodeIndex);
  }
  return nodeIndex;
}

void SceneGraph::setParent(uint32_t child, std::optional<uint32_t> parentIndex) {
  if (child >= nodes_.size()) return;

  SceneNode& childNode = nodes_[child];
  if (childNode.parent != kInvalidNode) {
    auto& siblings = nodes_[childNode.parent].children;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), child),
                   siblings.end());
  } else {
    roots_.erase(std::remove(roots_.begin(), roots_.end(), child), roots_.end());
  }

  if (parentIndex.has_value() && parentIndex.value() < nodes_.size()) {
    childNode.parent = parentIndex.value();
    nodes_[childNode.parent].children.push_back(child);
  } else {
    childNode.parent = kInvalidNode;
    roots_.push_back(child);
  }
}

void SceneGraph::setLocalTransform(uint32_t nodeIndex,
                                   const glm::mat4& localTransform) {
  if (nodeIndex >= nodes_.size()) return;
  nodes_[nodeIndex].localTransform = localTransform;
}

void SceneGraph::setRenderable(uint32_t nodeIndex, bool renderable) {
  if (nodeIndex >= nodes_.size()) return;
  nodes_[nodeIndex].renderable = renderable;
  if (renderable) {
    registerRenderable(nodeIndex);
  } else {
    unregisterRenderable(nodeIndex);
  }
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
  if (std::find(renderableNodes_.begin(), renderableNodes_.end(), nodeIndex) !=
      renderableNodes_.end()) {
    return;
  }
  renderableNodes_.push_back(nodeIndex);
}

void SceneGraph::unregisterRenderable(uint32_t nodeIndex) {
  renderableNodes_.erase(
      std::remove(renderableNodes_.begin(), renderableNodes_.end(), nodeIndex),
      renderableNodes_.end());
}

}  // namespace utility::scene
