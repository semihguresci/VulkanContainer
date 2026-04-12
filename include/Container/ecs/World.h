#pragma once

#include "Container/ecs/Components.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>

namespace container::scene {
class SceneGraph;
}

namespace container::ecs {

// Thin wrapper around entt::registry providing typed helpers for the
// VulkanContainer rendering pipeline.
//
// The World does NOT own or replace the SceneGraph.  Instead it provides
// a mirror: syncFromSceneGraph() reads the graph and creates/updates
// entities so that ECS views can be used for draw-call extraction.
class World {
 public:
  World() = default;

  // Rebuild the registry from the current SceneGraph state.
  // Clears all existing entities and re-creates one entity per renderable
  // node with TransformComponent, MeshComponent, MaterialComponent,
  // RenderableTag, and SceneNodeRef.
  void syncFromSceneGraph(const container::scene::SceneGraph& graph);

  // Iterate all renderable entities and invoke the callback with each
  // entity's components.  The callback receives transform, mesh, and
  // material components in a cache-friendly order via an EnTT view.
  using RenderableVisitor = std::function<void(
      const TransformComponent&, const MeshComponent&, const MaterialComponent&)>;
  void forEachRenderable(const RenderableVisitor& visitor) const;

  // Number of entities that carry the RenderableTag.
  [[nodiscard]] uint32_t renderableCount() const;

  // Number of total entities in the registry.
  [[nodiscard]] uint32_t entityCount() const;

  // Direct access for advanced queries.
  [[nodiscard]] entt::registry&       registry()       { return registry_; }
  [[nodiscard]] const entt::registry& registry() const { return registry_; }

  // Remove all entities and components.
  void clear();

 private:
  entt::registry registry_;
};

}  // namespace container::ecs
