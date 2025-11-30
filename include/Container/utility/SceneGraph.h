#pragma once

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace utility::scene {

struct SceneNode {
  glm::mat4 localTransform{1.0f};
  glm::mat4 worldTransform{1.0f};
  uint32_t parent = std::numeric_limits<uint32_t>::max();
  uint32_t materialIndex = 0;
  bool renderable = false;
  std::vector<uint32_t> children{};
};

class SceneGraph {
 public:
  static constexpr uint32_t kInvalidNode =
      std::numeric_limits<uint32_t>::max();

  uint32_t createNode(const glm::mat4& localTransform, uint32_t materialIndex,
                      bool renderable = false);
  void setParent(uint32_t child, std::optional<uint32_t> parentIndex);
  void setLocalTransform(uint32_t nodeIndex, const glm::mat4& localTransform);
  void setRenderable(uint32_t nodeIndex, bool renderable);
  void updateWorldTransforms();

  const std::vector<uint32_t>& renderableNodes() const { return renderableNodes_; }
  const SceneNode* getNode(uint32_t nodeIndex) const;
  SceneNode* getNode(uint32_t nodeIndex);
  size_t nodeCount() const { return nodes_.size(); }

 private:
  void updateWorldRecursive(uint32_t nodeIndex, const glm::mat4& parentTransform);
  void registerRenderable(uint32_t nodeIndex);
  void unregisterRenderable(uint32_t nodeIndex);

  std::vector<SceneNode> nodes_{};
  std::vector<uint32_t> roots_{};
  std::vector<uint32_t> renderableNodes_{};
};

}  // namespace utility::scene
