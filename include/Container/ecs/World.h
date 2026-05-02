#pragma once

#include "Container/ecs/Components.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <span>

namespace container::scene {
class SceneGraph;
}

namespace container::ecs {

// Thin wrapper around entt::registry providing typed helpers for the
// VulkanSceneRenderer rendering pipeline.
//
// The World does NOT own or replace the SceneGraph.  Instead it provides
// a mirror: syncFromSceneGraph() reads the graph and creates/updates
// entities so that ECS views can be used for draw-call extraction.
class World {
public:
  World() = default;

  // Rebuild renderable entities from the current SceneGraph state.
  // Re-creates one entity per renderable node with TransformComponent,
  // MeshComponent, MaterialComponent, RenderableTag, and SceneNodeRef.
  // Light and camera entities are left intact.
  void syncFromSceneGraph(const container::scene::SceneGraph &graph);

  // Iterate all renderable entities and invoke the callback with each
  // entity's components.  The callback receives transform, mesh, and
  // material components in a cache-friendly order via an EnTT view.
  using RenderableVisitor =
      std::function<void(const TransformComponent &, const MeshComponent &,
                         const MaterialComponent &)>;
  void forEachRenderable(const RenderableVisitor &visitor) const;

  using RenderableWithNodeVisitor =
      std::function<void(const TransformComponent &, const MeshComponent &,
                         const MaterialComponent &, const SceneNodeRef &)>;
  void forEachRenderableWithNode(const RenderableWithNodeVisitor &visitor) const;

  // Point-light entity helpers used by lighting systems.
  using LightVisitor = std::function<void(const LightComponent &)>;
  [[nodiscard]] entt::entity
  createPointLight(const container::gpu::PointLightData &data);
  void
  replacePointLights(std::span<const container::gpu::PointLightData> lights);
  void forEachPointLight(const LightVisitor &visitor) const;
  void clearPointLights();

  // Active camera entity helpers.
  [[nodiscard]] entt::entity
  setActiveCamera(const container::gpu::CameraData &data,
                  float nearPlane = 0.1f, float farPlane = 100.0f);
  [[nodiscard]] const CameraComponent *activeCamera() const;
  [[nodiscard]] bool hasActiveCamera() const;
  void clearActiveCamera();

  // Number of entities that carry the RenderableTag.
  [[nodiscard]] uint32_t renderableCount() const;

  // Number of entities that carry the LightTag.
  [[nodiscard]] uint32_t pointLightCount() const;

  // Number of total entities in the registry.
  [[nodiscard]] uint32_t entityCount() const;

  // Direct access for advanced queries.
  [[nodiscard]] entt::registry &registry() { return registry_; }
  [[nodiscard]] const entt::registry &registry() const { return registry_; }

  // Remove all entities and components.
  void clear();

private:
  void clearRenderables();

  entt::registry registry_;
  entt::entity activeCameraEntity_{entt::null};
};

} // namespace container::ecs
