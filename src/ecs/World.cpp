#include "Container/ecs/World.h"
#include "Container/utility/SceneGraph.h"

#include <vector>

namespace container::ecs {

void World::syncFromSceneGraph(const container::scene::SceneGraph &graph) {
  clearRenderables();

  for (const uint32_t nodeIndex : graph.renderableNodes()) {
    const auto *node = graph.getNode(nodeIndex);
    if (!node)
      continue;

    const auto entity = registry_.create();
    registry_.emplace<TransformComponent>(entity, node->localTransform,
                                          node->worldTransform);
    registry_.emplace<MeshComponent>(entity, node->primitiveIndex);
    registry_.emplace<MaterialComponent>(entity, node->materialIndex);
    registry_.emplace<RenderableTag>(entity);
    registry_.emplace<SceneNodeRef>(entity, nodeIndex);
  }
}

void World::clearRenderables() {
  auto view = registry_.view<const RenderableTag>();

  std::vector<entt::entity> entities;
  entities.reserve(view.size());
  for (const auto entity : view) {
    entities.push_back(entity);
  }

  for (const auto entity : entities) {
    registry_.destroy(entity);
  }
}

void World::forEachRenderable(const RenderableVisitor &visitor) const {
  auto view = registry_.view<const TransformComponent, const MeshComponent,
                             const MaterialComponent, const RenderableTag>();
  for (auto [entity, transform, mesh, material] : view.each()) {
    visitor(transform, mesh, material);
  }
}

entt::entity
World::createPointLight(const container::gpu::PointLightData &data) {
  const auto entity = registry_.create();
  registry_.emplace<LightComponent>(entity, LightComponent{data});
  registry_.emplace<LightTag>(entity);
  return entity;
}

void World::replacePointLights(
    std::span<const container::gpu::PointLightData> lights) {
  clearPointLights();
  for (const auto &light : lights) {
    (void)createPointLight(light);
  }
}

void World::forEachPointLight(const LightVisitor &visitor) const {
  auto view = registry_.view<const LightComponent, const LightTag>();
  for (auto [entity, light] : view.each()) {
    visitor(light);
  }
}

void World::clearPointLights() {
  auto view = registry_.view<const LightTag>();

  std::vector<entt::entity> entities;
  entities.reserve(view.size());
  for (const auto entity : view) {
    entities.push_back(entity);
  }

  for (const auto entity : entities) {
    registry_.destroy(entity);
  }
}

entt::entity World::setActiveCamera(const container::gpu::CameraData &data,
                                    float nearPlane, float farPlane) {
  if (activeCameraEntity_ == entt::null ||
      !registry_.valid(activeCameraEntity_)) {
    activeCameraEntity_ = registry_.create();
    registry_.emplace<CameraTag>(activeCameraEntity_);
  }

  registry_.emplace_or_replace<CameraComponent>(
      activeCameraEntity_, CameraComponent{data, nearPlane, farPlane});
  return activeCameraEntity_;
}

const CameraComponent *World::activeCamera() const {
  if (activeCameraEntity_ == entt::null ||
      !registry_.valid(activeCameraEntity_)) {
    return nullptr;
  }
  return registry_.try_get<CameraComponent>(activeCameraEntity_);
}

bool World::hasActiveCamera() const { return activeCamera() != nullptr; }

void World::clearActiveCamera() {
  if (activeCameraEntity_ != entt::null &&
      registry_.valid(activeCameraEntity_)) {
    registry_.destroy(activeCameraEntity_);
  }
  activeCameraEntity_ = entt::null;
}

uint32_t World::renderableCount() const {
  return static_cast<uint32_t>(registry_.view<const RenderableTag>().size());
}

uint32_t World::pointLightCount() const {
  return static_cast<uint32_t>(registry_.view<const LightTag>().size());
}

uint32_t World::entityCount() const {
  // In EnTT 3.14, the entity storage's each() iterates alive entities.
  const auto *storage = registry_.storage<entt::entity>();
  if (!storage)
    return 0;
  uint32_t count = 0;
  for ([[maybe_unused]] auto &&entry : storage->each()) {
    ++count;
  }
  return count;
}

void World::clear() {
  registry_.clear();
  activeCameraEntity_ = entt::null;
}

} // namespace container::ecs
