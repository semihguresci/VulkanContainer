#include "Container/ecs/World.h"
#include "Container/utility/SceneGraph.h"

namespace container::ecs {

void World::syncFromSceneGraph(const container::scene::SceneGraph& graph) {
  registry_.clear();

  for (const uint32_t nodeIndex : graph.renderableNodes()) {
    const auto* node = graph.getNode(nodeIndex);
    if (!node) continue;

    const auto entity = registry_.create();
    registry_.emplace<TransformComponent>(
        entity, node->localTransform, node->worldTransform);
    registry_.emplace<MeshComponent>(entity, node->primitiveIndex);
    registry_.emplace<MaterialComponent>(entity, node->materialIndex);
    registry_.emplace<RenderableTag>(entity);
    registry_.emplace<SceneNodeRef>(entity, nodeIndex);
  }
}

void World::forEachRenderable(const RenderableVisitor& visitor) const {
  auto view = registry_.view<const TransformComponent,
                             const MeshComponent,
                             const MaterialComponent,
                             const RenderableTag>();
  for (auto [entity, transform, mesh, material] : view.each()) {
    visitor(transform, mesh, material);
  }
}

uint32_t World::renderableCount() const {
  return static_cast<uint32_t>(
      registry_.view<const RenderableTag>().size());
}

uint32_t World::entityCount() const {
  // In EnTT 3.14, the entity storage's each() iterates alive entities.
  const auto* storage = registry_.storage<entt::entity>();
  if (!storage) return 0;
  uint32_t count = 0;
  for ([[maybe_unused]] auto&& entry : storage->each()) {
    ++count;
  }
  return count;
}

void World::clear() { registry_.clear(); }

}  // namespace container::ecs
