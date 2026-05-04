#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

namespace container::scene {

enum class SceneProviderKind {
  Mesh,
  Bim,
  GaussianSplatting,
  RadianceField,
};

struct SceneProviderId {
  std::string value{};

  [[nodiscard]] bool operator==(const SceneProviderId& other) const {
    return value == other.value;
  }
};

struct SceneProviderRevision {
  uint64_t geometry{0};
  uint64_t materials{0};
  uint64_t instances{0};
};

struct SceneProviderBounds {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  bool valid{false};
};

struct MeshSceneAsset {
  std::size_t primitiveCount{0};
  std::size_t materialCount{0};
  std::size_t instanceCount{0};
  SceneProviderBounds bounds{};
};

struct BimSceneAsset {
  std::size_t elementCount{0};
  std::size_t meshPrimitiveCount{0};
  std::size_t nativePointRangeCount{0};
  std::size_t nativeCurveRangeCount{0};
  SceneProviderBounds bounds{};
};

struct GaussianSplatSceneAsset {
  std::size_t splatCount{0};
  uint32_t sphericalHarmonicCoefficientCount{0};
  SceneProviderBounds bounds{};
};

struct RadianceFieldSceneAsset {
  uint32_t fieldCount{0};
  bool hasOccupancyData{false};
  SceneProviderBounds bounds{};
};

struct SceneProviderSnapshot {
  SceneProviderKind kind{SceneProviderKind::Mesh};
  SceneProviderId id{};
  std::string displayName{};
  SceneProviderRevision revision{};
  SceneProviderBounds bounds{};
  std::size_t elementCount{0};
};

class IRenderSceneProvider {
 public:
  virtual ~IRenderSceneProvider() = default;

  [[nodiscard]] virtual SceneProviderSnapshot snapshot() const = 0;
};

class SceneProviderRegistry {
 public:
  void registerProvider(const IRenderSceneProvider& provider) {
    const SceneProviderSnapshot providerSnapshot = provider.snapshot();
    if (providerSnapshot.id.value.empty()) {
      throw std::invalid_argument("scene provider id must not be empty");
    }
    if (find(providerSnapshot.id) != nullptr) {
      throw std::invalid_argument("scene provider is already registered");
    }
    providers_.push_back(&provider);
  }

  [[nodiscard]] const IRenderSceneProvider* find(
      const SceneProviderId& id) const {
    for (const IRenderSceneProvider* provider : providers_) {
      if (provider != nullptr && provider->snapshot().id == id) {
        return provider;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::vector<const IRenderSceneProvider*> providersForKind(
      SceneProviderKind kind) const {
    std::vector<const IRenderSceneProvider*> matches;
    for (const IRenderSceneProvider* provider : providers_) {
      if (provider != nullptr && provider->snapshot().kind == kind) {
        matches.push_back(provider);
      }
    }
    return matches;
  }

  [[nodiscard]] std::vector<SceneProviderSnapshot> snapshots() const {
    std::vector<SceneProviderSnapshot> result;
    result.reserve(providers_.size());
    for (const IRenderSceneProvider* provider : providers_) {
      if (provider != nullptr) {
        result.push_back(provider->snapshot());
      }
    }
    return result;
  }

  [[nodiscard]] std::span<const IRenderSceneProvider* const> providers() const {
    return {providers_.data(), providers_.size()};
  }

  void clear() { providers_.clear(); }

 private:
  std::vector<const IRenderSceneProvider*> providers_{};
};

}  // namespace container::scene
